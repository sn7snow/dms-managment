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
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#else
#include <conio.h>
#endif

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;
using Response = http::response<http::vector_body<unsigned char>>;

namespace dms {
namespace {

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

    // Mailboxes are session-scoped and in-memory; logout drops all queued events.
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
        if (found == mailboxes_.end()) {
            return Json::array();
        }
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
          permissions(config.PermissionsFile()),
          pending(config.PendingFile()),
          auth(config.users),
          audit(config.Logs()) {
        std::filesystem::create_directories(config.FileStore());
        std::filesystem::create_directories(config.VersionStore());
        std::filesystem::create_directories(config.Logs());
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

std::string UrlDecode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            decoded.push_back(' ');
        } else if (value[i] == '%') {
            if (i + 2 >= value.size()) {
                throw std::invalid_argument("Invalid URL encoding");
            }
            unsigned int byte = 0;
            const auto result = std::from_chars(value.data() + i + 1,
                                                value.data() + i + 3, byte, 16);
            if (result.ec != std::errc{} || result.ptr != value.data() + i + 3 ||
                byte == 0) {
                throw std::invalid_argument("Invalid URL encoding");
            }
            decoded.push_back(static_cast<char>(byte));
            i += 2;
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

std::map<std::string, std::string> ParseQuery(std::string_view target) {
    std::map<std::string, std::string> query;
    const auto question = target.find('?');
    if (question == std::string_view::npos) {
        return query;
    }
    auto remainder = target.substr(question + 1);
    while (!remainder.empty()) {
        const auto ampersand = remainder.find('&');
        const auto pair = remainder.substr(0, ampersand);
        const auto equals = pair.find('=');
        const auto key = UrlDecode(pair.substr(0, equals));
        const auto value = equals == std::string_view::npos
                               ? std::string{}
                               : UrlDecode(pair.substr(equals + 1));
        if (key.empty() || !query.emplace(key, value).second) {
            throw std::invalid_argument("Invalid or duplicate query parameter");
        }
        if (ampersand == std::string_view::npos) {
            break;
        }
        remainder.remove_prefix(ampersand + 1);
    }
    return query;
}

std::string Route(std::string_view target) {
    const auto question = target.find('?');
    return std::string(target.substr(0, question));
}

std::string RequiredQuery(const std::map<std::string, std::string>& query,
                          const std::string& name) {
    const auto found = query.find(name);
    if (found == query.end() || found->second.empty()) {
        throw std::invalid_argument("Missing query parameter: " + name);
    }
    return found->second;
}

Json ParseJson(const std::string& body) {
    try {
        const auto parsed = Json::parse(body);
        if (!parsed.is_object()) {
            throw std::invalid_argument("JSON request body must be an object");
        }
        return parsed;
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("Invalid JSON body: ") + error.what());
    }
}

std::string RequiredString(const Json& body, const char* name) {
    if (!body.contains(name) || !body.at(name).is_string()) {
        throw std::invalid_argument(std::string("Missing or invalid field: ") + name);
    }
    return body.at(name).get<std::string>();
}

Response MakeResponse(const http::request<http::string_body>& request,
                      http::status status, std::vector<unsigned char> body,
                      const std::string& content_type) {
    Response response{status, request.version()};
    response.set(http::field::server, "dms-server");
    response.set(http::field::content_type, content_type);
    response.keep_alive(request.keep_alive());
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

Response JsonResponse(const http::request<http::string_body>& request,
                     http::status status, const Json& body) {
    const auto serialized = body.dump();
    return MakeResponse(request, status,
                        std::vector<unsigned char>(serialized.begin(), serialized.end()),
                        "application/json; charset=utf-8");
}

Response ErrorResponse(const http::request<http::string_body>& request,
                       http::status status, const std::string& error) {
    return JsonResponse(request, status, Json{{"error", error}});
}

std::string BearerToken(const http::request<http::string_body>& request) {
    const auto header = request[http::field::authorization];
    constexpr std::string_view prefix = "Bearer ";
    const std::string value(header);
    if (value.size() <= prefix.size() ||
        value.compare(0, prefix.size(), prefix) != 0) {
        return {};
    }
    return value.substr(prefix.size());
}

std::optional<UserAccount> AuthenticatedUser(
    Runtime& runtime, const http::request<http::string_body>& request) {
    const auto token = BearerToken(request);
    if (token.empty()) {
        return std::nullopt;
    }
    return runtime.auth.Resolve(token);
}

void RequirePermission(Runtime& runtime, const UserAccount& user,
                       const std::string& path, Action action) {
    if (!runtime.permissions.Check(user.user, user.groups, path, action).allowed) {
        throw std::domain_error("Permission denied");
    }
}

std::string PermissionLetters(Runtime& runtime, const UserAccount& user,
                              const std::string& path) {
    const std::array<Action, 6> actions{
        Action::READ, Action::WRITE, Action::DELETE,
        Action::ADD_VERSION, Action::DEL_VERSION, Action::UPLOAD};
    std::string bits;
    bits.reserve(actions.size());
    for (const auto action : actions) {
        bits.push_back(runtime.permissions.Check(
                           user.user, user.groups, path, action).allowed
                           ? '1'
                           : '0');
    }
    return bits;
}

std::filesystem::path VersionDirectory(const Config& config,
                                       const std::string& logical_path) {
    const auto normalized = NormalizeLogicalPath(logical_path);
    return ResolveSafePath(config.VersionStore(), normalized);
}

std::filesystem::path VersionFile(const Config& config,
                                  const std::string& logical_path, int revision) {
    const auto normalized = NormalizeLogicalPath(logical_path);
    const auto relative = std::filesystem::u8path(normalized.substr(1));
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
    const auto filename = std::filesystem::u8path(
        NormalizeLogicalPath(logical_path).substr(1)).filename();
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

void CheckVisible(Runtime& runtime, const std::string& path) {
    if (runtime.pending.IsHidden(path)) {
        throw std::domain_error("Path is pending deletion");
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
    const auto document = Json::parse(input);
    if (!document.is_object()) {
        throw std::runtime_error("Metadata file must contain a JSON object");
    }
    return document;
}

void WriteMetadataDocument(const Config& config, const Json& document) {
    const auto file = config.MetadataFile();
    std::filesystem::create_directories(file.parent_path());
    const auto temporary = file.string() + "." + MakeRandomToken() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Cannot write metadata file: " + temporary);
    }
    output << document.dump(2) << '\n';
    output.flush();
    if (!output) {
        throw std::runtime_error("Cannot flush metadata file: " + temporary);
    }
    output.close();
    std::filesystem::rename(temporary, file);
}

std::string MetadataOwner(Runtime& runtime, const std::string& path) {
    std::lock_guard lock(runtime.metadata_mutex);
    const auto document = ReadMetadataDocument(runtime.config);
    const auto found = document.find(path);
    if (found == document.end() || !found.value().is_object()) {
        return "unknown";
    }
    return found.value().value("owner", "unknown");
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

std::string FormatUtcTime(const std::filesystem::file_time_type& time_point) {
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time_point - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    const auto timestamp = std::chrono::system_clock::to_time_t(system_time);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &timestamp);
#else
    gmtime_r(&timestamp, &tm);
#endif
    std::ostringstream formatted;
    formatted << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return formatted.str();
}

std::string FormatUtcTime(
    const std::chrono::system_clock::time_point& time_point) {
    const auto timestamp = std::chrono::system_clock::to_time_t(time_point);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &timestamp);
#else
    gmtime_r(&timestamp, &tm);
#endif
    std::ostringstream formatted;
    formatted << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return formatted.str();
}

std::string FormatShanghaiTime(
    const std::chrono::system_clock::time_point& time_point) {
    constexpr auto shanghai_offset = std::chrono::hours(8);
    const auto timestamp = std::chrono::system_clock::to_time_t(
        time_point + shanghai_offset);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &timestamp);
#else
    gmtime_r(&timestamp, &tm);
#endif
    std::ostringstream formatted;
    formatted << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return formatted.str();
}

template <typename Stream>
std::string RemoteAddress(const Stream& stream) {
    boost::system::error_code error;
    const auto endpoint = stream.lowest_layer().remote_endpoint(error);
    return error ? std::string{} : endpoint.address().to_string();
}

Response HandleRequest(Runtime& runtime,
                       const http::request<http::string_body>& request,
                       const std::string& ip) {
    const std::string target(request.target());
    const auto route = Route(target);
    const auto query = ParseQuery(target);
    const auto method = request.method();

    if (route == "/api/internal/refresh" && method == http::verb::post) {
        const std::string supplied(request["X-DMS-Refresh-Token"]);
        if (supplied.empty() || supplied != runtime.config.refresh_token) {
            return ErrorResponse(request, http::status::unauthorized, "Unauthorized");
        }
        runtime.Reload();
        return JsonResponse(request, http::status::ok, Json{{"ok", true}});
    }

    if (route == "/api/login" && method == http::verb::post) {
        const auto body = ParseJson(request.body());
        const auto username = RequiredString(body, "user");
        const auto password = RequiredString(body, "pass");
        const auto result = runtime.auth.Login(username, password);
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
                             {"owner", MetadataOwner(runtime, node.path)},
                             {"perms", PermissionLetters(runtime, *user, node.path)},
                             {"locked_by", occupied_by},
                             {"locked_at", locked_at},
                             {"version", version_count > 0
                                             ? "v" + std::to_string(version_count)
                                             : std::string{}}});
        }
        return JsonResponse(request, http::status::ok,
                            Json{{"nodes", nodes},
                                 {"directory_perms",
                                  PermissionLetters(runtime, *user, path)}});
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
        std::lock_guard lock(runtime.document_locks_mutex);
        const auto now = std::chrono::steady_clock::now();
        auto found = runtime.document_locks.find(path);
        if (found != runtime.document_locks.end() &&
            now - found->second.heartbeat >= std::chrono::minutes(30)) {
            runtime.document_locks.erase(found);
            runtime.notifications.PublishRelease(path);
            found = runtime.document_locks.end();
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
                                     {"locked_at", locked_at_text}});
        }
        return JsonResponse(request, http::status::ok,
                            Json{{"status", "read_only"},
                                 {"occupied_by", found->second.user},
                                 {"locked_at",
                                  FormatShanghaiTime(found->second.locked_at)}});
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
        const auto path = NormalizeLogicalPath(RequiredString(body, "path"));
        std::lock_guard lock(runtime.document_locks_mutex);
        const auto found = runtime.document_locks.find(path);
        if (found == runtime.document_locks.end() ||
            found->second.user != user->user ||
            std::chrono::steady_clock::now() - found->second.heartbeat >=
                std::chrono::minutes(30)) {
            if (found != runtime.document_locks.end() &&
                std::chrono::steady_clock::now() - found->second.heartbeat >=
                    std::chrono::minutes(30)) {
                runtime.notifications.PublishRelease(path);
                runtime.document_locks.erase(found);
            }
            return ErrorResponse(request, http::status::conflict, "OCCUPIED");
        }
        found->second.heartbeat = std::chrono::steady_clock::now();
        return JsonResponse(request, http::status::ok, Json{{"ok", true}});
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
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Cannot read file: " + file.string());
        }
        std::vector<unsigned char> bytes(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return MakeResponse(request, http::status::ok, std::move(bytes),
                            "application/octet-stream");
    }

    if (route == "/api/file" && method == http::verb::put) {
        const auto path = NormalizeLogicalPath(RequiredQuery(query, "path"));
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, path);
        std::unique_lock document_lock(runtime.document_locks_mutex);
        const auto permission = runtime.permissions.Check(user->user, user->groups,
                                                          path, Action::WRITE);
        if (!permission.allowed) {
            throw std::domain_error("Permission denied");
        }
        const auto held = runtime.document_locks.find(path);
        if (held == runtime.document_locks.end() ||
            held->second.user != user->user ||
            std::chrono::steady_clock::now() - held->second.heartbeat >=
                std::chrono::minutes(30)) {
            return ErrorResponse(request, http::status::conflict, "OCCUPIED");
        }
        const auto workspace_file = ResolveSafePath(runtime.config.FileStore(), path);
        if (!std::filesystem::is_directory(workspace_file.parent_path())) {
            return ErrorResponse(request, http::status::not_found,
                                 "Parent directory not found");
        }
        {
            std::ofstream output(workspace_file, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("Cannot write file: " + workspace_file.string());
            }
            output.write(request.body().data(),
                         static_cast<std::streamsize>(request.body().size()));
            output.flush();
            if (!output) {
                throw std::runtime_error("Unable to flush uploaded file");
            }
        }
        SaveVersion(runtime, path, workspace_file, permission.save_mode);
        const int version = LatestRevision(runtime.config, path);
        runtime.tree.Upsert(path, false);
        SetMetadataOwner(runtime, path, user->user);
        runtime.audit.Write(user->user, ip, "SAVE", path,
                            permission.save_mode == SaveMode::CREATE_NEW
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
            name.find('\0') != std::string::npos) {
            throw std::invalid_argument("Invalid upload filename");
        }
        const auto path = NormalizeLogicalPath(
            directory_path == "/" ? "/" + name : directory_path + "/" + name);
        auto guard = runtime.file_locks.Acquire(path);
        std::shared_lock visibility(runtime.pending_gate);
        CheckVisible(runtime, directory_path);
        CheckVisible(runtime, path);
        // Upload permission is intentionally independent from edit permission and locks.
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
        {
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
        }
        std::filesystem::rename(temporary, destination);
        SaveVersion(runtime, path, destination, SaveMode::CREATE_NEW);
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
        std::vector<unsigned char> bytes(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return MakeResponse(request, http::status::ok, std::move(bytes),
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
        RequirePermission(runtime, *user, from, Action::WRITE);
        RequirePermission(runtime, *user, to, Action::WRITE);
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
        const bool has_versions = std::filesystem::exists(old_versions);
        if (has_versions) {
            std::filesystem::create_directories(new_versions.parent_path());
        }
        std::filesystem::rename(source, destination);
        try {
            if (has_versions) {
                std::filesystem::rename(old_versions, new_versions);
            }
        } catch (const std::exception&) {
            std::error_code rollback_error;
            std::filesystem::rename(destination, source, rollback_error);
            if (rollback_error) {
                throw std::runtime_error("Version history move failed and workspace "
                                         "rollback failed: " +
                                         rollback_error.message());
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
            const std::string source_prefix = from + "/";
            std::vector<std::pair<std::string, HeldLock>> moved_locks;
            for (auto it = runtime.document_locks.begin();
                 it != runtime.document_locks.end();) {
                if (it->first == from ||
                    (is_directory && it->first.starts_with(source_prefix))) {
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

Response SafeHandle(Runtime& runtime,
                    const http::request<http::string_body>& request,
                    const std::string& ip) {
    try {
        return HandleRequest(runtime, request, ip);
    } catch (const std::invalid_argument& error) {
        return ErrorResponse(request, http::status::bad_request, error.what());
    } catch (const std::domain_error& error) {
        const auto status = std::string_view(error.what()) == "Permission denied"
                                ? http::status::forbidden
                                : http::status::not_found;
        return ErrorResponse(request, status, error.what());
    } catch (const std::exception& error) {
        std::cerr << "Request failed: " << error.what() << '\n';
        return ErrorResponse(request, http::status::internal_server_error,
                             "Internal server error");
    }
}

template <typename Stream, bool Tls>
class Session : public std::enable_shared_from_this<Session<Stream, Tls>> {
public:
    Session(tcp::socket socket, asio::ssl::context* tls_context,
            Runtime& runtime, asio::thread_pool& workers)
        : stream_(MakeStream(std::move(socket), tls_context)),
          runtime_(runtime), workers_(workers) {}

    void Start() {
        if constexpr (Tls) {
            stream_.async_handshake(
                asio::ssl::stream_base::server,
                beast::bind_front_handler(&Session::OnHandshake, this->shared_from_this()));
        } else {
            Read();
        }
    }

private:
    static Stream MakeStream(tcp::socket socket, asio::ssl::context* tls_context) {
        if constexpr (Tls) {
            if (tls_context == nullptr) {
                throw std::invalid_argument("TLS session requires an SSL context");
            }
            return Stream(std::move(socket), *tls_context);
        } else {
            (void)tls_context;
            return Stream(std::move(socket));
        }
    }

    void OnHandshake(beast::error_code error) {
        if (error) {
            std::cerr << "TLS handshake failed: " << error.message() << '\n';
            return Close();
        }
        Read();
    }

    void Read() {
        parser_.emplace();
        parser_->body_limit(boost::none);
        http::async_read(stream_, buffer_, *parser_,
                         beast::bind_front_handler(&Session::OnRead,
                                                   this->shared_from_this()));
    }

    void OnRead(beast::error_code error, std::size_t) {
        if (error == http::error::end_of_stream) {
            return Close();
        }
        if (error) {
            std::cerr << "HTTP read failed: " << error.message() << '\n';
            return Close();
        }
        request_ = parser_->release();
        parser_.reset();
        const auto ip = RemoteAddress(stream_);
        auto self = this->shared_from_this();
        asio::post(workers_, [self, ip] {
            auto response = SafeHandle(self->runtime_, self->request_, ip);
            asio::post(self->stream_.get_executor(),
                       [self, response = std::move(response)]() mutable {
                           self->response_ = std::move(response);
                           http::async_write(
                               self->stream_, self->response_,
                               beast::bind_front_handler(&Session::OnWrite, self));
                       });
        });
    }

    void OnWrite(beast::error_code error, std::size_t) {
        if (error) {
            std::cerr << "HTTP write failed: " << error.message() << '\n';
            return Close();
        }
        if (!response_.keep_alive()) {
            return Close();
        }
        Read();
    }

    void Close() {
        beast::error_code ignored;
        stream_.lowest_layer().shutdown(tcp::socket::shutdown_both, ignored);
        stream_.lowest_layer().close(ignored);
    }

    Stream stream_;
    beast::flat_buffer buffer_;
    std::optional<http::request_parser<http::string_body>> parser_;
    http::request<http::string_body> request_;
    Response response_;
    Runtime& runtime_;
    asio::thread_pool& workers_;
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(asio::io_context& io, tcp::endpoint endpoint, Runtime& runtime,
             asio::thread_pool& workers, asio::ssl::context* tls_context)
        : io_(io), acceptor_(asio::make_strand(io)), runtime_(runtime),
          workers_(workers), tls_context_(tls_context) {
        beast::error_code error;
        acceptor_.open(endpoint.protocol(), error);
        if (error) throw beast::system_error(error);
        acceptor_.set_option(asio::socket_base::reuse_address(true), error);
        if (error) throw beast::system_error(error);
        acceptor_.bind(endpoint, error);
        if (error) throw beast::system_error(error);
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        if (error) throw beast::system_error(error);
    }

    void Run() { Accept(); }

private:
    void Accept() {
        acceptor_.async_accept(
            asio::make_strand(io_),
            beast::bind_front_handler(&Listener::OnAccept, shared_from_this()));
    }

    void OnAccept(beast::error_code error, tcp::socket socket) {
        if (!error) {
            if (tls_context_ != nullptr) {
                using TlsStream = asio::ssl::stream<tcp::socket>;
                std::make_shared<Session<TlsStream, true>>(
                    std::move(socket), tls_context_, runtime_, workers_)->Start();
            } else {
                std::make_shared<Session<tcp::socket, false>>(
                    std::move(socket), nullptr, runtime_, workers_)->Start();
            }
        } else {
            std::cerr << "Accept failed: " << error.message() << '\n';
        }
        Accept();
    }

    asio::io_context& io_;
    tcp::acceptor acceptor_;
    Runtime& runtime_;
    asio::thread_pool& workers_;
    asio::ssl::context* tls_context_;
};

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
        }
    });
}

}  // namespace
}  // namespace dms

namespace {

std::string ReadPassword() {
#ifdef _WIN32
    std::string password;
    for (;;) {
        const int ch = _getch();
        if (ch == '\r' || ch == '\n') {
            std::cout << '\n';
            break;
        }
        if (ch == 3) {
            throw std::runtime_error("Password input cancelled");
        }
        if (ch == '\b') {
            if (!password.empty()) password.pop_back();
        } else {
            password.push_back(static_cast<char>(ch));
        }
    }
    return password;
#else
    termios original{};
    if (::tcgetattr(STDIN_FILENO, &original) != 0) {
        std::string password;
        if (!std::getline(std::cin, password)) {
            throw std::runtime_error("Unable to read password from stdin");
        }
        return password;
    }
    auto hidden = original;
    hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
        throw std::runtime_error("Unable to disable terminal echo");
    }
    std::cout << "Password: " << std::flush;
    std::string password;
    const bool read_ok = static_cast<bool>(std::getline(std::cin, password));
    const int restore_result = ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    std::cout << '\n';
    if (restore_result != 0) {
        throw std::runtime_error("Unable to restore terminal settings");
    }
    if (!read_ok) {
        throw std::runtime_error("Unable to read password");
    }
    return password;
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--hash-password") {
            const auto password = ReadPassword();
            const auto salt = dms::MakeRandomToken();
            std::cout << Json{{"pass_salt", salt},
                              {"pass_hash", dms::AuthManager::HashPassword(password, salt)}}
                             .dump(2)
                      << '\n';
            return 0;
        }
        std::filesystem::path config_file = "/opt/dms/etc/config.json";
        if (argc == 3 && std::string_view(argv[1]) == "--config") {
            config_file = argv[2];
        } else if (argc != 1) {
            std::cerr << "Usage: dms-server [--config PATH] | --hash-password\n";
            return 2;
        }
        dms::Config config = dms::Config::Load(config_file);
        std::filesystem::create_directories(config.Etc());
        std::filesystem::create_directories(config.PendingFile().parent_path());
        std::filesystem::create_directories(config.FileStore());
        std::filesystem::create_directories(config.VersionStore());
        std::filesystem::create_directories(config.Logs());
        dms::Runtime runtime(std::move(config));

        asio::io_context io(4);
        asio::thread_pool workers(32);
        const auto address = asio::ip::make_address(runtime.config.listen_address);
        std::optional<asio::ssl::context> tls_context;
        if (!runtime.config.tls_cert_file.empty()) {
            tls_context.emplace(asio::ssl::context::tls_server);
            tls_context->set_options(asio::ssl::context::default_workarounds |
                                     asio::ssl::context::no_sslv2 |
                                     asio::ssl::context::no_sslv3 |
                                     asio::ssl::context::no_tlsv1 |
                                     asio::ssl::context::no_tlsv1_1);
            tls_context->use_certificate_chain_file(runtime.config.tls_cert_file);
            tls_context->use_private_key_file(runtime.config.tls_key_file,
                                              asio::ssl::context::pem);
            if (SSL_CTX_check_private_key(tls_context->native_handle()) != 1) {
                throw std::runtime_error(
                    "TLS private key does not match the configured certificate");
            }
        } else if (!address.is_loopback()) {
            throw std::runtime_error(
                "Non-loopback listeners require tls_cert_file and tls_key_file");
        }
        const tcp::endpoint endpoint{address, runtime.config.port};
        auto listener = std::make_shared<dms::Listener>(
            io, endpoint, runtime, workers, tls_context ? &*tls_context : nullptr);
        listener->Run();
        dms::PidFile pid_file(runtime.config.root / "run" / "dms.pid");

        std::atomic_bool stop{false};
        auto monitor = dms::StartPermissionMonitor(runtime, stop);
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const beast::error_code&, int) {
            stop.store(true);
            io.stop();
        });
        std::cout << "dms-server listening on "
                  << (tls_context ? "https://" : "http://")
                  << runtime.config.listen_address
                  << ':' << runtime.config.port << '\n';
        std::array<std::thread, 3> network_threads;
        for (auto& thread : network_threads) {
            thread = std::thread([&io] { io.run(); });
        }
        io.run();
        for (auto& thread : network_threads) {
            thread.join();
        }
        stop.store(true);
        monitor.join();
        workers.join();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dms-server: " << error.what() << '\n';
        return 1;
    }
}
