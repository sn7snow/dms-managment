// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dms/storage.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/message_generator.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace dms::server {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;
struct Response {
    http::message_generator message;
    std::filesystem::path cleanup_file;

    bool keep_alive() const noexcept { return message.keep_alive(); }
};

struct HeldLock {
    std::string user;
    std::chrono::steady_clock::time_point heartbeat;
    std::chrono::system_clock::time_point locked_at;
};

class NotificationBus {
public:
    void Subscribe(const std::string& token) {
        std::lock_guard lock(mutex_);
        mailboxes_.try_emplace(token);
    }

    void Unsubscribe(const std::string& token) {
        std::lock_guard lock(mutex_);
        mailboxes_.erase(token);
    }

    void Publish(const std::string& message) {
        std::lock_guard lock(mutex_);
        for (auto& [token, messages] : mailboxes_) {
            (void)token;
            messages.push_back(Json{{"type", "message"}, {"text", message}});
        }
    }

    void PublishLockState(const std::string& path, const std::string& user,
                          const std::string& locked_at = {}) {
        std::lock_guard lock(mutex_);
        for (auto& [token, messages] : mailboxes_) {
            (void)token;
            messages.push_back(
                Json{{"type", "lock"},
                     {"path", path},
                     {"user", user},
                     {"locked_at", locked_at}});
        }
    }

    void PublishRelease(const std::string& path) {
        PublishLockState(path, {});
        Publish("‘" + path + "’已经释放。");
    }

    Json Take(const std::string& token) {
        std::lock_guard lock(mutex_);
        const auto found = mailboxes_.find(token);
        if (found == mailboxes_.end()) return Json::array();
        Json messages = Json::array();
        for (auto& message : found->second) {
            messages.push_back(std::move(message));
        }
        found->second.clear();
        return messages;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<Json>> mailboxes_;
};

struct Runtime {
    Config config;
    UploadExtensionPolicy upload_extensions;
    CsvPermissionProvider permissions;
    PendingDeleteStore pending;
    AuthManager auth;
    AuditLog audit;
    FileLocks file_locks;
    TreeIndex tree;
    std::shared_mutex pending_gate;
    std::mutex document_locks_mutex;
    std::unordered_map<std::string, HeldLock> document_locks;
    std::mutex metadata_mutex;
    NotificationBus notifications;

    explicit Runtime(Config value)
        : config(std::move(value)),
          upload_extensions(config.UploadAllowlistFile(),
                            config.UploadDenylistFile()),
          permissions(config.PermissionsFile()),
          pending(config.PendingFile()),
          auth(config.users),
          audit(config.Logs()) {
        std::filesystem::create_directories(config.FileStore());
        std::filesystem::create_directories(config.VersionStore());
        std::filesystem::create_directories(config.Logs());
        const auto temporary = config.root / ".http-tmp";
        const auto temporary_status = std::filesystem::symlink_status(temporary);
        if (std::filesystem::is_symlink(temporary_status)) {
            throw std::runtime_error(
                "HTTP temporary directory must not be a symbolic link");
        }
        if (temporary_status.type() == std::filesystem::file_type::not_found) {
            std::filesystem::create_directory(temporary);
        } else if (!std::filesystem::is_directory(temporary_status)) {
            throw std::runtime_error(
                "HTTP temporary path is not a directory");
        }
        std::filesystem::permissions(
            temporary, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
        tree.Build(config.FileStore());
    }

    void Reload() {
        std::unique_lock lock(pending_gate);
        permissions.Reload();
        pending.Reload();
        tree.Build(config.FileStore());
    }

    void ReloadPermissions() { permissions.Reload(); }

    void ReloadPending() {
        std::unique_lock lock(pending_gate);
        pending.Reload();
    }
};

class PidFile {
public:
    explicit PidFile(const std::filesystem::path& path) : path_(path) {
        std::filesystem::create_directories(path_.parent_path());
        descriptor_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0640);
        if (descriptor_ < 0) {
            throw std::runtime_error("Cannot open PID file: " + path_.string());
        }
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("Another server process holds the PID file");
        }
        const auto pid = std::to_string(::getpid()) + "\n";
        if (::ftruncate(descriptor_, 0) != 0 ||
            ::write(descriptor_, pid.data(), pid.size()) !=
                static_cast<ssize_t>(pid.size())) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("Cannot write PID file: " + path_.string());
        }
    }

    ~PidFile() {
        if (descriptor_ >= 0) {
            ::unlink(path_.c_str());
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
    }

    PidFile(const PidFile&) = delete;
    PidFile& operator=(const PidFile&) = delete;

private:
    std::filesystem::path path_;
    int descriptor_ = -1;
};

Response SafeHandle(Runtime& runtime,
                    const http::request<http::string_body>& request,
                    const std::string& ip,
                    const std::filesystem::path& body_file = {});
std::optional<Response> PreflightFileTransfer(
    Runtime& runtime, const http::request<http::string_body>& request);

void StartListener(asio::io_context& io, tcp::endpoint endpoint,
                   Runtime& runtime, asio::thread_pool& workers,
                   asio::ssl::context* tls_context);
std::thread StartPermissionMonitor(Runtime& runtime, std::atomic_bool& stop);

}  // namespace dms::server
