// SPDX-License-Identifier: GPL-3.0-or-later
#include "dms/storage.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

namespace {

class PendingFileLock {
public:
    explicit PendingFileLock(const std::filesystem::path& path) {
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0640);
        if (descriptor_ < 0) {
            throw std::runtime_error("Cannot open pending delete file for locking");
        }
        if (::flock(descriptor_, LOCK_EX) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("Cannot lock pending delete file");
        }
    }
    ~PendingFileLock() {
        Release();
    }

    void Release() {
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }
    PendingFileLock(const PendingFileLock&) = delete;
    PendingFileLock& operator=(const PendingFileLock&) = delete;

private:
    int descriptor_ = -1;
};

bool ParseStatus(const std::string& text, int& status) {
    if (text == "0" || text == "1" || text == "2") {
        status = text[0] - '0';
        return true;
    }
    return false;
}

std::filesystem::path VersionDirectory(const dms::Config& config,
                                      const std::string& logical_path) {
    const auto normalized = dms::NormalizeLogicalPath(logical_path);
    return dms::ResolveSafePath(config.VersionStore(), normalized);
}

template <typename Stream>
void SendRefreshRequest(Stream& stream, const std::string& token) {
    http::request<http::string_body> request{http::verb::post,
                                             "/api/internal/refresh", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set("X-DMS-Refresh-Token", token);
    request.body() = "{}";
    request.prepare_payload();
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    if (response.result() != http::status::ok) {
        throw std::runtime_error("dms-server refresh notification failed: HTTP " +
                                 std::to_string(response.result_int()));
    }
}

void NotifyServer(const dms::Config& config) {
    asio::io_context io;
    tcp::resolver resolver(io);
    const auto endpoints = resolver.resolve("127.0.0.1",
                                             std::to_string(config.port));
    if (config.tls_cert_file.empty()) {
        beast::tcp_stream stream(io);
        stream.connect(endpoints);
        SendRefreshRequest(stream, config.refresh_token);
        beast::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        return;
    }

    asio::ssl::context context(asio::ssl::context::tls_client);
    context.set_default_verify_paths();
    context.load_verify_file(config.tls_ca_file.empty()
                                 ? config.tls_cert_file
                                 : config.tls_ca_file);
    asio::ssl::stream<beast::tcp_stream> stream(io, context);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), "127.0.0.1")) {
        throw std::runtime_error("Unable to set TLS SNI for dms-server");
    }
    stream.set_verify_mode(asio::ssl::verify_peer);
    stream.set_verify_callback([](bool preverified,
                                  asio::ssl::verify_context& context) {
        if (!preverified) return false;
        auto* store = context.native_handle();
        if (X509_STORE_CTX_get_error_depth(store) != 0) return true;
        return X509_check_ip_asc(
                   X509_STORE_CTX_get_current_cert(store), "127.0.0.1", 0) == 1;
    });
    asio::connect(stream.next_layer().socket(), endpoints);
    stream.handshake(asio::ssl::stream_base::client);
    SendRefreshRequest(stream, config.refresh_token);
    beast::error_code ignored;
    stream.next_layer().socket().shutdown(tcp::socket::shutdown_both, ignored);
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 3 || std::string_view(argv[1]) != "--config") {
            std::cerr << "Usage: dms-cron --config PATH\n";
            return 2;
        }
        const auto config = dms::Config::Load(argv[2]);
        std::filesystem::create_directories(config.Logs());
        const auto pending_file = config.PendingFile();
        std::filesystem::create_directories(pending_file.parent_path());
        if (!std::filesystem::exists(pending_file)) {
            std::ofstream create_pending(pending_file);
            if (!create_pending) {
                throw std::runtime_error("Cannot create pending delete file");
            }
        }
        PendingFileLock lock(pending_file.string() + ".lock");

        std::ifstream input(pending_file);
        if (!input) {
            throw std::runtime_error("Cannot read pending delete file");
        }
        std::vector<std::string> remaining;
        std::string line;
        bool changed = false;
        dms::AuditLog audit(config.Logs());
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }
            const auto fields = dms::ParseCsvRow(line);
            if (fields.size() != 4) {
                std::cerr << "Ignoring malformed pending_delete.csv row\n";
                remaining.push_back(line);
                continue;
            }
            int status = -1;
            if (!ParseStatus(fields[2], status)) {
                std::cerr << "Ignoring pending_delete.csv row with illegal status: "
                          << fields[2] << '\n';
                remaining.push_back(line);
                continue;
            }
            if (status == 0) {
                remaining.push_back(line);
                continue;
            }
            const auto logical_path = dms::NormalizeLogicalPath(fields[1]);
            if (status == 1) {
                if (logical_path == "/") {
                    throw std::runtime_error(
                        "Refusing to delete the file-store root");
                }
                const auto workspace_path =
                    dms::ResolveSafePath(config.FileStore(), logical_path);
                if (std::filesystem::exists(workspace_path)) {
                    if (!std::filesystem::is_directory(workspace_path)) {
                        throw std::runtime_error(
                            "Approved pending-delete path is not a directory: " +
                            logical_path);
                    }
                    std::filesystem::remove_all(workspace_path);
                }
                const auto versions = VersionDirectory(config, logical_path);
                if (std::filesystem::exists(versions)) {
                    std::filesystem::remove_all(versions);
                }
                audit.Write(fields[0], "127.0.0.1", "DELETE", logical_path,
                            "审批删除文件夹");
                std::cout << "Approved deletion: " << logical_path << '\n';
            } else {
                std::cout << "Rejected deletion: " << logical_path << '\n';
            }
            changed = true;
        }
        if (changed) {
            const auto temporary = pending_file.string() + ".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output) {
                    throw std::runtime_error("Cannot write temporary pending delete file");
                }
                for (const auto& entry : remaining) {
                    output << entry << '\n';
                }
                output.flush();
                if (!output) {
                    throw std::runtime_error("Unable to flush pending delete file");
                }
            }
            if (::chmod(temporary.c_str(), 0640) != 0) {
                std::filesystem::remove(temporary);
                throw std::runtime_error("Cannot secure temporary pending delete file");
            }
            if (::rename(temporary.c_str(), pending_file.c_str()) != 0) {
                std::filesystem::remove(temporary);
                throw std::runtime_error("Cannot atomically replace pending delete file");
            }
        }
        lock.Release();
        NotifyServer(config);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dms-cron: " << error.what() << '\n';
        return 1;
    }
}
