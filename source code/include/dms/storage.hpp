#pragma once

#include "dms/permission_provider.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace dms {

struct UserAccount {
    std::string user;
    std::string pass_salt;
    std::string pass_hash;
    std::vector<std::string> groups;
    bool admin = false;
};

struct Config {
    std::filesystem::path root;
    std::string listen_address = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string refresh_token;
    std::string tls_cert_file;
    std::string tls_key_file;
    std::string tls_ca_file;
    std::vector<UserAccount> users;

    static Config Load(const std::filesystem::path& file);
    std::filesystem::path Etc() const;
    std::filesystem::path FileStore() const;
    std::filesystem::path VersionStore() const;
    std::filesystem::path Logs() const;
    std::filesystem::path PermissionsFile() const;
    std::filesystem::path PendingFile() const;
    std::filesystem::path MetadataFile() const;
};

class AuthManager {
public:
    explicit AuthManager(std::vector<UserAccount> users);
    std::optional<nlohmann::json> Login(const std::string& user,
                                        const std::string& password);
    std::optional<UserAccount> Resolve(const std::string& token) const;
    void Logout(const std::string& token);
    static bool VerifyPassword(const std::string& password,
                               const std::string& salt_hex,
                               const std::string& hash_hex);
    static std::string HashPassword(const std::string& password,
                                    const std::string& salt_hex);

private:
    std::unordered_map<std::string, UserAccount> users_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> tokens_;
};

class AuditLog {
public:
    explicit AuditLog(std::filesystem::path directory);
    ~AuditLog();
    void Write(std::string user, std::string ip, std::string operation,
               std::string path, std::string detail);

private:
    void Run();
    std::filesystem::path directory_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<nlohmann::json> queue_;
    bool stopping_ = false;
    std::thread worker_;
};

struct PendingDelete {
    std::string user;
    std::string path;
    int status = 0;
    std::string requested_at;
};

class PendingDeleteStore {
public:
    explicit PendingDeleteStore(std::filesystem::path file);
    void Reload();
    void Add(const PendingDelete& pending);
    void RemoveResolved();
    std::vector<PendingDelete> Records() const;
    bool IsHidden(const std::string& logical_path) const;
    const std::filesystem::path& File() const { return file_; }

private:
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::vector<PendingDelete> records_;
};

class FileLocks {
public:
    using Guard = std::unique_lock<std::mutex>;
    Guard Acquire(const std::string& path);
    std::vector<Guard> AcquireAll();

private:
    std::array<std::mutex, 256> shards_;
};

struct TreeNode {
    std::string name;
    std::string path;
    bool directory = false;
};

class TreeIndex {
public:
    void Build(const std::filesystem::path& root);
    std::vector<TreeNode> Children(const std::string& directory) const;
    void Upsert(const std::string& path, bool directory);
    void Remove(const std::string& path);
    void Move(const std::string& from, const std::string& to, bool directory);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::vector<TreeNode>> children_;
};

std::filesystem::path ResolveSafePath(const std::filesystem::path& root,
                                      const std::string& logical_path);
std::string UtcTimestamp();
std::string MakeRandomToken();
std::string ToHex(const unsigned char* data, std::size_t size);
std::vector<unsigned char> FromHex(const std::string& hex);

}  // namespace dms
