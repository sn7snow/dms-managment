// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dms {

enum class Action { READ, WRITE, UPLOAD, DELETE, ADD_VERSION, DEL_VERSION, RENAME };
enum class SaveMode { OVERWRITE_LAST, CREATE_NEW };

struct PermissionResult {
    bool allowed = false;
    SaveMode save_mode = SaveMode::OVERWRITE_LAST;
};

class IPermissionProvider {
public:
    virtual ~IPermissionProvider() = default;
    virtual PermissionResult Check(const std::string& user,
                                  const std::vector<std::string>& groups,
                                  const std::string& path,
                                  Action action) = 0;
    virtual void Reload() = 0;
};

class CsvPermissionProvider final : public IPermissionProvider {
public:
    explicit CsvPermissionProvider(std::filesystem::path file);
    PermissionResult Check(const std::string& user,
                           const std::vector<std::string>& groups,
                           const std::string& path,
                           Action action) override;
    void Reload() override;

    struct Rule {
        std::string path;
        std::string object;
        bool directory = false;
        bool read = false;
        bool write = false;
        bool upload = false;
        bool remove = false;
        bool add_version = false;
        bool delete_version = false;
        bool rename = false;
        SaveMode save_mode = SaveMode::OVERWRITE_LAST;
    };

private:
    std::filesystem::path file_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::vector<Rule>>> rules_by_object_;
    std::filesystem::file_time_type modified_{};
};

std::vector<std::string> ParseCsvRow(const std::string& row);
std::string NormalizeLogicalPath(const std::string& path);

}  // namespace dms
