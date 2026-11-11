// SPDX-License-Identifier: GPL-3.0-or-later
#include "server_api_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace dms::server {

namespace {

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

constexpr std::uint64_t kMaxArchiveBytes = 2ULL * 1024 * 1024 * 1024;
constexpr std::size_t kMaxArchiveEntries = 100000;

}  // namespace

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

        // 归档必须能被打住：否则一次「下载文件夹」就能把磁盘写满，进而让服务无法建临时目录。
        std::uint64_t total_bytes = 0;
        std::size_t total_entries = 0;
        for (std::filesystem::recursive_directory_iterator it(root), end;
             it != end; ++it) {
            const auto entry = *it;
            if (entry.is_symlink()) {
                if (entry.is_directory()) it.disable_recursion_pending();
                continue;
            }
            const auto relative = std::filesystem::relative(entry.path(), root);
            const auto relative_text = relative.generic_string();
            std::string logical_entry;
            try {
                logical_entry = NormalizeLogicalPath(
                    logical_path == "/" ? "/" + relative_text
                                        : logical_path + "/" + relative_text);
            } catch (const std::exception&) {
                continue;  // 名字不合规的条目直接跳过，不进归档
            }
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
            if (++total_entries > kMaxArchiveEntries) {
                throw std::invalid_argument("ARCHIVE_TOO_MANY_ENTRIES");
            }
            if (is_file) {
                std::error_code size_error;
                const auto size = std::filesystem::file_size(entry.path(), size_error);
                if (size_error) continue;
                total_bytes += size;
                if (total_bytes > kMaxArchiveBytes) {
                    throw std::invalid_argument("ARCHIVE_TOO_LARGE");
                }
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

}  // namespace dms::server
