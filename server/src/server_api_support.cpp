// SPDX-License-Identifier: GPL-3.0-or-later
#include "server_api_support.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace dms::server {

bool ConstantTimeEquals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        difference |= static_cast<unsigned char>(left[i]) ^
                      static_cast<unsigned char>(right[i]);
    }
    return difference == 0;
}

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

Response MakeResponse(const http::request<http::string_body>& request,
                      http::status status, std::vector<unsigned char> body,
                      const std::string& content_type) {
    http::response<http::string_body> response{status, request.version()};
    response.set(http::field::server, "dms-server");
    response.set(http::field::content_type, content_type);
    response.set("X-Content-Type-Options", "nosniff");
    response.set(http::field::cache_control, "no-store");
    response.keep_alive(request.keep_alive());
    response.body().assign(body.begin(), body.end());
    response.prepare_payload();
    return Response{http::message_generator(std::move(response)), {}};
}

Response MakeFileResponse(const http::request<http::string_body>& request,
                          http::status status,
                          const std::filesystem::path& file,
                          const std::string& content_type,
                          const std::filesystem::path& cleanup_file,
                          const std::string& download_name) {
    http::response<http::file_body> response{status, request.version()};
    response.set(http::field::server, "dms-server");
    response.set(http::field::content_type, content_type);
    response.set("X-Content-Type-Options", "nosniff");
    response.set(http::field::cache_control, "no-store");
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

Response JsonResponse(const http::request<http::string_body>& request,
                     http::status status, const Json& body) {
    // replace：所有出口统一用非严格模式，杜绝非法 UTF-8 把 dump 打成异常。
    const auto serialized =
        body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
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

int PermissionSaveMode(Runtime& runtime, const UserAccount& user,
                       const std::string& path) {
    return runtime.permissions.Check(user.user, user.groups, path,
                                     Action::WRITE).save_mode ==
                   SaveMode::CREATE_NEW
               ? 1
               : 0;
}

void CheckVisible(Runtime& runtime, const std::string& path) {
    if (runtime.pending.IsHidden(path)) {
        throw std::domain_error("Path is pending deletion");
    }
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

}  // namespace dms::server
