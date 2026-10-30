// SPDX-License-Identifier: GPL-3.0-or-later
#include "dms/storage.hpp"
#include "server_internal.hpp"

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
        dms::server::Runtime runtime(std::move(config));

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
        dms::server::StartListener(
            io, endpoint, runtime, workers,
            tls_context ? &*tls_context : nullptr);
        dms::server::PidFile pid_file(runtime.config.root / "run" / "dms.pid");

        std::atomic_bool stop{false};
        auto monitor = dms::server::StartPermissionMonitor(runtime, stop);
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
