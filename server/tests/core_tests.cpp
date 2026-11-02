// SPDX-License-Identifier: GPL-3.0-or-later
#include "dms/permission_provider.hpp"
#include "dms/storage.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("dms-tests-" + dms::MakeRandomToken());
    try {
        std::filesystem::create_directories(root / "etc");
        const auto permissions_file = root / "etc" / "permissions.csv";
        {
            std::ofstream permissions(permissions_file);
            permissions << "/drawings/,@all,1,0,0,0,0,0,0\n"
                        << "/drawings/,design,1,1,0,0,0,0,1\n"
                        << "/drawings/,renamer,1,0,0,0,0,0,0,1\n"
                        << "/drawings/private/,design,1,0,0,0,0,0,0\n"
                        << "/drawings/private/,review,0,1,0,0,0,1,0\n";
        }
        dms::CsvPermissionProvider permissions(permissions_file);
        const auto read = permissions.Check("alice", {"design"},
                                            "/drawings/private/plan.dwg",
                                            dms::Action::READ);
        const auto write = permissions.Check("alice", {"design"},
                                             "/drawings/private/plan.dwg",
                                             dms::Action::WRITE);
        const auto multi_group_write = permissions.Check(
            "bob", {"design", "review"}, "/drawings/private/plan.dwg",
            dms::Action::WRITE);
        Require(read.allowed, "Most-specific group read grant should be allowed");
        Require(!write.allowed, "Most-specific group rule should override parent write");
        Require(multi_group_write.allowed, "Permissions from groups should be unioned");
        Require(multi_group_write.save_mode == dms::SaveMode::CREATE_NEW,
                "Writable CREATE_NEW policy should be preserved");
        Require(permissions.Check("alice", {"design"}, "/drawings/new.dwg",
                                  dms::Action::UPLOAD).allowed,
                "Upload permission should be independently granted");
        Require(!permissions.Check("alice", {"design"}, "/drawings/private/new.dwg",
                                   dms::Action::UPLOAD).allowed,
                "Upload permission should honor the most-specific rule");
        Require(permissions.Check("carol", {"renamer"}, "/drawings/new.dwg",
                                  dms::Action::RENAME).allowed,
                "The appended rename permission bit should be recognized");
        Require(!permissions.Check("alice", {"design"}, "/drawings/new.dwg",
                                   dms::Action::RENAME).allowed,
                "Legacy permission rows should deny rename by default");

        bool rejected = false;
        try {
            (void)dms::NormalizeLogicalPath("/drawings/../secret");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        Require(rejected, "Dot segments must be rejected");

        const std::string salt = "00112233445566778899aabbccddeeff";
        const auto hash = dms::AuthManager::HashPassword("test-password", salt);
        Require(dms::AuthManager::VerifyPassword("test-password", salt, hash),
                "PBKDF2 password verification should succeed");
        Require(!dms::AuthManager::VerifyPassword("wrong-password", salt, hash),
                "PBKDF2 password verification should reject a wrong password");

        const auto config_file = root / "etc" / "config.json";
        const auto base_config = nlohmann::json{
            {"root", root.string()},
            {"listen_address", "127.0.0.1"},
            {"port", 8080},
            {"refresh_token", std::string(64, 'a')},
            {"users", nlohmann::json::array({
                {{"user", "qa"}, {"pass_salt", salt},
                 {"pass_hash", hash}, {"groups", nlohmann::json::array({"qa"})}}
            })}
        };
        {
            std::ofstream config(config_file);
            config << base_config.dump(2);
        }
        const auto config_without_tls = dms::Config::Load(config_file);
        Require(config_without_tls.tls_cert_file.empty() &&
                    config_without_tls.tls_key_file.empty(),
                "Existing configurations should remain loopback HTTP compatible");
        Require(config_without_tls.PendingFile() ==
                    root / "admin" / "pending_delete.csv",
                "Pending approvals should be stored outside the served file store");
        Require(config_without_tls.UploadAllowlistFile() ==
                    root / "etc" / "upload-allowlist.txt" &&
                    config_without_tls.UploadDenylistFile() ==
                    root / "etc" / "upload-denylist.txt",
                "Upload extension lists should be stored under etc");

        const auto allowlist = config_without_tls.UploadAllowlistFile();
        const auto denylist = config_without_tls.UploadDenylistFile();
        {
            std::ofstream output(allowlist);
            output << "# supported file types\n"
                   << "dwg\n.PDF\nblocked\n.exe\n";
        }
        {
            std::ofstream output(denylist);
            output << " pdf \nblocked\n";
        }
        const dms::UploadExtensionPolicy extension_policy(allowlist, denylist);
        Require(extension_policy.IsAllowed("drawing.DWG"),
                "Extension allowlist should be case-insensitive");
        Require(!extension_policy.IsAllowed("report.pdf"),
                "Denylist should override an allowlist entry");
        Require(!extension_policy.IsAllowed("program.exe"),
                "Protected executable extensions must remain denied");
        Require(!extension_policy.IsAllowed("README"),
                "Files without extensions should be rejected");
        const auto effective_extensions =
            extension_policy.AllowedExtensions();
        Require(effective_extensions.size() == 1 &&
                    effective_extensions.front() == "dwg",
                "Client policy should expose only effective allowed extensions");

        {
            std::ofstream output(allowlist);
            output << "dwg;exe\n";
        }
        bool rejected_invalid_extension_list = false;
        try {
            (void)dms::UploadExtensionPolicy(allowlist, denylist);
        } catch (const std::runtime_error&) {
            rejected_invalid_extension_list = true;
        }
        Require(rejected_invalid_extension_list,
                "Malformed extension lists must fail closed");

        auto tls_config = base_config;
        tls_config["tls_cert_file"] = "/tmp/dms-test/server.crt";
        tls_config["tls_key_file"] = "/tmp/dms-test/server.key";
        tls_config["tls_ca_file"] = "/tmp/dms-test/root-ca.crt";
        {
            std::ofstream config(config_file);
            config << tls_config.dump(2);
        }
        const auto loaded_tls = dms::Config::Load(config_file);
        Require(loaded_tls.tls_cert_file == "/tmp/dms-test/server.crt" &&
                    loaded_tls.tls_key_file == "/tmp/dms-test/server.key" &&
                    loaded_tls.tls_ca_file == "/tmp/dms-test/root-ca.crt",
                "TLS certificate and key configuration should load");
        tls_config.erase("tls_key_file");
        {
            std::ofstream config(config_file);
            config << tls_config.dump(2);
        }
        bool rejected_partial_tls = false;
        try {
            (void)dms::Config::Load(config_file);
        } catch (const std::runtime_error&) {
            rejected_partial_tls = true;
        }
        Require(rejected_partial_tls,
                "A partial TLS certificate/key configuration must be rejected");

        const auto pending_file = config_without_tls.PendingFile();
        std::filesystem::create_directories(pending_file.parent_path());
        {
            std::ofstream pending(pending_file);
        }
        dms::PendingDeleteStore pending(pending_file);
        pending.Add({"alice", "/drawings/old", 0, "2026-10-26T00:00:00Z"});
        Require(pending.IsHidden("/drawings/old/plan.dwg"),
                "Pending folder deletion should hide descendant paths");
        Require(!pending.IsHidden("/drawings/older/plan.dwg"),
                "Pending folder deletion must not hide similarly prefixed siblings");

        const auto file_store = root / "filestore";
        std::filesystem::create_directories(file_store / "drawings");
        {
            std::ofstream file(file_store / "drawings" / "plan.dwg");
            file << "drawing";
        }
        dms::TreeIndex tree;
        tree.Build(file_store);
        Require(tree.Children("/").size() == 1,
                "Tree index should include the top-level directory");
        Require(tree.Children("/drawings").size() == 1,
                "Tree index should include files under directories");
        tree.Upsert("/drawings/new.dwg", false);
        Require(tree.Children("/drawings").size() == 2,
                "Tree index should accept newly created files");
        tree.Move("/drawings", "/archive", true);
        Require(tree.Children("/archive").size() == 2,
                "Tree index should preserve descendants after a directory move");
        tree.Remove("/archive");
        Require(tree.Children("/").empty(),
                "Tree index should remove deleted subtrees");

        std::filesystem::remove_all(root);
        std::cout << "All core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
