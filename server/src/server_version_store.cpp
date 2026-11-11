// SPDX-License-Identifier: GPL-3.0-or-later
#include "server_api_support.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unordered_set>

namespace dms::server {

std::filesystem::path VersionDirectory(const Config& config,
                                       const std::string& logical_path) {
    const auto normalized = NormalizeLogicalPath(logical_path);
    return ResolveSafePath(config.VersionStore(), normalized);
}

std::filesystem::path VersionFile(const Config& config,
                                  const std::string& logical_path, int revision) {
    const auto normalized = NormalizeLogicalPath(logical_path);
    const auto relative = Utf8Path(normalized.substr(1));
    const auto extension = relative.extension().string();
    return VersionDirectory(config, normalized) /
           ("rev_" + std::to_string(revision) + extension);
}

std::vector<int> Revisions(const Config& config, const std::string& logical_path) {
    const auto directory = VersionDirectory(config, logical_path);
    std::vector<int> revisions;
    if (!std::filesystem::exists(directory)) {
        return revisions;
    }
    const auto filename =
        Utf8Path(NormalizeLogicalPath(logical_path).substr(1)).filename();
    const auto extension = filename.extension().string();
    const std::string suffix = extension;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        const auto prefix = std::string("rev_");
        if (name.size() <= prefix.size() + suffix.size() ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        const auto digits = name.substr(prefix.size(),
                                        name.size() - prefix.size() - suffix.size());
        int revision = 0;
        const auto parsed = std::from_chars(digits.data(),
                                            digits.data() + digits.size(), revision);
        if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() &&
            revision > 0) {
            revisions.push_back(revision);
        }
    }
    std::sort(revisions.begin(), revisions.end());
    return revisions;
}

int LatestRevision(const Config& config, const std::string& logical_path) {
    const auto revisions = Revisions(config, logical_path);
    return revisions.empty() ? 0 : revisions.back();
}

void SaveVersion(Runtime& runtime, const std::string& logical_path,
                 const std::filesystem::path& workspace_file, SaveMode mode) {
    const int latest = LatestRevision(runtime.config, logical_path);
    const int revision = latest == 0 ? 1
                         : mode == SaveMode::CREATE_NEW ? latest + 1 : latest;
    std::filesystem::create_directories(VersionDirectory(runtime.config, logical_path));
    std::filesystem::copy_file(workspace_file,
                               VersionFile(runtime.config, logical_path, revision),
                               std::filesystem::copy_options::overwrite_existing);
}

void TrimVersions(Runtime& runtime, const std::string& logical_path) {
    const auto revisions = Revisions(runtime.config, logical_path);
    if (revisions.size() <= 20) return;

    std::unordered_set<int> retained;
    const auto latest = revisions.back();
    const auto first_recent = revisions.size() > 20 ? revisions.size() - 20 : 0;
    for (std::size_t i = first_recent; i < revisions.size(); ++i) {
        retained.insert(revisions[i]);
    }

    using Clock = std::chrono::system_clock;
    const auto now = Clock::now();
    const auto cutoff = now - std::chrono::days(30);
    std::map<std::chrono::sys_days, std::pair<Clock::time_point, int>> earliest;
    for (const int revision : revisions) {
        const auto file = VersionFile(runtime.config, logical_path, revision);
        const auto write_time = std::filesystem::last_write_time(file);
        const auto modified = std::chrono::time_point_cast<Clock::duration>(
            write_time - std::filesystem::file_time_type::clock::now() + now);
        if (modified < cutoff || modified > now) continue;
        const auto day = std::chrono::floor<std::chrono::days>(modified);
        const auto found = earliest.find(day);
        if (found == earliest.end() || modified < found->second.first) {
            earliest.insert_or_assign(day, std::pair{modified, revision});
        }
    }
    for (const auto& [day, entry] : earliest) {
        (void)day;
        retained.insert(entry.second);
    }

    for (const int revision : revisions) {
        if (revision == latest || retained.contains(revision)) continue;
        if (!std::filesystem::remove(
                VersionFile(runtime.config, logical_path, revision))) {
            throw std::runtime_error("Unable to remove expired version rev_" +
                                     std::to_string(revision));
        }
    }
}

Json ReadMetadataDocument(const Config& config) {
    const auto file = config.MetadataFile();
    std::ifstream input(file);
    if (!input) {
        if (std::filesystem::exists(file)) {
            throw std::runtime_error("Cannot read metadata file: " + file.string());
        }
        return Json::object();
    }
    try {
        const auto document = Json::parse(input);
        if (!document.is_object()) {
            throw std::runtime_error("Metadata file must contain a JSON object");
        }
        return document;
    } catch (const std::exception& error) {
        // 元数据损坏不应该让整个目录浏览失败：降级为空表（创建者显示 unknown）。
        std::cerr << "Ignoring unreadable metadata file " << file << ": "
                  << error.what() << '\n';
        return Json::object();
    }
}

void WriteMetadataDocument(const Config& config, const Json& document) {
    const auto file = config.MetadataFile();
    std::filesystem::create_directories(file.parent_path());
    const auto temporary = file.string() + "." + MakeRandomToken() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Cannot write metadata file: " + temporary);
    }
    // replace：绝不允许序列化环节因为脏数据把请求（甚至线程）打挂。
    output << document.dump(2, ' ', false,
                            nlohmann::json::error_handler_t::replace)
           << '\n';
    output.flush();
    if (!output) {
        throw std::runtime_error("Cannot flush metadata file: " + temporary);
    }
    output.close();
    std::filesystem::rename(temporary, file);
}

void SetMetadataOwner(Runtime& runtime, const std::string& path, const std::string& owner) {
    std::lock_guard lock(runtime.metadata_mutex);
    auto document = ReadMetadataDocument(runtime.config);
    const auto found = document.find(path);
    if (found != document.end() && found->is_object() &&
        found->value("owner", std::string{}).size() > 0) {
        return;
    }
    document[path] = Json{{"owner", owner}, {"created_at", UtcTimestamp()}};
    WriteMetadataDocument(runtime.config, document);
}

void RemoveMetadataPath(Runtime& runtime, const std::string& path) {
    std::lock_guard lock(runtime.metadata_mutex);
    auto document = ReadMetadataDocument(runtime.config);
    document.erase(path);
    WriteMetadataDocument(runtime.config, document);
}

}  // namespace dms::server
