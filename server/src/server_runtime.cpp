// SPDX-License-Identifier: GPL-3.0-or-later
#include "dms/storage.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/none.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#else
#include <conio.h>
#endif

#include "server_internal.hpp"

namespace dms::server {
namespace {

void SyncDirectory(const std::filesystem::path& directory) {
    const int descriptor =
        ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::runtime_error("Cannot open directory for sync: " +
                                 directory.string());
    }
    const int result = ::fsync(descriptor);
    const int sync_error = errno;
    ::close(descriptor);
    if (result != 0) {
        throw std::runtime_error("Cannot sync directory " + directory.string() +
                                 ": " + std::strerror(sync_error));
    }
}

void AppendRenameRecord(const std::filesystem::path& file,
                        const nlohmann::json& record) {
    const bool existed = std::filesystem::exists(file);
    const int descriptor =
        ::open(file.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        throw std::runtime_error("Cannot open rename intent log: " +
                                 std::string(std::strerror(errno)));
    }
    if (::fchmod(descriptor, 0600) != 0) {
        const int error = errno;
        ::close(descriptor);
        throw std::runtime_error("Cannot set rename intent log permissions: " +
                                 std::string(std::strerror(error)));
    }
    const auto line = record.dump() + '\n';
    std::size_t written = 0;
    while (written < line.size()) {
        const auto result =
            ::write(descriptor, line.data() + written, line.size() - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) {
            const int error = errno;
            ::close(descriptor);
            throw std::runtime_error("Cannot append rename intent log: " +
                                     std::string(std::strerror(error)));
        }
        written += static_cast<std::size_t>(result);
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        throw std::runtime_error("Cannot sync rename intent log: " +
                                 std::string(std::strerror(error)));
    }
    if (::close(descriptor) != 0) {
        throw std::runtime_error("Cannot close rename intent log: " +
                                 std::string(std::strerror(errno)));
    }
    if (!existed) SyncDirectory(file.parent_path());
}

std::filesystem::path RenameIntentFile(const Config& config) {
    return config.PendingFile().parent_path() / "rename_intent.jsonl";
}

struct RenameIntent {
    std::string id;
    std::string from;
    std::string to;
    bool has_versions = false;
    bool done = false;
};

}  // namespace

void DurableRename(const std::filesystem::path& from,
                   const std::filesystem::path& to) {
    std::filesystem::rename(from, to);
    SyncDirectory(from.parent_path());
    if (to.parent_path() != from.parent_path()) {
        SyncDirectory(to.parent_path());
    }
}

std::string WriteRenameIntent(const Config& config, const std::string& from,
                              const std::string& to, bool has_versions) {
    const auto id = MakeRandomToken();
    AppendRenameRecord(
        RenameIntentFile(config),
        {{"id", id},
         {"from", from},
         {"to", to},
         {"ts", UtcTimestamp()},
         {"has_versions", has_versions}});
    return id;
}

void CompleteRenameIntent(const Config& config, const std::string& id,
                          bool recovered) {
    auto record = nlohmann::json{
        {"id", id}, {"done", true}, {"ts", UtcTimestamp()}};
    if (recovered) record["recovered"] = true;
    AppendRenameRecord(RenameIntentFile(config), record);
}

void RecoverRenameIntents(const Config& config) {
    const auto file = RenameIntentFile(config);
    if (!std::filesystem::exists(file)) return;
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read rename intent log: " +
                                 file.string());
    }

    std::vector<RenameIntent> intents;
    std::unordered_map<std::string, std::size_t> indices;
    std::string line;
    while (std::getline(input, line)) {
        nlohmann::json record;
        try {
            record = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception& error) {
            if (input.eof()) break;
            throw std::runtime_error("Invalid rename intent log record: " +
                                     std::string(error.what()));
        }
        if (!record.is_object() || !record.contains("id") ||
            !record.at("id").is_string()) {
            throw std::runtime_error("Invalid rename intent log record");
        }
        const auto id = record.at("id").get<std::string>();
        if (record.value("done", false)) {
            const auto found = indices.find(id);
            if (found == indices.end()) {
                throw std::runtime_error("Unmatched rename completion record");
            }
            intents[found->second].done = true;
            continue;
        }
        if (id.empty() || !record.contains("from") ||
            !record.at("from").is_string() || !record.contains("to") ||
            !record.at("to").is_string() ||
            !record.contains("has_versions") ||
            !record.at("has_versions").is_boolean() ||
            indices.contains(id)) {
            throw std::runtime_error("Invalid rename intent log record");
        }
        RenameIntent intent{
            id, record.at("from").get<std::string>(),
            record.at("to").get<std::string>(),
            record.at("has_versions").get<bool>(), false};
        try {
            intent.from = NormalizeLogicalPath(intent.from);
            intent.to = NormalizeLogicalPath(intent.to);
        } catch (const std::exception& error) {
            // 坏记录不能让服务起不来：跳过并告警，由运维手工核对。
            std::cerr << "Ignoring rename intent with invalid path: "
                      << error.what() << '\n';
            continue;
        }
        if (intent.from == "/" || intent.to == "/") {
            throw std::runtime_error("Invalid path in rename intent log");
        }
        indices.emplace(id, intents.size());
        intents.push_back(std::move(intent));
    }
    if (input.bad()) {
        throw std::runtime_error("Error reading rename intent log: " +
                                 file.string());
    }

    for (const auto& intent : intents) {
        if (intent.done) continue;
        const auto source = ResolveSafePath(config.FileStore(), intent.from);
        const auto destination = ResolveSafePath(config.FileStore(), intent.to);
        const bool source_exists = std::filesystem::exists(source);
        const bool destination_exists = std::filesystem::exists(destination);
        if (source_exists == destination_exists) {
            throw std::runtime_error(
                "Cannot recover rename transaction: workspace state is ambiguous");
        }

        const auto old_versions =
            ResolveSafePath(config.VersionStore(), intent.from);
        const auto new_versions =
            ResolveSafePath(config.VersionStore(), intent.to);
        const bool old_versions_exist = std::filesystem::exists(old_versions);
        const bool new_versions_exist = std::filesystem::exists(new_versions);
        if (intent.has_versions) {
            if (old_versions_exist == new_versions_exist) {
                throw std::runtime_error(
                    "Cannot recover rename transaction: version state is ambiguous");
            }
            if (new_versions_exist) DurableRename(new_versions, old_versions);
        } else if (old_versions_exist || new_versions_exist) {
            throw std::runtime_error(
                "Cannot recover rename transaction: unexpected version history");
        }
        if (destination_exists) DurableRename(destination, source);
        CompleteRenameIntent(config, intent.id, true);
    }
}

std::thread StartPermissionMonitor(Runtime& runtime, std::atomic_bool& stop) {
    return std::thread([&runtime, &stop] {
        std::error_code permissions_error;
        std::error_code pending_error;
        auto permissions_time =
            std::filesystem::last_write_time(runtime.config.PermissionsFile(),
                                             permissions_error);
        auto pending_time =
            std::filesystem::last_write_time(runtime.config.PendingFile(), pending_error);
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (stop.load()) break;
            // 单轮失败不应让监控线程消失，否则权限变更再也不会生效。
            try {
            const auto current_permissions =
                std::filesystem::last_write_time(runtime.config.PermissionsFile(),
                                                 permissions_error);
            const auto current_pending =
                std::filesystem::last_write_time(runtime.config.PendingFile(),
                                                 pending_error);
            if (!permissions_error && current_permissions != permissions_time) {
                try {
                    runtime.ReloadPermissions();
                    permissions_time = current_permissions;
                } catch (const std::exception& error) {
                    std::cerr << "Permission reload failed: " << error.what() << '\n';
                }
            }
            if (!pending_error && current_pending != pending_time) {
                try {
                    runtime.ReloadPending();
                    pending_time = current_pending;
                } catch (const std::exception& error) {
                    std::cerr << "Pending-delete reload failed: " << error.what() << '\n';
                }
            }
            for (const auto& token : runtime.auth.Sweep()) {
                runtime.notifications.Unsubscribe(token);
            }
            {
                std::lock_guard lock(runtime.document_locks_mutex);
                const auto now = std::chrono::steady_clock::now();
                for (auto it = runtime.document_locks.begin();
                     it != runtime.document_locks.end();) {
                    if (now - it->second.heartbeat >= std::chrono::minutes(30)) {
                        runtime.notifications.PublishRelease(it->first);
                        it = runtime.document_locks.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            } catch (const std::exception& error) {
                std::cerr << "Permission monitor iteration failed: "
                          << error.what() << '\n';
            } catch (...) {
                std::cerr << "Permission monitor iteration failed unexpectedly\n";
            }
        }
    });
}

}  // namespace dms::server
