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

std::thread StartPermissionMonitor(Runtime& runtime, std::atomic_bool& stop) {
    return std::thread([&runtime, &stop] {
        std::error_code permissions_error;
        std::error_code pending_error;
        auto permissions_time =
            std::filesystem::last_write_time(runtime.config.PermissionsFile(),
                                             permissions_error);
        auto pending_time =
            std::filesystem::last_write_time(runtime.config.PendingFile(), pending_error);
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (stop.load()) break;
            const auto current_permissions =
                std::filesystem::last_write_time(runtime.config.PermissionsFile(),
                                                 permissions_error);
            const auto current_pending =
                std::filesystem::last_write_time(runtime.config.PendingFile(),
                                                 pending_error);
            if (!permissions_error && current_permissions != permissions_time) {
                try {
                    runtime.ReloadPermissions();
                    permissions_time = current_permissions;
                } catch (const std::exception& error) {
                    std::cerr << "Permission reload failed: " << error.what() << '\n';
                }
            }
            if (!pending_error && current_pending != pending_time) {
                try {
                    runtime.ReloadPending();
                    pending_time = current_pending;
                } catch (const std::exception& error) {
                    std::cerr << "Pending-delete reload failed: " << error.what() << '\n';
                }
            }
            {
                std::lock_guard lock(runtime.document_locks_mutex);
                const auto now = std::chrono::steady_clock::now();
                for (auto it = runtime.document_locks.begin();
                     it != runtime.document_locks.end();) {
                    if (now - it->second.heartbeat >= std::chrono::minutes(30)) {
                        runtime.notifications.PublishRelease(it->first);
                        it = runtime.document_locks.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
    });
}

}  // namespace dms::server
