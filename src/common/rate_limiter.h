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

#include <asio.hpp>
#include <chrono>
#include <deque>
#include <mutex>
#include <functional>
#include <memory>
#include <algorithm>

namespace cfrp {
namespace common {

class RateLimiter : public std::enable_shared_from_this<RateLimiter> {
public:
    explicit RateLimiter(asio::io_context& io_context, int64_t bytes_per_sec)
        : io_context_(io_context), bytes_per_sec_(bytes_per_sec), timer_(io_context) {
        last_update_ = std::chrono::steady_clock::now();
        tokens_ = static_cast<double>(bytes_per_sec);
    }

    void set_rate(int64_t bytes_per_sec) {
        std::lock_guard<std::mutex> lock(mutex_);
        bytes_per_sec_ = bytes_per_sec;
        tokens_ = std::min(tokens_, static_cast<double>(bytes_per_sec_));
    }

    template <typename Handler>
    void async_wait(int64_t bytes, Handler&& handler) {
        if (bytes_per_sec_ <= 0) {
            asio::post(io_context_, std::forward<Handler>(handler));
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        update_tokens();

        if (waiting_handlers_.empty() && tokens_ >= static_cast<double>(bytes)) {
            tokens_ -= static_cast<double>(bytes);
            asio::post(io_context_, std::forward<Handler>(handler));
            return;
        }

        waiting_handlers_.push_back({bytes, std::function<void()>(std::forward<Handler>(handler))});
        if (waiting_handlers_.size() == 1) {
            schedule_timer();
        }
    }

private:
    void update_tokens() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last_update_).count();
        last_update_ = now;
        tokens_ += (static_cast<double>(elapsed) * bytes_per_sec_) / 1000000.0;
        if (tokens_ > static_cast<double>(bytes_per_sec_)) {
            tokens_ = static_cast<double>(bytes_per_sec_);
        }
    }

    void schedule_timer() {
        if (waiting_handlers_.empty()) return;

        update_tokens();
        auto& first = waiting_handlers_.front();
        if (tokens_ >= static_cast<double>(first.bytes)) {
            tokens_ -= static_cast<double>(first.bytes);
            auto handler = std::move(first.handler);
            waiting_handlers_.pop_front();
            
            // Post the handler and schedule next if any
            asio::post(io_context_, [handler = std::move(handler), self = shared_from_this()]() {
                handler();
            });
            
            if (!waiting_handlers_.empty()) {
                schedule_timer();
            }
            return;
        }

        auto needed = static_cast<double>(first.bytes) - tokens_;
        auto delay_us = static_cast<int64_t>((needed * 1000000.0) / static_cast<double>(bytes_per_sec_));
        
        timer_.expires_after(std::chrono::microseconds(delay_us));
        auto self = shared_from_this();
        timer_.async_wait([this, self](const std::error_code& ec) {
            if (!ec) {
                std::lock_guard<std::mutex> lock(mutex_);
                schedule_timer();
            }
        });
    }

    struct WaitingHandler {
        int64_t bytes;
        std::function<void()> handler;
    };

    asio::io_context& io_context_;
    int64_t bytes_per_sec_;
    double tokens_;
    std::chrono::steady_clock::time_point last_update_;
    asio::steady_timer timer_;
    std::deque<WaitingHandler> waiting_handlers_;
    std::mutex mutex_;
};

} // namespace common
} // namespace cfrp
