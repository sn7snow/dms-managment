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
#include <cstdint>
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
#include <semaphore>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#else
#include <conio.h>
#endif

#include "server_internal.hpp"
#include "server_api_support.hpp"

namespace dms::server {
namespace {

// PBKDF2 单次约 0.2 秒 CPU。不设并发闸门，几十个并发登录就能把 32 个 worker 全占满。
struct LoginSlots {
    static constexpr std::ptrdiff_t kMaxConcurrent = 2;

    static std::counting_semaphore<kMaxConcurrent>& Slots() {
        static std::counting_semaphore<kMaxConcurrent> slots{kMaxConcurrent};
        return slots;
    }

    static bool Acquire() {
        return Slots().try_acquire_for(std::chrono::seconds(2));
    }

    struct Release {
        ~Release() { Slots().release(); }
    };
};

Response HandleRequest(Runtime& runtime,
                       const http::request<http::string_body>& request,
                       const std::string& ip,
                       const std::filesystem::path& body_file) {
    const std::string target(request.target());
    const auto route = Route(target);
    const auto query = ParseQuery(target);
    const auto method = request.method();

    if (route == "/api/internal/refresh" && method == http::verb::post) {
        // 该接口只服务本机 dms-cron：非回环来源一律当作不存在。
        if (ip != "127.0.0.1" && ip != "::1") {
            return ErrorResponse(request, http::status::not_found, "Not found");
        }
        const std::string supplied(request["X-DMS-Refresh-Token"]);
        if (supplied.empty() ||
            !ConstantTimeEquals(supplied, runtime.config.refresh_token)) {
            return ErrorResponse(request, http::status::unauthorized, "Unauthorized");
        }
        runtime.Reload();
        return JsonResponse(request, http::status::ok, Json{{"ok", true}});
    }

    if (route == "/api/login" && method == http::verb::post) {
        if (!LoginSlots::Acquire()) {
            return ErrorResponse(request, http::status::too_many_requests,
                                 "SERVER_BUSY");
        }
        LoginSlots::Release login_slot;
        const auto body = ParseJson(request.body());
        const auto username = RequiredString(body, "user");
        const auto password = RequiredString(body, "pass");
        // 字段长度必须先卡住：失败表以用户名（+来源 IP）为键，超长会把内存吃光。
        if (username.empty() || username.size() > 64 || password.empty() ||
            password.size() > 256 ||
            username.find_first_of("\r\n\t") != std::string::npos) {
            throw std::invalid_argument("Invalid credential");
        }
        const auto result = runtime.auth.Login(username, password, ip);
        runtime.audit.Write(username, ip, "LOGIN", "", result ? "成功" : "失败");
        if (!result) {
            return ErrorResponse(request, http::status::unauthorized,
                                 "Invalid username or password");
        }
        runtime.notifications.Subscribe(result->at("token").get<std::string>());
        return JsonResponse(request, http::status::ok, *result);
    }

    const auto user = AuthenticatedUser(runtime, request);
    if (!user) {
        return ErrorResponse(request, http::status::unauthorized, "Authentication required");
    }
    const auto token = BearerToken(request);

    if (route == "/api/upload-policy" && method == http::verb::get) {
        return JsonResponse(
            request, http::status::ok,
            Json{{"allowed_extensions",
                  runtime.upload_extensions.AllowedExtensions()}});
    }

    if (route == "/api/logout" && method == http::verb::post) {
        std::vector<std::string> released_paths;
        {
            std::lock_guard lock(runtime.document_locks_mutex);
            for (auto it = runtime.document_locks.begin();
                 it != runtime.document_locks.end();) {
                if (it->second.user == user->user) {
                    released_paths.push_back(it->first);
                    it = runtime.document_locks.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& path : released_paths) {
            runtime.notifications.PublishRelease(path);
        }
        runtime.notifications.Unsubscribe(token);
        runtime.auth.Logout(token);
        return JsonResponse(request, http::status::ok, Json{{"ok", true}});
    }

    if (route == "/api/file/release-all" && method == http::verb::post) {
        std::vector<std::string> released_paths;
        {
            std::lock_guard lock(runtime.document_locks_mutex);
            // 遍历删除自己名下的锁
            for (auto it = runtime.document_locks.begin();
                 it != runtime.document_locks.end();) {
                if (it->second.user == user->user) {
                    released_paths.push_back(it->first);
                    it = runtime.document_locks.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& path : released_paths) {
            runtime.notifications.PublishRelease(path);
        }
        runtime.audit.Write(user->user, ip, "RELEASE_ALL", "",
                "释放全部锁" + std::to_string(released_paths.size()) + " 个");
        return JsonResponse(request, http::status::ok, Json{{"released", released_paths.size()}});
    }

    if (route == "/api/notifications" && method == http::verb::get) {
        // Notifications exist only in per-session memory queues; nothing is persisted.
        runtime.notifications.Subscribe(token);
        return JsonResponse(request, http::status::ok,
                            Json{{"messages", runtime.notifications.Take(token)}});
    }

    if (route == "/api/list" && method == http::verb::get) {
        const auto path = NormalizeLogicalPath(RequiredQuery(query, "path"));
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        if (path != "/") {
            RequirePermission(runtime, *user, path, Action::READ);
        }
        const auto directory = ResolveSafePath(runtime.config.FileStore(), path);
        if (!std::filesystem::is_directory(directory)) {
            return ErrorResponse(request, http::status::not_found, "Directory not found");
        }
        Json nodes = Json::array();
        // 元数据只读一次：以前每个条目都重读并重新解析整份 JSON，目录一大就是灾难。
        Json metadata;
        {
            std::lock_guard lock(runtime.metadata_mutex);
            metadata = ReadMetadataDocument(runtime.config);
        }
        const auto owner_of = [&metadata](const std::string& node_path) {
            const auto found = metadata.find(node_path);
            return found == metadata.end() || !found->is_object()
                       ? std::string("unknown")
                       : found->value("owner", std::string("unknown"));
        };
        for (const auto& node : runtime.tree.Children(path)) {
            if (runtime.pending.IsHidden(node.path) ||
                !runtime.permissions.Check(user->user, user->groups, node.path,
                                           Action::READ).allowed) {
                continue;
            }
            const auto actual = ResolveSafePath(runtime.config.FileStore(), node.path);
            const auto time_point = std::filesystem::exists(actual)
                ? std::filesystem::last_write_time(actual)
                : std::filesystem::file_time_type::min();
            const auto size = std::filesystem::is_regular_file(actual)
                ? std::filesystem::file_size(actual)
                : 0ULL;
            // Report the lock owner so every client can render the lock on the row.
            std::string occupied_by;
            std::string locked_at;
            {
                std::lock_guard lock(runtime.document_locks_mutex);
                const auto found = runtime.document_locks.find(node.path);
                if (found != runtime.document_locks.end()) {
                    occupied_by = found->second.user;
                    locked_at = FormatShanghaiTime(found->second.locked_at);
                }
            }
            const auto version_count = node.directory
                ? 0U
                : static_cast<unsigned>(Revisions(runtime.config, node.path).size());
            nodes.push_back({{"name", node.name},
                             {"modified", FormatUtcTime(time_point)},
                             {"type", node.directory ? "dir" : "file"},
                             {"size", std::to_string(size)},
                             {"owner", owner_of(node.path)},
                             {"perms", PermissionLetters(runtime, *user, node.path)},
                             {"save_mode",
                              PermissionSaveMode(runtime, *user, node.path)},
                             {"locked_by", occupied_by},
                             {"locked_at", locked_at},
                             {"version", version_count > 0
                                             ? "v" + std::to_string(version_count)
                                             : std::string{}}});
        }
        return JsonResponse(request, http::status::ok,
                            Json{{"nodes", nodes},
                                 {"directory_perms",
                                  PermissionLetters(runtime, *user, path)},
                                 {"directory_save_mode",
                                  PermissionSaveMode(runtime, *user, path)}});
    }

    if (route == "/api/file/open" && method == http::verb::post) {
        const auto body = ParseJson(request.body());
        const auto path = NormalizeLogicalPath(RequiredString(body, "path"));
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        RequirePermission(runtime, *user, path, Action::READ);
        const auto file = ResolveSafePath(runtime.config.FileStore(), path);
        if (!std::filesystem::is_regular_file(file)) {
            return ErrorResponse(request, http::status::not_found, "File not found");
        }
        const int version = LatestRevision(runtime.config, path);
        // 只有能写的人才能拿编辑锁。否则只读用户可以对任意可读文件霸占 30 分钟，
        // 让真正需要编辑的人永远拿不到锁。
        const bool can_write = runtime.permissions
                                   .Check(user->user, user->groups, path,
                                          Action::WRITE)
                                   .allowed;
        std::lock_guard lock(runtime.document_locks_mutex);
        const auto now = std::chrono::steady_clock::now();
        auto found = runtime.document_locks.find(path);
        if (found != runtime.document_locks.end() &&
            now - found->second.heartbeat >= std::chrono::minutes(30)) {
            runtime.document_locks.erase(found);
            runtime.notifications.PublishRelease(path);
            found = runtime.document_locks.end();
        }
        if (!can_write) {
            return JsonResponse(
                request, http::status::ok,
                Json{{"status", "read_only"},
                     {"occupied_by", found == runtime.document_locks.end()
                                         ? std::string{}
                                         : found->second.user},
                     {"locked_at", found == runtime.document_locks.end()
                                       ? std::string{}
                                       : FormatShanghaiTime(found->second.locked_at)},
                     {"version", version}});
        }
        if (found == runtime.document_locks.end() ||
            found->second.user == user->user) {
            const auto locked_at = std::chrono::system_clock::now();
            runtime.document_locks.insert_or_assign(
                path, HeldLock{user->user, now, locked_at});
            const auto locked_at_text = FormatShanghaiTime(locked_at);
            runtime.notifications.PublishLockState(
                path, user->user, locked_at_text);
            return JsonResponse(request, http::status::ok,
                                Json{{"status", "editable"},
                                     {"occupied_by", ""},
                                     {"locked_at", locked_at_text},
                                     {"version", version}});
        }
        return JsonResponse(request, http::status::ok,
                            Json{{"status", "read_only"},
                                 {"occupied_by", found->second.user},
                                 {"locked_at",
                                  FormatShanghaiTime(found->second.locked_at)},
                                 {"version", version}});
    }

    if (route == "/api/file/close" && method == http::verb::post) {
        const auto body = ParseJson(request.body());
        const auto path = NormalizeLogicalPath(RequiredString(body, "path"));
        std::lock_guard lock(runtime.document_locks_mutex);
        const auto found = runtime.document_locks.find(path);
        if (found != runtime.document_locks.end() && found->second.user == user->user) {
            runtime.document_locks.erase(found);
            runtime.notifications.PublishRelease(path);
        }
        return JsonResponse(request, http::status::ok, Json{{"ok", true}});
    }

    if (route == "/api/file/request-edit" && method == http::verb::post) {
        const auto body = ParseJson(request.body());
        const auto path = NormalizeLogicalPath(RequiredString(body, "path"));
        RequirePermission(runtime, *user, path, Action::READ);
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        const auto file = ResolveSafePath(runtime.config.FileStore(), path);
        if (!std::filesystem::is_regular_file(file)) {
            return ErrorResponse(request, http::status::not_found, "File not found");
        }
        std::lock_guard lock(runtime.document_locks_mutex);
        const auto found = runtime.document_locks.find(path);
        if (found == runtime.document_locks.end()) {
            return ErrorResponse(request, http::status::conflict,
                                 "File is not currently locked");
        }
        if (std::chrono::steady_clock::now() - found->second.heartbeat >=
            std::chrono::minutes(30)) {
            runtime.document_locks.erase(found);
            runtime.notifications.PublishRelease(path);
            return ErrorResponse(request, http::status::conflict,
                                 "File lock has expired");
        }
        runtime.notifications.Publish("用户" + user->user + "想要编辑'" +
                                      path + "'，请求释放。");
        return JsonResponse(request, http::status::accepted, Json{{"ok", true}});
    }

    if (route == "/api/file/heartbeat" && method == http::verb::post) {
        const auto body = ParseJson(request.body());
        std::vector<std::string> paths;
        if (body.contains("paths") && body.at("paths").is_array()) {
            for (const auto& value : body.at("paths")) {
                if (!value.is_string()) {
                    throw std::invalid_argument("Invalid heartbeat path");
                }
                paths.push_back(
                    NormalizeLogicalPath(value.get<std::string>()));
            }
        } else if (body.contains("path") && body.at("path").is_string()) {
            paths.push_back(NormalizeLogicalPath(
                body.at("path").get<std::string>()));
        } else {
            throw std::invalid_argument("Missing heartbeat paths");
        }
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        Json ok = Json::array();
        Json expired = Json::array();
        std::lock_guard lock(runtime.document_locks_mutex);
        const auto now = std::chrono::steady_clock::now();
        for (const auto& path : paths) {
            const auto found = runtime.document_locks.find(path);
            if (found == runtime.document_locks.end() ||
                found->second.user != user->user ||
                now - found->second.heartbeat >= std::chrono::minutes(30)) {
                if (found != runtime.document_locks.end() &&
                    now - found->second.heartbeat >= std::chrono::minutes(30)) {
                    runtime.notifications.PublishRelease(path);
                    runtime.document_locks.erase(found);
                }
                expired.push_back(path);
                continue;
            }
            found->second.heartbeat = now;
            ok.push_back(path);
        }
        return JsonResponse(request, http::status::ok,
                            Json{{"ok", ok}, {"expired", expired}});
    }

    if (route == "/api/archive" && method == http::verb::get) {
        const auto path = NormalizeLogicalPath(RequiredQuery(query, "path"));
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        RequirePermission(runtime, *user, path, Action::READ);
        const auto archive = BuildTarArchive(runtime, *user, path);
        runtime.audit.Write(user->user, ip, "ARCHIVE", path, "下载目录归档");
        return MakeFileResponse(request, http::status::ok, archive,
                                "application/x-tar", archive, "download.tar");
    }

    if (route == "/api/file" && method == http::verb::get) {
        const auto path = NormalizeLogicalPath(RequiredQuery(query, "path"));
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        RequirePermission(runtime, *user, path, Action::READ);
        const auto file = ResolveSafePath(runtime.config.FileStore(), path);
        if (!std::filesystem::is_regular_file(file)) {
            return ErrorResponse(request, http::status::not_found, "File not found");
        }
        runtime.audit.Write(user->user, ip, "DOWNLOAD", path, "下载文件");
        return MakeFileResponse(request, http::status::ok, file,
                                "application/octet-stream");
    }

    if (route == "/api/file" && method == http::verb::put) {
        const auto path = NormalizeLogicalPath(RequiredQuery(query, "path"));
        if (!runtime.upload_extensions.IsAllowed(path)) {
            return ErrorResponse(request, http::status::unsupported_media_type,
                                 "UNSUPPORTED_FILE_TYPE");
        }
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        // 临界区里只做内存态判定。文件 IO（整份拷贝 + 版本裁剪）绝不持全局锁，
        // 否则一次大文件保存就会让所有客户端的加锁/刷新一起卡住。
        SaveMode save_mode = SaveMode::OVERWRITE_LAST;
        {
            std::lock_guard document_lock(runtime.document_locks_mutex);
            const auto permission = runtime.permissions.Check(
                user->user, user->groups, path, Action::WRITE);
            if (!permission.allowed) {
                throw std::domain_error("Permission denied");
            }
            save_mode = permission.save_mode;
            const auto held = runtime.document_locks.find(path);
            if (held == runtime.document_locks.end() ||
                held->second.user != user->user ||
                std::chrono::steady_clock::now() - held->second.heartbeat >=
                    std::chrono::minutes(30)) {
                return ErrorResponse(request, http::status::conflict, "OCCUPIED");
            }
        }
        const int requested_version = ParseIfMatch(request);
        const int current_version = LatestRevision(runtime.config, path);
        if (requested_version != current_version) {
            return JsonResponse(
                request, http::status::conflict,
                Json{{"error", "VERSION_CONFLICT"},
                     {"current_version", current_version}});
        }
        const auto workspace_file = ResolveSafePath(runtime.config.FileStore(), path);
        if (!std::filesystem::is_directory(workspace_file.parent_path())) {
            return ErrorResponse(request, http::status::not_found,
                                 "Parent directory not found");
        }
        {
            // 空正文会把已有文件截断为 0 字节，并生成一个"正常"版本，属于不可逆破坏。
            std::error_code existing_error;
            const auto existing_size =
                std::filesystem::file_size(workspace_file, existing_error);
            std::error_code incoming_error;
            const auto incoming_size =
                body_file.empty()
                    ? static_cast<std::uintmax_t>(request.body().size())
                    : std::filesystem::file_size(body_file, incoming_error);
            if (incoming_size == 0 && !existing_error && existing_size > 0) {
                return ErrorResponse(request, http::status::bad_request,
                                     "EMPTY_BODY");
            }
        }
        const auto staging = workspace_file.string() + "." +
                             MakeRandomToken() + ".upload";
        if (body_file.empty()) {
            std::ofstream output(staging, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("Cannot write file: " + staging);
            }
            output.write(request.body().data(),
                         static_cast<std::streamsize>(request.body().size()));
            output.flush();
            if (!output) {
                throw std::runtime_error("Unable to flush uploaded file");
            }
        } else {
            StageRequestBody(body_file, staging);
        }
        try {
            std::filesystem::rename(staging, workspace_file);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            throw;
        }
        SaveVersion(runtime, path, workspace_file, save_mode);
        TrimVersions(runtime, path);
        const int version = LatestRevision(runtime.config, path);
        runtime.tree.Upsert(path, false);
        SetMetadataOwner(runtime, path, user->user);
        runtime.audit.Write(user->user, ip, "SAVE", path,
                            save_mode == SaveMode::CREATE_NEW
                                ? "新增版本 rev_" + std::to_string(version)
                                : "覆盖版本 rev_" + std::to_string(version));
        return JsonResponse(request, http::status::ok,
                            Json{{"ok", true}, {"version", version}});
    }

    if (route == "/api/upload" && method == http::verb::post) {
        const auto directory_path =
            NormalizeLogicalPath(RequiredQuery(query, "path"));
        const auto name = RequiredQuery(query, "name");
        if (name == "." || name == ".." ||
            name.find_first_of("/\\") != std::string::npos ||
            name.find('\0') != std::string::npos ||
            name.size() > 255 ||
            name.find_first_of("\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b"
                               "\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16"
                               "\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f") !=
                std::string::npos) {
            throw std::invalid_argument("Invalid upload filename");
        }
        const auto path = NormalizeLogicalPath(
            directory_path == "/" ? "/" + name : directory_path + "/" + name);
        if (!runtime.upload_extensions.IsAllowed(name)) {
            return ErrorResponse(request, http::status::unsupported_media_type,
                                 "UNSUPPORTED_FILE_TYPE");
        }
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, directory_path);
        CheckVisible(runtime, path);
        // 新文件的导入权限与编辑权限相互独立（见下：覆盖已有文件另需 WRITE）。
        RequirePermission(runtime, *user, path, Action::UPLOAD);
        const auto directory =
            ResolveSafePath(runtime.config.FileStore(), directory_path);
        if (!std::filesystem::is_directory(directory)) {
            return ErrorResponse(request, http::status::not_found,
                                 "Upload directory not found");
        }
        const auto destination =
            ResolveSafePath(runtime.config.FileStore(), path);
        if (std::filesystem::exists(destination) &&
            !std::filesystem::is_regular_file(destination)) {
            return ErrorResponse(request, http::status::conflict,
                                 "Upload destination is not a regular file");
        }
        // 上传不隐含编辑权限：目标已存在时等同"改写内容"，必须另外持有写权限。
        if (std::filesystem::exists(destination) &&
            !runtime.permissions
                 .Check(user->user, user->groups, path, Action::WRITE)
                 .allowed) {
            return ErrorResponse(request, http::status::forbidden,
                                 "Permission denied");
        }
        {
            std::lock_guard lock(runtime.document_locks_mutex);
            const auto held = runtime.document_locks.find(path);
            if (held != runtime.document_locks.end()) {
                if (std::chrono::steady_clock::now() - held->second.heartbeat <
                    std::chrono::minutes(30)) {
                    return ErrorResponse(request, http::status::conflict,
                                         "File is currently locked");
                }
                runtime.document_locks.erase(held);
                runtime.notifications.PublishRelease(path);
            }
        }

        const auto temporary =
            destination.string() + "." + MakeRandomToken() + ".upload";
        if (body_file.empty()) {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("Cannot write uploaded file: " + temporary);
            }
            output.write(request.body().data(),
                         static_cast<std::streamsize>(request.body().size()));
            output.flush();
            if (!output) {
                throw std::runtime_error("Unable to flush uploaded file");
            }
        } else {
            StageRequestBody(body_file, temporary);
        }
        try {
            std::filesystem::rename(temporary, destination);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
        SaveVersion(runtime, path, destination, SaveMode::CREATE_NEW);
        TrimVersions(runtime, path);
        const int version = LatestRevision(runtime.config, path);
        runtime.tree.Upsert(path, false);
        SetMetadataOwner(runtime, path, user->user);
        runtime.audit.Write(user->user, ip, "UPLOAD", path,
                            "上传本地文件，版本 v" + std::to_string(version));
        return JsonResponse(request, http::status::created,
                            Json{{"ok", true}, {"version", version}});
    }

    if (route == "/api/upload-directory" && method == http::verb::post) {
        const auto path =
            NormalizeLogicalPath(RequiredQuery(query, "path"));
        if (path == "/") {
            throw std::invalid_argument("Cannot upload a directory as the storage root");
        }
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        RequirePermission(runtime, *user, path, Action::UPLOAD);
        const auto directory =
            ResolveSafePath(runtime.config.FileStore(), path);
        if (std::filesystem::exists(directory)) {
            if (!std::filesystem::is_directory(directory)) {
                return ErrorResponse(request, http::status::conflict,
                                     "Upload directory conflicts with a file");
            }
            return JsonResponse(request, http::status::ok,
                                Json{{"ok", true}, {"existing", true}});
        }
        if (!std::filesystem::is_directory(directory.parent_path())) {
            return ErrorResponse(request, http::status::not_found,
                                 "Parent directory not found");
        }
        if (!std::filesystem::create_directory(directory)) {
            return ErrorResponse(request, http::status::conflict,
                                 "Unable to create upload directory");
        }
        runtime.tree.Upsert(path, true);
        SetMetadataOwner(runtime, path, user->user);
        return JsonResponse(request, http::status::created,
                            Json{{"ok", true}, {"existing", false}});
    }

    if (route == "/api/file/version/manual" && method == http::verb::post) {
        const auto body = ParseJson(request.body());
        const auto path = NormalizeLogicalPath(RequiredString(body, "path"));
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        RequirePermission(runtime, *user, path, Action::ADD_VERSION);
        const auto file = ResolveSafePath(runtime.config.FileStore(), path);
        if (!std::filesystem::is_regular_file(file)) {
            return ErrorResponse(request, http::status::not_found, "File not found");
        }
        const int revision = LatestRevision(runtime.config, path) + 1;
        std::filesystem::create_directories(VersionDirectory(runtime.config, path));
        std::filesystem::copy_file(file, VersionFile(runtime.config, path, revision));
        TrimVersions(runtime, path);
        runtime.audit.Write(user->user, ip, "MANUAL_VER", path,
                            "新增版本 rev_" + std::to_string(revision));
        return JsonResponse(request, http::status::ok,
                            Json{{"ok", true}, {"version", revision}});
    }

    if (route == "/api/file/version" && method == http::verb::delete_) {
        const auto path = NormalizeLogicalPath(RequiredQuery(query, "path"));
        const auto revision_text = RequiredQuery(query, "rev");
        int revision = 0;
        const auto parsed = std::from_chars(revision_text.data(),
                                            revision_text.data() + revision_text.size(),
                                            revision);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != revision_text.data() + revision_text.size() || revision < 1) {
            throw std::invalid_argument("Invalid revision number");
        }
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        RequirePermission(runtime, *user, path, Action::DEL_VERSION);
        if (revision == LatestRevision(runtime.config, path)) {
            return ErrorResponse(request, http::status::conflict,
                                 "Cannot delete latest version");
        }
        const auto version = VersionFile(runtime.config, path, revision);
        if (!std::filesystem::remove(version)) {
            return ErrorResponse(request, http::status::not_found, "Version not found");
        }
        runtime.audit.Write(user->user, ip, "DEL_VER", path,
                            "删除版本 rev_" + std::to_string(revision));
        return JsonResponse(request, http::status::ok, Json{{"ok", true}});
    }

    if (route == "/api/file/versions" && method == http::verb::get) {
        const auto path = NormalizeLogicalPath(RequiredQuery(query, "path"));
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        RequirePermission(runtime, *user, path, Action::READ);
        Json versions = Json::array();
        for (const int revision : Revisions(runtime.config, path)) {
            const auto file = VersionFile(runtime.config, path, revision);
            const auto modified = std::filesystem::last_write_time(file);
            const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                modified - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            const auto timestamp = std::chrono::system_clock::to_time_t(system_time);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &timestamp);
#else
            gmtime_r(&timestamp, &tm);
#endif
            std::ostringstream formatted;
            formatted << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            versions.push_back({{"rev", revision}, {"ts", formatted.str()}});
        }
        return JsonResponse(request, http::status::ok, Json{{"versions", versions}});
    }

    if (route == "/api/file/version" && method == http::verb::get) {
        const auto path = NormalizeLogicalPath(RequiredQuery(query, "path"));
        const auto revision_text = RequiredQuery(query, "rev");
        int revision = 0;
        const auto parsed = std::from_chars(revision_text.data(),
                                            revision_text.data() + revision_text.size(),
                                            revision);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != revision_text.data() + revision_text.size() || revision < 1) {
            throw std::invalid_argument("Invalid revision number");
        }
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        RequirePermission(runtime, *user, path, Action::READ);
        const auto file = VersionFile(runtime.config, path, revision);
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            return ErrorResponse(request, http::status::not_found, "Version not found");
        }
        input.close();
        runtime.audit.Write(user->user, ip, "DOWNLOAD_VER", path,
                            "下载历史版本 rev_" + std::to_string(revision));
        return MakeFileResponse(request, http::status::ok, file,
                                "application/octet-stream");
    }

    if (route == "/api/mkdir" && method == http::verb::post) {
        const auto body = ParseJson(request.body());
        const auto path = NormalizeLogicalPath(RequiredString(body, "path"));
        if (path == "/") {
            throw std::invalid_argument("Cannot create the storage root");
        }
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        RequirePermission(runtime, *user, path, Action::WRITE);
        const auto directory = ResolveSafePath(runtime.config.FileStore(), path);
        if (!std::filesystem::is_directory(directory.parent_path())) {
            return ErrorResponse(request, http::status::not_found,
                                 "Parent directory not found");
        }
        if (!std::filesystem::create_directory(directory)) {
            return ErrorResponse(request, http::status::conflict, "Directory already exists");
        }
        runtime.tree.Upsert(path, true);
        SetMetadataOwner(runtime, path, user->user);
        return JsonResponse(request, http::status::ok, Json{{"ok", true}});
    }

    if (route == "/api/rename" && method == http::verb::post) {
        const auto body = ParseJson(request.body());
        const auto from = NormalizeLogicalPath(RequiredString(body, "from"));
        const auto to = NormalizeLogicalPath(RequiredString(body, "to"));
        if (from == "/" || to == "/" ||
            (to.size() > from.size() && to.compare(0, from.size(), from) == 0 &&
             to[from.size()] == '/')) {
            throw std::invalid_argument("Invalid rename destination");
        }
        std::vector<FileLocks::Guard> guards;
        guards.reserve(256);
        for (auto& shard_guard : runtime.file_locks.AcquireAll()) {
            guards.push_back(std::move(shard_guard));
        }
        std::unique_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, from);
        CheckVisible(runtime, to);
        RequirePermission(runtime, *user, from, Action::RENAME);
        RequirePermission(runtime, *user, to, Action::RENAME);
        const auto source = ResolveSafePath(runtime.config.FileStore(), from);
        const auto destination = ResolveSafePath(runtime.config.FileStore(), to);
        if (!std::filesystem::exists(source) || std::filesystem::exists(destination)) {
            return ErrorResponse(request, http::status::conflict,
                                 "Source missing or destination already exists");
        }
        const auto old_versions = VersionDirectory(runtime.config, from);
        const auto new_versions = VersionDirectory(runtime.config, to);
        if (std::filesystem::exists(new_versions)) {
            return ErrorResponse(request, http::status::conflict,
                                 "Destination already has version history");
        }
        if (!std::filesystem::is_directory(destination.parent_path())) {
            return ErrorResponse(request, http::status::not_found,
                                 "Destination parent directory not found");
        }
        const bool is_directory = std::filesystem::is_directory(source);
        const std::string source_prefix = from + "/";
        {
            std::lock_guard lock(runtime.document_locks_mutex);
            const auto now = std::chrono::steady_clock::now();
            for (auto it = runtime.document_locks.begin();
                 it != runtime.document_locks.end();) {
                if (now - it->second.heartbeat >= std::chrono::minutes(30)) {
                    runtime.notifications.PublishRelease(it->first);
                    it = runtime.document_locks.erase(it);
                    continue;
                }
                if (it->first == from ||
                    (is_directory && it->first.starts_with(source_prefix))) {
                    return JsonResponse(
                        request, http::status::conflict,
                        Json{{"error", "LOCKED"},
                             {"occupied_by", it->second.user}});
                }
                ++it;
            }
        }
        const bool has_versions = std::filesystem::exists(old_versions);
        if (has_versions) {
            std::filesystem::create_directories(new_versions.parent_path());
        }
        const auto intent_id =
            WriteRenameIntent(runtime.config, from, to, has_versions);
        const auto rollback = [&] {
            const bool old_exists = std::filesystem::exists(old_versions);
            const bool new_exists = std::filesystem::exists(new_versions);
            if (has_versions) {
                if (old_exists == new_exists) {
                    throw std::runtime_error(
                        "Version history state is ambiguous during rename rollback");
                }
                if (new_exists) DurableRename(new_versions, old_versions);
            } else if (old_exists || new_exists) {
                throw std::runtime_error(
                    "Unexpected version history during rename rollback");
            }

            const bool source_exists = std::filesystem::exists(source);
            const bool destination_exists = std::filesystem::exists(destination);
            if (source_exists == destination_exists) {
                throw std::runtime_error(
                    "Workspace state is ambiguous during rename rollback");
            }
            if (destination_exists) DurableRename(destination, source);
        };
        try {
            try {
                DurableRename(source, destination);
            } catch (const std::filesystem::filesystem_error& error) {
                if (error.code() == std::errc::cross_device_link) {
                    throw std::runtime_error(
                        "Workspace rename crosses filesystems (EXDEV): " +
                        std::string(error.what()));
                }
                throw;
            }
            if (has_versions) {
                try {
                    DurableRename(old_versions, new_versions);
                } catch (const std::filesystem::filesystem_error& error) {
                    if (error.code() == std::errc::cross_device_link) {
                        throw std::runtime_error(
                            "Version history rename crosses filesystems (EXDEV): " +
                            std::string(error.what()));
                    }
                    throw;
                }
            }
            CompleteRenameIntent(runtime.config, intent_id);
        } catch (const std::exception& error) {
            try {
                rollback();
                CompleteRenameIntent(runtime.config, intent_id);
            } catch (const std::exception& rollback_error) {
                throw std::runtime_error(
                    "Rename failed: " + std::string(error.what()) +
                    "; rollback or intent completion failed: " +
                    rollback_error.what());
            }
            throw;
        }
        runtime.tree.Move(from, to, is_directory);
        {
            std::lock_guard metadata_lock(runtime.metadata_mutex);
            auto document = ReadMetadataDocument(runtime.config);
            std::vector<std::string> moving_keys;
            for (auto it = document.begin(); it != document.end(); ++it) {
                const auto key = std::string(it.key());
                if (key == from ||
                    (is_directory && key.size() > from.size() &&
                     key.compare(0, from.size(), from) == 0 &&
                     key[from.size()] == '/')) {
                    moving_keys.push_back(key);
                }
            }
            for (const auto& key : moving_keys) {
                const auto metadata = document[key];
                const auto suffix = key == from ? std::string{} : key.substr(from.size());
                document.erase(key);
                document[to + suffix] = metadata;
            }
            WriteMetadataDocument(runtime.config, document);
        }
        {
            std::lock_guard document_lock(runtime.document_locks_mutex);
            const std::string lock_source_prefix = from + "/";
            std::vector<std::pair<std::string, HeldLock>> moved_locks;
            for (auto it = runtime.document_locks.begin();
                 it != runtime.document_locks.end();) {
                if (it->first == from ||
                    (is_directory &&
                     it->first.starts_with(lock_source_prefix))) {
                    const auto suffix = it->first == from
                                            ? std::string{}
                                            : it->first.substr(from.size());
                    moved_locks.emplace_back(to + suffix, it->second);
                    it = runtime.document_locks.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto& [path, held] : moved_locks) {
                runtime.document_locks.insert_or_assign(path, std::move(held));
            }
        }
        return JsonResponse(request, http::status::ok, Json{{"ok", true}});
    }

    if (route == "/api/file" && method == http::verb::delete_) {
        const auto path = NormalizeLogicalPath(RequiredQuery(query, "path"));
        if (path == "/") {
            throw std::invalid_argument("Cannot delete the storage root");
        }
        auto guard = runtime.file_locks.Acquire(path);
        std::unique_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        RequirePermission(runtime, *user, path, Action::DELETE);
        const auto target_path = ResolveSafePath(runtime.config.FileStore(), path);
        if (!std::filesystem::exists(target_path)) {
            return ErrorResponse(request, http::status::not_found, "Path not found");
        }
        if (std::filesystem::is_directory(target_path)) {
            runtime.pending.Add({user->user, path, 0, UtcTimestamp()});
            std::vector<std::string> released_paths;
            {
                std::lock_guard lock(runtime.document_locks_mutex);
                for (auto it = runtime.document_locks.begin();
                     it != runtime.document_locks.end();) {
                    if (it->first == path ||
                        (it->first.size() > path.size() &&
                         it->first.compare(0, path.size(), path) == 0 &&
                         it->first[path.size()] == '/')) {
                        released_paths.push_back(it->first);
                        it = runtime.document_locks.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (const auto& released_path : released_paths) {
                runtime.notifications.PublishRelease(released_path);
            }
            runtime.audit.Write(user->user, ip, "PENDING_DEL", path,
                                "申请删除文件夹");
            return JsonResponse(request, http::status::accepted,
                                Json{{"message", "删除申请已提交，等待管理员审核"}});
        }
        if (!std::filesystem::remove(target_path)) {
            throw std::runtime_error("Unable to delete file: " + target_path.string());
        }
        runtime.tree.Remove(path);
        RemoveMetadataPath(runtime, path);
        std::filesystem::remove_all(VersionDirectory(runtime.config, path));
        bool released_lock = false;
        {
            std::lock_guard lock(runtime.document_locks_mutex);
            released_lock = runtime.document_locks.erase(path) > 0;
        }
        if (released_lock) {
            runtime.notifications.PublishRelease(path);
        }
        runtime.audit.Write(user->user, ip, "DELETE", path, "删除文件");
        return JsonResponse(request, http::status::ok, Json{{"ok", true}});
    }

    return ErrorResponse(request, http::status::not_found, "Not found");
}


}  // namespace

std::optional<Response> PreflightFileTransfer(
    Runtime& runtime, const http::request<http::string_body>& request) {
    const std::string target(request.target());
    const auto route = Route(target);
    const auto method = request.method();
    const bool upload = route == "/api/upload" && method == http::verb::post;
    const bool save = route == "/api/file" && method == http::verb::put;
    if (!upload && !save) return std::nullopt;

    auto closed_request = request;
    closed_request.keep_alive(false);
    try {
        const auto user = AuthenticatedUser(runtime, request);
        if (!user) {
            return ErrorResponse(closed_request, http::status::unauthorized,
                                 "Authentication required");
        }
        const auto query = ParseQuery(target);
        if (save) {
            const auto path =
                NormalizeLogicalPath(RequiredQuery(query, "path"));
            if (!runtime.upload_extensions.IsAllowed(path)) {
                return ErrorResponse(closed_request,
                                     http::status::unsupported_media_type,
                                     "UNSUPPORTED_FILE_TYPE");
            }
            std::shared_lock visibility(runtime.pending_gate);
            CheckVisible(runtime, path);
            RequirePermission(runtime, *user, path, Action::WRITE);
            {
                std::lock_guard lock(runtime.document_locks_mutex);
                const auto lock_it = runtime.document_locks.find(path);
                if (lock_it == runtime.document_locks.end() ||
                    lock_it->second.user != user->user ||
                    std::chrono::steady_clock::now() -
                            lock_it->second.heartbeat >=
                        std::chrono::minutes(30)) {
                    return ErrorResponse(closed_request, http::status::conflict,
                                         "OCCUPIED");
                }
            }
            const int current_version = LatestRevision(runtime.config, path);
            if (ParseIfMatch(request) != current_version) {
                return JsonResponse(
                    closed_request, http::status::conflict,
                    Json{{"error", "VERSION_CONFLICT"},
                         {"current_version", current_version}});
            }
        } else {
            const auto directory_path =
                NormalizeLogicalPath(RequiredQuery(query, "path"));
            const auto name = RequiredQuery(query, "name");
            if (name == "." || name == ".." ||
                name.find_first_of("/\\") != std::string::npos ||
                name.find('\0') != std::string::npos ||
                name.size() > 255 ||
                name.find_first_of("\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b"
                                   "\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16"
                                   "\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f") !=
                    std::string::npos) {
                throw std::invalid_argument("Invalid upload filename");
            }
            if (!runtime.upload_extensions.IsAllowed(name)) {
                return ErrorResponse(closed_request,
                                     http::status::unsupported_media_type,
                                     "UNSUPPORTED_FILE_TYPE");
            }
            const auto path = NormalizeLogicalPath(
                directory_path == "/" ? "/" + name
                                       : directory_path + "/" + name);
            std::shared_lock visibility(runtime.pending_gate);
            CheckVisible(runtime, directory_path);
            CheckVisible(runtime, path);
            RequirePermission(runtime, *user, path, Action::UPLOAD);
            const auto directory =
                ResolveSafePath(runtime.config.FileStore(), directory_path);
            if (!std::filesystem::is_directory(directory)) {
                return ErrorResponse(closed_request, http::status::not_found,
                                     "Upload directory not found");
            }
            const auto destination =
                ResolveSafePath(runtime.config.FileStore(), path);
            if (std::filesystem::exists(destination) &&
                !std::filesystem::is_regular_file(destination)) {
                return ErrorResponse(closed_request, http::status::conflict,
                                     "Upload destination is not a regular file");
            }
            if (std::filesystem::exists(destination) &&
                !runtime.permissions
                     .Check(user->user, user->groups, path, Action::WRITE)
                     .allowed) {
                return ErrorResponse(closed_request, http::status::forbidden,
                                     "Permission denied");
            }
            std::lock_guard lock(runtime.document_locks_mutex);
            const auto held = runtime.document_locks.find(path);
            if (held != runtime.document_locks.end() &&
                std::chrono::steady_clock::now() - held->second.heartbeat <
                    std::chrono::minutes(30)) {
                return ErrorResponse(closed_request, http::status::conflict,
                                     "File is currently locked");
            }
        }
        return std::nullopt;
    } catch (const std::invalid_argument&) {
        return JsonResponse(closed_request, http::status::bad_request,
                            Json{{"error", "INVALID_REQUEST"}});
    } catch (const std::domain_error& error) {
        const auto status = std::string_view(error.what()) == "Permission denied"
                                ? http::status::forbidden
                                : http::status::not_found;
        return ErrorResponse(closed_request, status, error.what());
    } catch (const std::exception& error) {
        std::cerr << "File transfer preflight failed: " << error.what() << '\n';
        return ErrorResponse(closed_request,
                             http::status::internal_server_error,
                             "Internal server error");
    } catch (...) {
        std::cerr << "File transfer preflight failed with an unknown exception\n";
        return ErrorResponse(closed_request,
                             http::status::internal_server_error,
                             "Internal server error");
    }
}

Response SafeHandle(Runtime& runtime,
                    const http::request<http::string_body>& request,
                    const std::string& ip,
                    const std::filesystem::path& body_file) {
    try {
        return HandleRequest(runtime, request, ip, body_file);
    } catch (const std::invalid_argument&) {
        return JsonResponse(request, http::status::bad_request,
                            Json{{"error", "INVALID_REQUEST"}});
    } catch (const std::domain_error& error) {
        const auto status = std::string_view(error.what()) == "Permission denied"
                                ? http::status::forbidden
                                : http::status::not_found;
        return ErrorResponse(request, status, error.what());
    } catch (const std::exception& error) {
        std::cerr << "Request failed: " << error.what() << '\n';
        return ErrorResponse(request, http::status::internal_server_error,
                             "Internal server error");
    } catch (...) {
        // 非标准异常也必须就地消化：这是 worker 线程，漏出去就是进程终止。
        std::cerr << "Request failed with an unknown exception\n";
        return ErrorResponse(request, http::status::internal_server_error,
                             "Internal server error");
    }
}

}  // namespace dms::server
