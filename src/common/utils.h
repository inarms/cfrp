/*
 * Copyright 2026 inarms
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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <vector>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/coding.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

namespace cfrp {
namespace common {

inline std::string GetExecutablePath() {
    std::error_code ec;
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return std::string(buffer);
#elif defined(__APPLE__)
    char buffer[1024];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        return std::filesystem::canonical(buffer, ec).string();
    }
    return "";
#else
    return std::filesystem::read_symlink("/proc/self/exe", ec).string();
#endif
}

inline bool IsProcessRunning(int pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == NULL) return false;
    DWORD exitCode;
    GetExitCodeProcess(process, &exitCode);
    CloseHandle(process);
    return exitCode == STILL_ACTIVE;
#else
    return kill(pid, 0) == 0;
#endif
}

inline bool StopProcess(int pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (process == NULL) return false;
    bool result = TerminateProcess(process, 1);
    CloseHandle(process);
    return result;
#else
    return kill(pid, SIGTERM) == 0;
#endif
}

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

inline std::string Base64Encode(const uint8_t* data, size_t len) {
    word32 outLen = 0;
    Base64_Encode(reinterpret_cast<const byte*>(data), static_cast<word32>(len), NULL, &outLen);

    std::vector<byte> out(outLen);
    Base64_Encode(reinterpret_cast<const byte*>(data), static_cast<word32>(len), out.data(), &outLen);

    while (outLen > 0 && (out[outLen - 1] == '\0' || out[outLen - 1] == '\n' || out[outLen - 1] == '\r')) {
        --outLen;
    }
    return std::string(reinterpret_cast<const char*>(out.data()), outLen);
}

inline std::string Base64Encode(const std::string& s) {
    return Base64Encode(reinterpret_cast<const uint8_t*>(s.c_str()), s.length());
}

// ---------------------------------------------------------------------------
// Logger  — simple levelled output gate
// ---------------------------------------------------------------------------
enum class LogLevel : int {
    None  = 0,   // Suppress all output
    Error = 1,   // Errors only
    Info  = 2,   // Errors + informational messages  (default)
    Debug = 3    // Everything
};

class Logger {
public:
    static void SetLevel(LogLevel level) noexcept { level_ = level; }
    static LogLevel GetLevel() noexcept { return level_; }

    static void SetLevel(const std::string& level_s) {
        std::string s = level_s;
        for (auto& c : s) c = std::tolower(c);
        if (s == "none" || s == "0") level_ = LogLevel::None;
        else if (s == "error" || s == "1") level_ = LogLevel::Error;
        else if (s == "info" || s == "2") level_ = LogLevel::Info;
        else if (s == "debug" || s == "3") level_ = LogLevel::Debug;
    }

    static void Error(const std::string& msg) {
        if (level_ >= LogLevel::Error) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::cerr << msg << '\n';
        }
    }
    static void Info(const std::string& msg) {
        if (level_ >= LogLevel::Info) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout << msg << std::endl;
        }
    }
    static void Debug(const std::string& msg) {
        if (level_ >= LogLevel::Debug) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout << "[DEBUG] " << msg << std::endl;
        }
    }

private:
    inline static LogLevel level_ = LogLevel::Info;
    inline static std::mutex mutex_;
};

} // namespace common
} // namespace cfrp
