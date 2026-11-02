// SPDX-License-Identifier: GPL-3.0-or-later
#include "dms/permission_provider.hpp"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace dms {
namespace {

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

}  // namespace

std::vector<std::string> ParseCsvRow(const std::string& row) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < row.size(); ++i) {
        const char ch = row[i];
        if (ch == '"') {
            if (quoted && i + 1 < row.size() && row[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == ',' && !quoted) {
            fields.push_back(Trim(std::move(field)));
            field.clear();
        } else {
            field.push_back(ch);
        }
    }
    if (quoted) {
        throw std::runtime_error("Malformed CSV row: unmatched quote");
    }
    fields.push_back(Trim(std::move(field)));
    return fields;
}

std::string NormalizeLogicalPath(const std::string& path) {
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos) {
        throw std::invalid_argument("Logical path must begin with '/'");
    }
    std::string normalized;
    normalized.reserve(path.size());
    bool previous_slash = false;
    for (const char ch : path) {
        if (ch == '\\') {
            throw std::invalid_argument("Backslashes are not valid logical path separators");
        }
        if (ch == '/') {
            if (!previous_slash) {
                normalized.push_back(ch);
            }
            previous_slash = true;
        } else {
            previous_slash = false;
            normalized.push_back(ch);
        }
    }
    if (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    std::size_t start = 1;
    while (start <= normalized.size()) {
        const auto end = normalized.find('/', start);
        const auto part = normalized.substr(start, end == std::string::npos
                                                       ? std::string::npos
                                                       : end - start);
        if (part == "." || part == "..") {
            throw std::invalid_argument("Dot segments are not valid logical paths");
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return normalized;
}

CsvPermissionProvider::CsvPermissionProvider(std::filesystem::path file)
    : file_(std::move(file)) {
    Reload();
}

void CsvPermissionProvider::Reload() {
    std::ifstream input(file_);
    if (!input) {
        throw std::runtime_error("Cannot open permissions file: " + file_.string());
    }

    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::vector<Rule>>> next;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        const auto fields = ParseCsvRow(line);
        if (line_number == 1 && !fields.empty() && fields[0] == "路径") {
            continue;
        }
        if (fields.size() != 8 && fields.size() != 9 && fields.size() != 10) {
            throw std::runtime_error("permissions.csv line " +
                                     std::to_string(line_number) +
                                     " must contain 8, 9, or 10 columns");
        }
        const auto bit = [&fields, line_number](std::size_t index) {
            if (fields[index] != "0" && fields[index] != "1") {
                throw std::runtime_error("Invalid permission bit on permissions.csv line " +
                                         std::to_string(line_number));
            }
            return fields[index] == "1";
        };
        const auto raw_path = fields[0];
        const bool directory = !raw_path.empty() && raw_path.back() == '/';
        Rule rule;
        rule.path = NormalizeLogicalPath(raw_path);
        rule.object = fields[1];
        rule.directory = directory;
        rule.read = bit(2);
        rule.write = bit(3);
        rule.remove = bit(4);
        rule.add_version = bit(5);
        rule.delete_version = bit(6);
        rule.upload = fields.size() >= 9 ? bit(8) : false;
        rule.rename = fields.size() == 10 ? bit(9) : false;
        const auto mode = fields[7];
        if (mode == "0") {
            rule.save_mode = SaveMode::OVERWRITE_LAST;
        } else if (mode == "1") {
            rule.save_mode = SaveMode::CREATE_NEW;
        } else {
            throw std::runtime_error("Invalid save mode on permissions.csv line " +
                                     std::to_string(line_number));
        }
        if (rule.object.empty()) {
            throw std::runtime_error("Empty permission object on permissions.csv line " +
                                     std::to_string(line_number));
        }
        next[rule.object][rule.path].push_back(std::move(rule));
    }

    const auto modified = std::filesystem::last_write_time(file_);
    {
        std::unique_lock lock(mutex_);
        rules_by_object_ = std::move(next);
        modified_ = modified;
    }
}

PermissionResult CsvPermissionProvider::Check(
    const std::string& user,
    const std::vector<std::string>& groups,
    const std::string& path,
    Action action) {
    const auto normalized = NormalizeLogicalPath(path);
    const auto has_permission = [action](const Rule& rule) {
        switch (action) {
            case Action::READ: return rule.read;
            case Action::WRITE: return rule.write;
            case Action::UPLOAD: return rule.upload;
            case Action::DELETE: return rule.remove;
            case Action::ADD_VERSION: return rule.add_version;
            case Action::DEL_VERSION: return rule.delete_version;
            case Action::RENAME: return rule.rename;
        }
        return false;
    };

    std::shared_lock lock(mutex_);
    bool allowed = false;
    bool create_new = false;
    auto apply_object = [&](const std::string& object) {
        const auto object_rules = rules_by_object_.find(object);
        if (object_rules == rules_by_object_.end()) {
            return;
        }
        auto candidate = normalized;
        for (;;) {
            const auto path_rules = object_rules->second.find(candidate);
            if (path_rules != object_rules->second.end()) {
                bool matched = false;
                bool object_allowed = false;
                bool object_create_new = false;
                for (const auto& rule : path_rules->second) {
                    if (candidate != normalized && !rule.directory) {
                        continue;
                    }
                    matched = true;
                    object_allowed = object_allowed || has_permission(rule);
                    object_create_new = object_create_new ||
                        (rule.write && rule.save_mode == SaveMode::CREATE_NEW);
                }
                if (matched) {
                    allowed = allowed || object_allowed;
                    create_new = create_new || object_create_new;
                    return;
                }
            }
            if (candidate == "/") {
                return;
            }
            const auto separator = candidate.find_last_of('/');
            candidate = separator == 0 ? "/" : candidate.substr(0, separator);
        }
    };
    for (const auto& group : groups) {
        apply_object(group);
    }
    apply_object(user);
    apply_object("@all");
    return {allowed,
            create_new ? SaveMode::CREATE_NEW : SaveMode::OVERWRITE_LAST};
}

}  // namespace dms
