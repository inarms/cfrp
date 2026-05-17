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

#include "common/stream.h"
#include "common/rate_limiter.h"
#include "common/zstd_utils.h"
#include "common/buffer_pool.h"
#include <asio.hpp>
#include <memory>
#include <atomic>

namespace cfrp {
namespace common {

// Bidirectional async pipe between two AsyncStream objects.
class Bridge : public std::enable_shared_from_this<Bridge> {
public:
    Bridge(std::shared_ptr<AsyncStream> s1,
           std::shared_ptr<AsyncStream> s2,
           bool use_compression,
           int compression_level = 1,
           std::shared_ptr<RateLimiter> rate_limiter = nullptr,
           std::shared_ptr<BufferPool> buffer_pool = nullptr);

    void Start();

    // Set optional counters for bandwidth tracking
    void SetStatsCounters(std::atomic<uint64_t>* bytes_sent, std::atomic<uint64_t>* bytes_received) {
        bytes_sent_ = bytes_sent;
        bytes_received_ = bytes_received;
    }

    void SetOnStop(std::function<void()> on_stop) {
        on_stop_ = std::move(on_stop);
    }

private:
    void DoRead(int direction);
    void HandleStop();

    std::shared_ptr<AsyncStream> s1_;
    std::shared_ptr<AsyncStream> s2_;
    std::shared_ptr<RateLimiter> rate_limiter_;
    std::shared_ptr<BufferPool> buffer_pool_;
    bool use_compression_;
    int compression_level_;
    std::shared_ptr<uint8_t[]> data1_;
    std::shared_ptr<uint8_t[]> data2_;
    uint32_t header2_{};

    std::atomic<uint64_t>* bytes_sent_ = nullptr;     // Sent to user (s2 -> s1)
    std::atomic<uint64_t>* bytes_received_ = nullptr; // Received from user (s1 -> s2)
    std::function<void()> on_stop_;
    std::atomic_bool stopped_{false};

    ZstdContext cctx_;
    ZstdContext dctx_;
};

} // namespace common
} // namespace cfrp
