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

namespace dms::server {
namespace {

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

void StageRequestBody(const std::filesystem::path& body_file,
                     const std::filesystem::path& staging_file) {
    try {
        std::filesystem::rename(body_file, staging_file);
    } catch (const std::filesystem::filesystem_error& error) {
        if (error.code() != std::errc::cross_device_link) throw;
        std::filesystem::copy_file(body_file, staging_file);
    }
}

void CheckVisible(Runtime& runtime, const std::string& path);

Response MakeResponse(const http::request<http::string_body>& request,
                      http::status status, std::vector<unsigned char> body,
                      const std::string& content_type) {
    http::response<http::string_body> response{status, request.version()};
    response.set(http::field::server, "dms-server");
    response.set(http::field::content_type, content_type);
    response.keep_alive(request.keep_alive());
    response.body().assign(body.begin(), body.end());
    response.prepare_payload();
    return Response{http::message_generator(std::move(response)), {}};
}

Response MakeFileResponse(const http::request<http::string_body>& request,
                          http::status status,
                          const std::filesystem::path& file,
                          const std::string& content_type,
                          const std::filesystem::path& cleanup_file = {},
                          const std::string& download_name = {}) {
    http::response<http::file_body> response{status, request.version()};
    response.set(http::field::server, "dms-server");
    response.set(http::field::content_type, content_type);
    if (!download_name.empty()) {
        response.set(http::field::content_disposition,
                     "attachment; filename=\"" + download_name + "\"");
    }
    response.keep_alive(request.keep_alive());
    beast::error_code error;
    response.body().open(file.c_str(), beast::file_mode::scan, error);
    if (error) {
        throw std::runtime_error("Cannot open response file: " + file.string());
    }
    response.content_length(response.body().size());
    return Response{http::message_generator(std::move(response)), cleanup_file};
}

void SetTarText(std::array<char, 512>& header, std::size_t offset,
                std::size_t width, const std::string& value) {
    if (value.size() > width) {
        throw std::invalid_argument("Archive path component is too long");
    }
    std::copy(value.begin(), value.end(), header.begin() +
              static_cast<std::ptrdiff_t>(offset));
}

void SetTarOctal(std::array<char, 512>& header, std::size_t offset,
                 std::size_t width, std::uint64_t value) {
    std::ostringstream formatted;
    formatted << std::oct << std::setw(static_cast<int>(width - 1))
              << std::setfill('0') << value;
    const auto text = formatted.str();
    if (text.size() >= width) {
        throw std::invalid_argument("File is too large for tar format");
    }
    std::copy(text.begin(), text.end(), header.begin() +
              static_cast<std::ptrdiff_t>(offset));
    header[offset + width - 1] = '\0';
}

void AddTarEntry(std::ofstream& archive, const std::filesystem::path& file,
                 std::string archive_name, bool directory) {
    if (directory && !archive_name.ends_with('/')) archive_name.push_back('/');
    std::array<char, 512> header{};
    std::string name = archive_name;
    std::string prefix;
    if (name.size() > 100) {
        const auto split = name.rfind('/', 155);
        if (split == std::string::npos || split == 0 || name.size() - split - 1 > 100) {
            throw std::invalid_argument("Archive path is too long for ustar");
        }
        prefix = name.substr(0, split);
        name = name.substr(split + 1);
    }
    SetTarText(header, 0, 100, name);
    SetTarOctal(header, 100, 8, directory ? 0755 : 0644);
    SetTarOctal(header, 108, 8, 0);
    SetTarOctal(header, 116, 8, 0);
    const auto size = directory ? 0 : std::filesystem::file_size(file);
    SetTarOctal(header, 124, 12, size);
    SetTarOctal(header, 136, 12, 0);
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    header[156] = directory ? '5' : '0';
    SetTarText(header, 257, 6, "ustar");
    SetTarText(header, 263, 2, "00");
    SetTarText(header, 265, 32, "dms");
    SetTarText(header, 297, 32, "dms");
    if (!prefix.empty()) SetTarText(header, 345, 155, prefix);

    unsigned int checksum = 0;
    for (const auto byte : header) {
        checksum += static_cast<unsigned char>(byte);
    }
    std::ostringstream formatted_checksum;
    formatted_checksum << std::oct << std::setw(6) << std::setfill('0')
                       << checksum;
    const auto checksum_text = formatted_checksum.str();
    std::copy(checksum_text.begin(), checksum_text.end(), header.begin() + 148);
    header[154] = '\0';
    header[155] = ' ';
    archive.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (directory) return;

    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read archive file: " + file.string());
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), count);
        const auto read = input.gcount();
        if (read <= 0) throw std::runtime_error("Archive source changed while reading");
        archive.write(buffer.data(), read);
        remaining -= static_cast<std::uint64_t>(read);
    }
    const auto padding = static_cast<std::streamsize>((512 - size % 512) % 512);
    if (padding != 0) {
        const std::array<char, 512> zeroes{};
        archive.write(zeroes.data(), padding);
    }
}

std::filesystem::path BuildTarArchive(Runtime& runtime, const UserAccount& user,
                                      const std::string& logical_path) {
    const auto root = ResolveSafePath(runtime.config.FileStore(), logical_path);
    if (!std::filesystem::is_directory(root)) {
        throw std::invalid_argument("Archive target must be a directory");
    }
    const auto archive_path = runtime.config.root / ".http-tmp" /
                              (MakeRandomToken() + ".tar");
    try {
        std::ofstream archive(archive_path, std::ios::binary | std::ios::trunc);
        if (!archive) {
            throw std::runtime_error("Cannot create tar archive");
        }
        const std::string archive_root =
            logical_path == "/"
                ? "files"
                : std::filesystem::path(logical_path).filename().string();
        AddTarEntry(archive, root, archive_root, true);

        for (std::filesystem::recursive_directory_iterator it(root), end;
             it != end; ++it) {
            const auto entry = *it;
            if (entry.is_symlink()) {
                if (entry.is_directory()) it.disable_recursion_pending();
                continue;
            }
            const auto relative = std::filesystem::relative(entry.path(), root);
            const auto relative_text = relative.generic_string();
            const auto logical_entry = NormalizeLogicalPath(
                logical_path == "/" ? "/" + relative_text
                                    : logical_path + "/" + relative_text);
            CheckVisible(runtime, logical_entry);
            const auto is_directory = entry.is_directory();
            const auto is_file = entry.is_regular_file();
            if (!is_directory && !is_file) continue;
            const auto permission = runtime.permissions.Check(
                user.user, user.groups, logical_entry, Action::READ);
            if (!permission.allowed) {
                if (is_directory) it.disable_recursion_pending();
                continue;
            }
            AddTarEntry(archive, entry.path(),
                        archive_root + "/" + relative_text, is_directory);
        }
        const std::array<char, 1024> end_marker{};
        archive.write(end_marker.data(),
                      static_cast<std::streamsize>(end_marker.size()));
        archive.flush();
        if (!archive) throw std::runtime_error("Cannot finish tar archive");
        return archive_path;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(archive_path, ignored);
        throw;
    }
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
    const std::array<Action, 7> actions{
        Action::READ, Action::WRITE, Action::DELETE,
        Action::ADD_VERSION, Action::DEL_VERSION, Action::UPLOAD,
        Action::RENAME};
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

int ParseIfMatch(const http::request<http::string_body>& request) {
    const std::string value(request[http::field::if_match]);
    constexpr std::string_view prefix = "\"rev_";
    if (value.size() <= prefix.size() ||
        value.compare(0, prefix.size(), prefix) != 0 ||
        value.back() != '"') {
        throw std::invalid_argument("Invalid If-Match");
    }
    const auto digits = std::string_view(value).substr(
        prefix.size(), value.size() - prefix.size() - 1);
    int revision = -1;
    const auto parsed =
        std::from_chars(digits.data(), digits.data() + digits.size(), revision);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
        revision < 0) {
        throw std::invalid_argument("Invalid If-Match");
    }
    return revision;
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


Response HandleRequest(Runtime& runtime,
                       const http::request<http::string_body>& request,
                       const std::string& ip,
                       const std::filesystem::path& body_file) {
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
        const int version = LatestRevision(runtime.config, path);
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
        if (!runtime.upload_extensions.IsAllowed(name)) {
            return ErrorResponse(request, http::status::unsupported_media_type,
                                 "UNSUPPORTED_FILE_TYPE");
        }
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
        input.close();
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
                name.find('\0') != std::string::npos) {
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
    }
}

}  // namespace dms::server
