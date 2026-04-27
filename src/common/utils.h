/*
 * Copyright 2026 neesonqk
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string>
#include <cstdint>
#include <cctype>
#include <cstdlib>

namespace cfrp {
namespace common {

inline std::string GetHomeDirectory() {
    const char* home = nullptr;
#ifdef _WIN32
    home = std::getenv("USERPROFILE");
#else
    home = std::getenv("HOME");
#endif
    return home ? std::string(home) : "";
}

inline void SetTcpKeepalive(asio::ip::tcp::socket& socket) {
    std::error_code ec;
    socket.set_option(asio::ip::tcp::no_delay(true), ec);
    socket.set_option(asio::socket_base::keep_alive(true), ec);
#ifdef _WIN32
    // Windows specific keepalive settings could go here
#else
    // For Linux/macOS, we can set more fine-grained options if needed
    // but the basic keep_alive(true) is a good start.
#endif
}

inline int64_t ParseBandwidth(const std::string& s) {
    if (s.empty()) return 0;
    std::string val_s = s;
    char unit = std::toupper(val_s.back());
    int64_t multiplier = 1;
    if (unit == 'K') {
        multiplier = 1024;
        val_s.pop_back();
    } else if (unit == 'M') {
        multiplier = 1024 * 1024;
        val_s.pop_back();
    } else if (unit == 'G') {
        multiplier = 1024 * 1024 * 1024;
        val_s.pop_back();
    }
    try {
        return std::stoll(val_s) * multiplier;
    } catch (...) {
        return 0;
    }
}

} // namespace common
} // namespace cfrp
