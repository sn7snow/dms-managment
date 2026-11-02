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

template <typename Stream>
std::string RemoteAddress(const Stream& stream) {
    boost::system::error_code error;
    const auto endpoint = stream.lowest_layer().remote_endpoint(error);
    return error ? std::string{} : endpoint.address().to_string();
}

template <typename Stream, bool Tls>
class Session : public std::enable_shared_from_this<Session<Stream, Tls>> {
public:
    Session(tcp::socket socket, asio::ssl::context* tls_context,
            Runtime& runtime, asio::thread_pool& workers)
        : stream_(MakeStream(std::move(socket), tls_context)),
          runtime_(runtime), workers_(workers),
          temporary_directory_(runtime.config.root / ".http-tmp" /
                               MakeRandomToken()) {
        if (!std::filesystem::create_directory(temporary_directory_)) {
            throw std::runtime_error(
                "Cannot create isolated HTTP temporary directory");
        }
        std::filesystem::permissions(
            temporary_directory_, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
    }

    ~Session() {
        std::error_code ignored;
        if (response_ && !response_->cleanup_file.empty()) {
            std::filesystem::remove(response_->cleanup_file, ignored);
            if (ignored) {
                std::cerr << "Cannot remove temporary response file: "
                          << ignored.message() << '\n';
            }
        }
        std::filesystem::remove_all(temporary_directory_, ignored);
        if (ignored) {
            std::cerr << "Cannot remove HTTP temporary directory: "
                      << ignored.message() << '\n';
        }
    }

    void Start() {
        if constexpr (Tls) {
            stream_.async_handshake(
                asio::ssl::stream_base::server,
                beast::bind_front_handler(&Session::OnHandshake, this->shared_from_this()));
        } else {
            Read();
        }
    }

private:
    static Stream MakeStream(tcp::socket socket, asio::ssl::context* tls_context) {
        if constexpr (Tls) {
            if (tls_context == nullptr) {
                throw std::invalid_argument("TLS session requires an SSL context");
            }
            return Stream(std::move(socket), *tls_context);
        } else {
            (void)tls_context;
            return Stream(std::move(socket));
        }
    }

    void OnHandshake(beast::error_code error) {
        if (error) {
            std::cerr << "TLS handshake failed: " << error.message() << '\n';
            return Close();
        }
        Read();
    }

    void Read() {
        parser_.emplace();
        parser_->body_limit(4ULL * 1024 * 1024 * 1024);
        request_body_file_ = temporary_directory_ / "request.body";
        beast::error_code open_error;
        parser_->get().body().open(
            request_body_file_.c_str(), beast::file_mode::write, open_error);
        if (open_error) {
            std::cerr << "Cannot create request body file: "
                      << open_error.message() << '\n';
            return Close();
        }
        http::async_read_header(
            stream_, buffer_, *parser_,
            beast::bind_front_handler(&Session::OnHeader,
                                      this->shared_from_this()));
    }

    void OnHeader(beast::error_code error, std::size_t) {
        if (error == http::error::end_of_stream) return Close();
        if (error == http::error::body_limit) return WritePayloadTooLarge();
        if (error) {
            std::cerr << "HTTP header read failed: " << error.message() << '\n';
            return Close();
        }
        const auto& request = parser_->get();
        const auto target = request.target();
        const auto query_position = target.find('?');
        const auto route = target.substr(0, query_position);
        const bool streaming_body =
            (request.method() == http::verb::post && route == "/api/upload") ||
            (request.method() == http::verb::put && route == "/api/file");
        parser_->body_limit(streaming_body
                                ? 4ULL * 1024 * 1024 * 1024
                                : 1024ULL * 1024);
        if (streaming_body) {
            http::request<http::string_body> header_request{
                request.method(), request.target(), request.version()};
            header_request.base() = request.base();
            if (auto response =
                    PreflightFileTransfer(runtime_, header_request)) {
                parser_->get().body().close();
                parser_.reset();
                std::error_code cleanup_error;
                std::filesystem::remove(request_body_file_, cleanup_error);
                if (cleanup_error) {
                    std::cerr << "Cannot remove rejected request body: "
                              << cleanup_error.message() << '\n';
                }
                WriteResponse(std::move(*response));
                return;
            }
        }
        http::async_read(
            stream_, buffer_, *parser_,
            beast::bind_front_handler(&Session::OnRead,
                                      this->shared_from_this()));
    }

    void OnRead(beast::error_code error, std::size_t) {
        if (error == http::error::end_of_stream) {
            return Close();
        }
        if (error) {
            if (error == http::error::body_limit) {
                return WritePayloadTooLarge();
            }
            std::cerr << "HTTP read failed: " << error.message() << '\n';
            return Close();
        }
        auto file_request = parser_->release();
        parser_.reset();
        file_request.body().close();
        const auto body_size = std::filesystem::file_size(request_body_file_);

        const auto target = file_request.target();
        const auto query_position = target.find('?');
        const auto route = target.substr(0, query_position);
        const bool streaming_body =
            (file_request.method() == http::verb::post &&
             route == "/api/upload") ||
            (file_request.method() == http::verb::put &&
             route == "/api/file");
        http::request<http::string_body> request{
            file_request.method(), file_request.target(), file_request.version()};
        request.base() = file_request.base();
        if (!streaming_body && body_size != 0) {
            std::ifstream input(request_body_file_, std::ios::binary);
            if (!input) {
                std::cerr << "Cannot read buffered request body\n";
                return Close();
            }
            request.body().assign(std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>());
        }
        const auto ip = RemoteAddress(stream_);
        auto self = this->shared_from_this();
        auto request_ptr =
            std::make_shared<http::request<http::string_body>>(
                std::move(request));
        const auto body_file = streaming_body
                                   ? request_body_file_
                                   : std::filesystem::path{};
        asio::post(workers_, [self, ip, request_ptr, body_file] {
            auto response =
                SafeHandle(self->runtime_, *request_ptr, ip, body_file);
            std::error_code cleanup_error;
            std::filesystem::remove(self->request_body_file_, cleanup_error);
            if (cleanup_error) {
                std::cerr << "Cannot remove temporary request body: "
                          << cleanup_error.message() << '\n';
            }
            asio::post(self->stream_.get_executor(),
                       [self, response = std::move(response)]() mutable {
                           self->WriteResponse(std::move(response));
                       });
        });
    }

    void WriteResponse(Response response) {
        response_.emplace(std::move(response));
        response_keep_alive_ = response_->keep_alive();
        beast::async_write(
            stream_, std::move(response_->message),
            beast::bind_front_handler(&Session::OnWrite,
                                      this->shared_from_this()));
    }

    void OnWrite(beast::error_code error, std::size_t) {
        if (error) {
            std::cerr << "HTTP write failed: " << error.message() << '\n';
            return Close();
        }
        const bool keep_alive = response_keep_alive_;
        if (!response_->cleanup_file.empty()) {
            std::error_code cleanup_error;
            std::filesystem::remove(response_->cleanup_file, cleanup_error);
            if (cleanup_error) {
                std::cerr << "Cannot remove temporary response file: "
                          << cleanup_error.message() << '\n';
            }
        }
        response_.reset();
        if (!keep_alive) {
            return Close();
        }
        Read();
    }

    void WritePayloadTooLarge() {
        http::response<http::string_body> response{
            http::status::payload_too_large, parser_->get().version()};
        response.set(http::field::server, "dms-server");
        response.set(http::field::content_type, "application/json; charset=utf-8");
        response.keep_alive(false);
        response.body() = "{\"error\":\"FILE_TOO_LARGE\"}";
        response.prepare_payload();
        parser_.reset();
        response_.emplace(Response{
            http::message_generator(std::move(response)), {}});
        response_keep_alive_ = false;
        beast::async_write(
            stream_, std::move(response_->message),
            beast::bind_front_handler(&Session::OnWrite, this->shared_from_this()));
    }

    void Close() {
        beast::error_code ignored;
        stream_.lowest_layer().shutdown(tcp::socket::shutdown_both, ignored);
        stream_.lowest_layer().close(ignored);
        if (response_ && !response_->cleanup_file.empty()) {
            std::filesystem::remove(response_->cleanup_file, ignored);
            if (ignored) {
                std::cerr << "Cannot remove temporary response file: "
                          << ignored.message() << '\n';
            }
        }
        std::filesystem::remove_all(temporary_directory_, ignored);
        if (ignored) {
            std::cerr << "Cannot remove HTTP temporary directory: "
                      << ignored.message() << '\n';
        }
    }

    Stream stream_;
    beast::flat_buffer buffer_;
    std::optional<http::request_parser<http::file_body>> parser_;
    std::optional<Response> response_;
    bool response_keep_alive_ = false;
    Runtime& runtime_;
    asio::thread_pool& workers_;
    std::filesystem::path temporary_directory_;
    std::filesystem::path request_body_file_;
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(asio::io_context& io, tcp::endpoint endpoint, Runtime& runtime,
             asio::thread_pool& workers, asio::ssl::context* tls_context)
        : io_(io), acceptor_(asio::make_strand(io)), runtime_(runtime),
          workers_(workers), tls_context_(tls_context) {
        beast::error_code error;
        acceptor_.open(endpoint.protocol(), error);
        if (error) throw beast::system_error(error);
        acceptor_.set_option(asio::socket_base::reuse_address(true), error);
        if (error) throw beast::system_error(error);
        acceptor_.bind(endpoint, error);
        if (error) throw beast::system_error(error);
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        if (error) throw beast::system_error(error);
    }

    void Run() { Accept(); }

private:
    void Accept() {
        acceptor_.async_accept(
            asio::make_strand(io_),
            beast::bind_front_handler(&Listener::OnAccept, shared_from_this()));
    }

    void OnAccept(beast::error_code error, tcp::socket socket) {
        if (!error) {
            if (tls_context_ != nullptr) {
                using TlsStream = asio::ssl::stream<tcp::socket>;
                std::make_shared<Session<TlsStream, true>>(
                    std::move(socket), tls_context_, runtime_, workers_)->Start();
            } else {
                std::make_shared<Session<tcp::socket, false>>(
                    std::move(socket), nullptr, runtime_, workers_)->Start();
            }
        } else {
            std::cerr << "Accept failed: " << error.message() << '\n';
        }
        Accept();
    }

    asio::io_context& io_;
    tcp::acceptor acceptor_;
    Runtime& runtime_;
    asio::thread_pool& workers_;
    asio::ssl::context* tls_context_;
};

}  // namespace

void StartListener(asio::io_context& io, tcp::endpoint endpoint,
                   Runtime& runtime, asio::thread_pool& workers,
                   asio::ssl::context* tls_context) {
    auto listener = std::make_shared<Listener>(
        io, endpoint, runtime, workers, tls_context);
    listener->Run();
}

}  // namespace dms::server
