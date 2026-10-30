// SPDX-License-Identifier: GPL-3.0-or-later
#include "dms/storage.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace dms {
namespace {

std::filesystem::path Utf8Path(const std::string& value) {
    return std::filesystem::u8path(value);
}

class PendingFileLock {
public:
    explicit PendingFileLock(const std::filesystem::path& path) {
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0640);
        if (descriptor_ < 0) {
            throw std::runtime_error("Cannot open pending delete lock file");
        }
        if (::flock(descriptor_, LOCK_EX) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("Cannot lock pending delete file");
        }
    }

    ~PendingFileLock() {
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
    }

    PendingFileLock(const PendingFileLock&) = delete;
    PendingFileLock& operator=(const PendingFileLock&) = delete;

private:
    int descriptor_ = -1;
};

std::string JsonString(const nlohmann::json& value, const char* name) {
    if (!value.contains(name) || !value.at(name).is_string()) {
        throw std::runtime_error(std::string("Missing or invalid config field: ") + name);
    }
    return value.at(name).get<std::string>();
}

std::string CsvField(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (const char ch : value) {
        if (ch == '"') escaped.push_back('"');
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

bool IsWithin(const std::filesystem::path& root,
              const std::filesystem::path& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

std::string Trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::unordered_set<std::string> LoadExtensionList(
    const std::filesystem::path& file, bool allow_empty) {
    const auto status = std::filesystem::symlink_status(file);
    if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error(
            "Upload extension list must be a regular file: " + file.string());
    }

    std::ifstream input(file);
    if (!input) {
        throw std::runtime_error("Cannot read upload extension list: " +
                                 file.string());
    }

    std::unordered_set<std::string> extensions;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        auto entry = Trim(std::move(line));
        if (entry.empty() || entry.front() == '#') continue;
        if (entry.front() == '.') entry.erase(entry.begin());
        if (entry.empty() ||
            !std::all_of(entry.begin(), entry.end(), [](unsigned char ch) {
                return (ch >= 'a' && ch <= 'z') ||
                       (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9');
            })) {
            throw std::runtime_error(
                "Invalid extension on line " + std::to_string(line_number) +
                " of " + file.string());
        }
        std::transform(entry.begin(), entry.end(), entry.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        extensions.insert(std::move(entry));
    }
    if (input.bad()) {
        throw std::runtime_error("Error reading upload extension list: " +
                                 file.string());
    }
    if (!allow_empty && extensions.empty()) {
        throw std::runtime_error("Upload allowlist must not be empty: " +
                                 file.string());
    }
    return extensions;
}

bool IsProtectedExtension(const std::string& extension) {
    static const std::unordered_set<std::string> protected_extensions{
        "app", "apk", "asp", "aspx", "bash", "bat", "cgi", "cmd", "com",
        "deb", "desktop", "dylib", "dll", "exe", "fish", "hta", "jar",
        "js", "lnk", "msi", "php", "pl", "ps1", "psm1", "py", "pyw",
        "rb", "reg", "rpm", "scr", "sh", "so", "vbs", "wsf", "zsh"};
    return protected_extensions.contains(extension);
}

}  // namespace

Config Config::Load(const std::filesystem::path& file) {
    std::ifstream input(file);
    if (!input) {
        throw std::runtime_error("Cannot open config file: " + file.string());
    }
    nlohmann::json document;
    input >> document;
    if (!document.is_object()) {
        throw std::runtime_error("config.json must contain an object");
    }

    Config config;
    config.root = Utf8Path(JsonString(document, "root"));
    if (document.contains("listen_address")) {
        config.listen_address = JsonString(document, "listen_address");
    }
    if (!document.contains("port") || !document.at("port").is_number_unsigned()) {
        throw std::runtime_error("Missing or invalid config field: port");
    }
    const auto port = document.at("port").get<unsigned>();
    if (port == 0 || port > 65535) {
        throw std::runtime_error("config port must be between 1 and 65535");
    }
    config.port = static_cast<std::uint16_t>(port);
    config.refresh_token = JsonString(document, "refresh_token");
    if (config.refresh_token.size() < 32) {
        throw std::runtime_error("refresh_token must contain at least 32 characters");
    }
    if (document.contains("tls_cert_file")) {
        if (!document.at("tls_cert_file").is_string()) {
            throw std::runtime_error("tls_cert_file must be a string");
        }
        config.tls_cert_file = document.at("tls_cert_file").get<std::string>();
    }
    if (document.contains("tls_key_file")) {
        if (!document.at("tls_key_file").is_string()) {
            throw std::runtime_error("tls_key_file must be a string");
        }
        config.tls_key_file = document.at("tls_key_file").get<std::string>();
    }
    if (document.contains("tls_ca_file")) {
        if (!document.at("tls_ca_file").is_string()) {
            throw std::runtime_error("tls_ca_file must be a string");
        }
        config.tls_ca_file = document.at("tls_ca_file").get<std::string>();
    }
    if (config.tls_cert_file.empty() != config.tls_key_file.empty()) {
        throw std::runtime_error(
            "tls_cert_file and tls_key_file must either both be set or both be empty");
    }
    if (!document.contains("users") || !document.at("users").is_array()) {
        throw std::runtime_error("Missing or invalid config field: users");
    }
    for (const auto& item : document.at("users")) {
        if (!item.is_object() || !item.contains("groups") ||
            !item.at("groups").is_array()) {
            throw std::runtime_error("Each config user requires a groups array");
        }
        UserAccount account;
        account.user = JsonString(item, "user");
        account.pass_salt = JsonString(item, "pass_salt");
        account.pass_hash = JsonString(item, "pass_hash");
        for (const auto& group : item.at("groups")) {
            if (!group.is_string()) {
                throw std::runtime_error("User groups must be strings");
            }
            account.groups.push_back(group.get<std::string>());
        }
        account.admin = item.value("admin", false);
        if (account.user.empty() || account.user.find_first_of(",\r\n") != std::string::npos) {
            throw std::runtime_error("Invalid configured user name");
        }
        if (FromHex(account.pass_salt).size() < 16 ||
            FromHex(account.pass_hash).size() != 32) {
            throw std::runtime_error("Invalid PBKDF2 salt/hash for user: " + account.user);
        }
        config.users.push_back(std::move(account));
    }
    if (config.users.empty()) {
        throw std::runtime_error("At least one user must be configured");
    }
    return config;
}

std::filesystem::path Config::Etc() const { return root / "etc"; }
std::filesystem::path Config::FileStore() const { return root / "filestore"; }
std::filesystem::path Config::VersionStore() const { return root / "version_store"; }
std::filesystem::path Config::Logs() const { return root / "logs"; }
std::filesystem::path Config::PermissionsFile() const {
    return Etc() / "permissions.csv";
}
std::filesystem::path Config::UploadAllowlistFile() const {
    return Etc() / "upload-allowlist.txt";
}
std::filesystem::path Config::UploadDenylistFile() const {
    return Etc() / "upload-denylist.txt";
}
std::filesystem::path Config::PendingFile() const {
    return root / "admin" / "pending_delete.csv";
}
std::filesystem::path Config::MetadataFile() const {
    return Etc() / "file_metadata.json";
}

UploadExtensionPolicy::UploadExtensionPolicy(
    const std::filesystem::path& allowlist_file,
    const std::filesystem::path& denylist_file)
    : allowed_(LoadExtensionList(allowlist_file, false)),
      denied_(LoadExtensionList(denylist_file, true)) {}

bool UploadExtensionPolicy::IsAllowed(const std::string& filename) const {
    auto extension = std::filesystem::path(filename).extension().string();
    if (extension.size() < 2 || extension.front() != '.') return false;
    extension.erase(extension.begin());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return !IsProtectedExtension(extension) && !denied_.contains(extension) &&
           allowed_.contains(extension);
}

std::vector<std::string> UploadExtensionPolicy::AllowedExtensions() const {
    std::vector<std::string> extensions;
    for (const auto& extension : allowed_) {
        if (!IsProtectedExtension(extension) && !denied_.contains(extension)) {
            extensions.push_back(extension);
        }
    }
    std::sort(extensions.begin(), extensions.end());
    return extensions;
}

std::vector<unsigned char> FromHex(const std::string& hex) {
    if (hex.empty() || hex.size() % 2 != 0) {
        return {};
    }
    std::vector<unsigned char> result(hex.size() / 2);
    for (std::size_t i = 0; i < result.size(); ++i) {
        unsigned int value = 0;
        std::istringstream stream(hex.substr(i * 2, 2));
        stream >> std::hex >> value;
        if (!stream || value > 255) {
            return {};
        }
        result[i] = static_cast<unsigned char>(value);
    }
    return result;
}

std::string ToHex(const unsigned char* data, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        output << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return output.str();
}

bool AuthManager::VerifyPassword(const std::string& password,
                                 const std::string& salt_hex,
                                 const std::string& hash_hex) {
    const auto salt = FromHex(salt_hex);
    const auto expected = FromHex(hash_hex);
    if (salt.size() < 16 || expected.size() != 32) {
        return false;
    }
    std::array<unsigned char, 32> actual{};
    constexpr int iterations = 600000;
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()), iterations,
                          EVP_sha256(), static_cast<int>(actual.size()),
                          actual.data()) != 1) {
        throw std::runtime_error("PBKDF2 password verification failed");
    }
    return CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

std::string AuthManager::HashPassword(const std::string& password,
                                      const std::string& salt_hex) {
    const auto salt = FromHex(salt_hex);
    if (salt.size() < 16) {
        throw std::invalid_argument("PBKDF2 salt must be at least 16 bytes");
    }
    std::array<unsigned char, 32> hash{};
    constexpr int iterations = 600000;
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()), iterations,
                          EVP_sha256(), static_cast<int>(hash.size()),
                          hash.data()) != 1) {
        throw std::runtime_error("PBKDF2 password hashing failed");
    }
    return ToHex(hash.data(), hash.size());
}

std::string MakeRandomToken() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("Cryptographic random generator failed");
    }
    return ToHex(bytes.data(), bytes.size());
}

AuthManager::AuthManager(std::vector<UserAccount> users) {
    for (auto& user : users) {
        const auto username = user.user;
        if (!users_.emplace(username, std::move(user)).second) {
            throw std::runtime_error("Duplicate configured user");
        }
    }
}

std::optional<nlohmann::json> AuthManager::Login(const std::string& user,
                                                 const std::string& password) {
    const auto found = users_.find(user);
    if (found == users_.end() ||
        !VerifyPassword(password, found->second.pass_salt, found->second.pass_hash)) {
        return std::nullopt;
    }
    const auto token = MakeRandomToken();
    {
        std::lock_guard lock(mutex_);
        tokens_.insert_or_assign(token, user);
    }
    return nlohmann::json{{"token", token},
                          {"user", user},
                          {"groups", found->second.groups}};
}

std::optional<UserAccount> AuthManager::Resolve(const std::string& token) const {
    std::lock_guard lock(mutex_);
    const auto session = tokens_.find(token);
    if (session == tokens_.end()) {
        return std::nullopt;
    }
    const auto user = users_.find(session->second);
    return user == users_.end() ? std::nullopt
                                : std::optional<UserAccount>(user->second);
}

void AuthManager::Logout(const std::string& token) {
    std::lock_guard lock(mutex_);
    tokens_.erase(token);
}

AuditLog::AuditLog(std::filesystem::path directory)
    : directory_(std::move(directory)), worker_([this] { Run(); }) {}

AuditLog::~AuditLog() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    worker_.join();
}

std::string UtcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setw(3) << std::setfill('0') << milliseconds.count()
           << 'Z';
    return output.str();
}

void AuditLog::Write(std::string user, std::string ip, std::string operation,
                     std::string path, std::string detail) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            throw std::runtime_error("Audit logger is stopping");
        }
        queue_.push_back({{"ts", UtcTimestamp()}, {"user", std::move(user)},
                          {"ip", std::move(ip)}, {"op", std::move(operation)},
                          {"path", std::move(path)}, {"detail", std::move(detail)}});
    }
    ready_.notify_one();
}

void AuditLog::Run() {
    for (;;) {
        nlohmann::json entry;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) {
                return;
            }
            entry = std::move(queue_.front());
            queue_.pop_front();
        }
        const auto date = entry.at("ts").get<std::string>().substr(0, 10);
        const auto file = directory_ / ("audit-" + date + ".jsonl");
        std::ofstream output(file, std::ios::binary | std::ios::app);
        if (!output) {
            std::cerr << "Unable to open audit log: " << file << '\n';
            continue;
        }
        output << entry.dump() << '\n';
        output.flush();
        if (!output) {
            std::cerr << "Unable to write audit log: " << file << '\n';
        }
    }
}

PendingDeleteStore::PendingDeleteStore(std::filesystem::path file)
    : file_(std::move(file)) {
    Reload();
}

void PendingDeleteStore::Reload() {
    std::ifstream input(file_);
    if (!input) {
        throw std::runtime_error("Cannot open pending delete file: " + file_.string());
    }
    std::vector<PendingDelete> next;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        const auto fields = ParseCsvRow(line);
        if (fields.size() != 4) {
            throw std::runtime_error("pending_delete.csv line " +
                                     std::to_string(line_number) +
                                     " must contain 4 columns");
        }
        int status = -1;
        const auto parsed = std::from_chars(fields[2].data(),
                                            fields[2].data() + fields[2].size(),
                                            status);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != fields[2].data() + fields[2].size()) {
            std::cerr << "Ignoring invalid pending-delete status on line "
                      << line_number << '\n';
            continue;
        }
        if (status < 0 || status > 2) {
            std::cerr << "Ignoring illegal pending-delete status on line "
                      << line_number << '\n';
            continue;
        }
        next.push_back({fields[0], NormalizeLogicalPath(fields[1]), status, fields[3]});
    }
    std::lock_guard lock(mutex_);
    records_ = std::move(next);
}

void PendingDeleteStore::Add(const PendingDelete& pending) {
    const auto normalized = NormalizeLogicalPath(pending.path);
    std::lock_guard lock(mutex_);
    PendingFileLock file_lock(file_.string() + ".lock");
    std::ofstream output(file_, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("Cannot append pending delete file: " + file_.string());
    }
    output << CsvField(pending.user) << ',' << CsvField(normalized) << ",0,"
           << CsvField(pending.requested_at) << '\n';
    output.flush();
    if (!output) {
        throw std::runtime_error("Unable to write pending delete record");
    }
    records_.push_back({pending.user, normalized, 0, pending.requested_at});
}

void PendingDeleteStore::RemoveResolved() {
    std::lock_guard lock(mutex_);
    std::vector<PendingDelete> unresolved;
    for (const auto& record : records_) {
        if (record.status == 0) {
            unresolved.push_back(record);
        }
    }
    const auto temporary = file_.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Cannot write temporary pending delete file");
        }
        for (const auto& record : unresolved) {
            output << CsvField(record.user) << ',' << CsvField(record.path) << ','
                   << record.status << ',' << CsvField(record.requested_at) << '\n';
        }
        output.flush();
        if (!output) {
            throw std::runtime_error("Unable to flush pending delete file");
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, file_, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::filesystem::filesystem_error("Replacing pending delete file",
                                                temporary, file_, error);
    }
    records_ = std::move(unresolved);
}

std::vector<PendingDelete> PendingDeleteStore::Records() const {
    std::lock_guard lock(mutex_);
    return records_;
}

bool PendingDeleteStore::IsHidden(const std::string& logical_path) const {
    const auto normalized = NormalizeLogicalPath(logical_path);
    std::lock_guard lock(mutex_);
    for (const auto& record : records_) {
        if (record.status != 0) {
            continue;
        }
        if (normalized == record.path ||
            (normalized.size() > record.path.size() &&
             normalized.compare(0, record.path.size(), record.path) == 0 &&
             normalized[record.path.size()] == '/')) {
            return true;
        }
    }
    return false;
}

FileLocks::Guard FileLocks::Acquire(const std::string& path) {
    const auto shard = std::hash<std::string>{}(path) % shards_.size();
    return Guard(shards_[shard]);
}

void TreeIndex::Build(const std::filesystem::path& root) {
    std::unordered_map<std::string, std::vector<TreeNode>> next;
    next.emplace("/", std::vector<TreeNode>{});
    std::function<void(const std::filesystem::path&, const std::string&)> walk;
    walk = [&next, &walk](const std::filesystem::path& directory,
                          const std::string& logical_directory) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_symlink()) {
                continue;
            }
            const auto name_u8 = entry.path().filename().u8string();
            const std::string name(reinterpret_cast<const char*>(name_u8.data()),
                                   name_u8.size());
            const auto path = logical_directory == "/" ? "/" + name
                                                        : logical_directory + "/" + name;
            const bool is_directory = entry.is_directory();
            next[logical_directory].push_back({name, path, is_directory});
            if (is_directory) {
                next.try_emplace(path);
                walk(entry.path(), path);
            }
        }
    };
    walk(std::filesystem::weakly_canonical(root), "/");
    std::unique_lock lock(mutex_);
    children_ = std::move(next);
}

std::vector<TreeNode> TreeIndex::Children(const std::string& directory) const {
    const auto normalized = NormalizeLogicalPath(directory);
    std::shared_lock lock(mutex_);
    const auto found = children_.find(normalized);
    return found == children_.end() ? std::vector<TreeNode>{} : found->second;
}

void TreeIndex::Upsert(const std::string& path, bool directory) {
    const auto normalized = NormalizeLogicalPath(path);
    if (normalized == "/") {
        throw std::invalid_argument("Cannot index the storage root as a node");
    }
    const auto separator = normalized.find_last_of('/');
    const auto parent = separator == 0 ? "/" : normalized.substr(0, separator);
    const auto name = normalized.substr(separator + 1);
    std::unique_lock lock(mutex_);
    const auto parent_nodes = children_.find(parent);
    if (parent_nodes == children_.end()) {
        throw std::runtime_error("Parent directory is missing from the tree index");
    }
    auto& nodes = parent_nodes->second;
    std::erase_if(nodes, [&normalized](const TreeNode& node) {
        return node.path == normalized;
    });
    nodes.push_back({name, normalized, directory});
    if (directory) {
        children_.try_emplace(normalized);
    }
}

void TreeIndex::Remove(const std::string& path) {
    const auto normalized = NormalizeLogicalPath(path);
    if (normalized == "/") {
        throw std::invalid_argument("Cannot remove the storage root from the tree index");
    }
    const auto separator = normalized.find_last_of('/');
    const auto parent = separator == 0 ? "/" : normalized.substr(0, separator);
    const auto descendant_prefix = normalized + "/";
    std::unique_lock lock(mutex_);
    if (const auto found = children_.find(parent); found != children_.end()) {
        std::erase_if(found->second, [&normalized](const TreeNode& node) {
            return node.path == normalized;
        });
    }
    std::erase_if(children_, [&normalized, &descendant_prefix](const auto& entry) {
        return entry.first == normalized ||
               entry.first.compare(0, descendant_prefix.size(), descendant_prefix) == 0;
    });
}

void TreeIndex::Move(const std::string& from, const std::string& to,
                     bool directory) {
    const auto old_path = NormalizeLogicalPath(from);
    const auto new_path = NormalizeLogicalPath(to);
    const auto old_separator = old_path.find_last_of('/');
    const auto new_separator = new_path.find_last_of('/');
    const auto old_parent = old_separator == 0 ? "/" : old_path.substr(0, old_separator);
    const auto new_parent = new_separator == 0 ? "/" : new_path.substr(0, new_separator);
    const auto new_name = new_path.substr(new_separator + 1);
    const auto old_prefix = old_path + "/";
    const auto new_prefix = new_path + "/";

    std::unique_lock lock(mutex_);
    auto old_parent_nodes = children_.find(old_parent);
    auto new_parent_nodes = children_.find(new_parent);
    if (old_parent_nodes == children_.end() || new_parent_nodes == children_.end()) {
        throw std::runtime_error("Rename parent directory is missing from the tree index");
    }
    std::vector<std::pair<std::string, std::vector<TreeNode>>> subtree;
    for (const auto& [key, value] : children_) {
        if (key == old_path ||
            key.compare(0, old_prefix.size(), old_prefix) == 0) {
            subtree.emplace_back(key, value);
        }
    }
    std::erase_if(old_parent_nodes->second, [&old_path](const TreeNode& node) {
        return node.path == old_path;
    });
    for (const auto& [key, value] : subtree) {
        (void)value;
        children_.erase(key);
    }
    auto& new_nodes = children_.at(new_parent);
    std::erase_if(new_nodes, [&new_path](const TreeNode& node) {
        return node.path == new_path;
    });
    new_nodes.push_back({new_name, new_path, directory});
    for (auto& [key, nodes] : subtree) {
        const auto suffix = key == old_path ? std::string{} : key.substr(old_path.size());
        const auto moved_key = new_path + suffix;
        for (auto& node : nodes) {
            if (node.path.compare(0, old_prefix.size(), old_prefix) == 0) {
                node.path = new_prefix + node.path.substr(old_prefix.size());
            }
        }
        children_.insert_or_assign(moved_key, std::move(nodes));
    }
    if (directory && !children_.contains(new_path)) {
        children_.emplace(new_path, std::vector<TreeNode>{});
    }
}

std::vector<FileLocks::Guard> FileLocks::AcquireAll() {
    std::vector<Guard> guards;
    guards.reserve(shards_.size());
    for (auto& shard : shards_) {
        guards.emplace_back(shard);
    }
    return guards;
}

std::filesystem::path ResolveSafePath(const std::filesystem::path& root,
                                      const std::string& logical_path) {
    const auto normalized = NormalizeLogicalPath(logical_path);
    const auto canonical_root = std::filesystem::weakly_canonical(root);
    auto candidate = canonical_root;
    std::size_t start = 1;
    while (start <= normalized.size()) {
        const auto end = normalized.find('/', start);
        const auto component = normalized.substr(start, end == std::string::npos
                                                           ? std::string::npos
                                                           : end - start);
        if (!component.empty()) {
            candidate /= Utf8Path(component);
            std::error_code error;
            const auto status = std::filesystem::symlink_status(candidate, error);
            if (!error && std::filesystem::is_symlink(status)) {
                throw std::invalid_argument("Symbolic links are not allowed in storage paths");
            }
            error.clear();
            const auto resolved = std::filesystem::weakly_canonical(candidate, error);
            if (error || !IsWithin(canonical_root, resolved)) {
                throw std::invalid_argument("Storage path escapes its configured root");
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return candidate;
}

}  // namespace dms
