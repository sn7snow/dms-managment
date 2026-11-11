// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// server_api.cpp 拆分出的共享辅助层声明。原先这些函数以匿名命名空间形式
// 内联在 server_api.cpp 中，拆分后统一在此声明，供路由、归档与存储辅助
// 模块共同使用。定义分布在：
//   - server_api_support.cpp   HTTP 解析 / 响应构造 / 鉴权 / 时间格式化
//   - server_archive.cpp       tar 归档构建
//   - server_version_store.cpp 版本目录与 file_metadata.json 元数据

#include "server_internal.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dms::server {

// ---- HTTP 解析 ----
bool ConstantTimeEquals(std::string_view left, std::string_view right);
std::string UrlDecode(std::string_view value);
std::map<std::string, std::string> ParseQuery(std::string_view target);
std::string Route(std::string_view target);
std::string RequiredQuery(const std::map<std::string, std::string>& query,
                          const std::string& name);
Json ParseJson(const std::string& body);
std::string RequiredString(const Json& body, const char* name);
void StageRequestBody(const std::filesystem::path& body_file,
                      const std::filesystem::path& staging_file);
int ParseIfMatch(const http::request<http::string_body>& request);

// ---- 响应构造 ----
Response MakeResponse(const http::request<http::string_body>& request,
                      http::status status, std::vector<unsigned char> body,
                      const std::string& content_type);
Response MakeFileResponse(const http::request<http::string_body>& request,
                          http::status status,
                          const std::filesystem::path& file,
                          const std::string& content_type,
                          const std::filesystem::path& cleanup_file = {},
                          const std::string& download_name = {});
Response JsonResponse(const http::request<http::string_body>& request,
                      http::status status, const Json& body);
Response ErrorResponse(const http::request<http::string_body>& request,
                       http::status status, const std::string& error);

// ---- 鉴权与权限 ----
std::string BearerToken(const http::request<http::string_body>& request);
std::optional<UserAccount> AuthenticatedUser(
    Runtime& runtime, const http::request<http::string_body>& request);
void RequirePermission(Runtime& runtime, const UserAccount& user,
                       const std::string& path, Action action);
std::string PermissionLetters(Runtime& runtime, const UserAccount& user,
                              const std::string& path);
int PermissionSaveMode(Runtime& runtime, const UserAccount& user,
                       const std::string& path);
void CheckVisible(Runtime& runtime, const std::string& path);

// ---- 版本存储 ----
std::filesystem::path VersionDirectory(const Config& config,
                                       const std::string& logical_path);
std::filesystem::path VersionFile(const Config& config,
                                  const std::string& logical_path, int revision);
std::vector<int> Revisions(const Config& config, const std::string& logical_path);
int LatestRevision(const Config& config, const std::string& logical_path);
void SaveVersion(Runtime& runtime, const std::string& logical_path,
                 const std::filesystem::path& workspace_file, SaveMode mode);
void TrimVersions(Runtime& runtime, const std::string& logical_path);

// ---- 元数据（file_metadata.json） ----
Json ReadMetadataDocument(const Config& config);
void WriteMetadataDocument(const Config& config, const Json& document);
void SetMetadataOwner(Runtime& runtime, const std::string& path,
                      const std::string& owner);
void RemoveMetadataPath(Runtime& runtime, const std::string& path);

// ---- 时间格式化 ----
std::string FormatUtcTime(const std::filesystem::file_time_type& time_point);
std::string FormatShanghaiTime(
    const std::chrono::system_clock::time_point& time_point);

// ---- 归档 ----
std::filesystem::path BuildTarArchive(Runtime& runtime, const UserAccount& user,
                                      const std::string& logical_path);

}  // namespace dms::server
