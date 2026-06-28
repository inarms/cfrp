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

#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include "utils.h"

namespace cfrp {
namespace common {

class BufferPool : public std::enable_shared_from_this<BufferPool> {
public:
    struct PoolConfig {
        size_t buffer_size;
        size_t initial_count;
        size_t max_count;
    };

    explicit BufferPool(const std::vector<PoolConfig>& configs) {
        for (const auto& config : configs) {
            auto& bucket = buckets_[config.buffer_size];
            bucket.config = config;
            bucket_sizes_.push_back(config.buffer_size);
            for (size_t i = 0; i < config.initial_count; ++i) {
                bucket.buffers.push_back(std::make_unique<uint8_t[]>(config.buffer_size));
                bucket.allocated_count++;
            }
        }
        std::sort(bucket_sizes_.begin(), bucket_sizes_.end());
        bucket_sizes_.erase(std::unique(bucket_sizes_.begin(), bucket_sizes_.end()), bucket_sizes_.end());
    }

    // Factory method to create pool based on memory profiles
    static std::shared_ptr<BufferPool> CreateByProfile(const std::string& profile) {
        std::string p = profile;
        std::transform(p.begin(), p.end(), p.begin(), ::tolower);

        if (p == "low") {
            return std::make_shared<BufferPool>(std::vector<PoolConfig>{
                {512, 8, 32}, {1024, 4, 16}, {4096, 2, 8}, {16384, 1, 4}, {65536, 1, 2}
            });
        } else if (p == "high") {
            return std::make_shared<BufferPool>(std::vector<PoolConfig>{
                {512, 32, 128}, {1024, 16, 64}, {4096, 16, 64}, {16384, 8, 32}, {65536, 4, 16}
            });
        } else {
            // "standard" or default
            return std::make_shared<BufferPool>(std::vector<PoolConfig>{
                {512, 16, 64}, {1024, 8, 32}, {4096, 8, 32}, {16384, 4, 16}, {65536, 2, 8}
            });
        }
    }

    // Default configuration for low-memory edge devices
    static std::shared_ptr<BufferPool> CreateDefault() {
        return CreateByProfile("low");
    }

    std::shared_ptr<uint8_t[]> Get(size_t size) {
        auto it = std::lower_bound(bucket_sizes_.begin(), bucket_sizes_.end(), size);
        if (it == bucket_sizes_.end()) {
            // No bucket large enough, allocate directly
            return std::shared_ptr<uint8_t[]>(new uint8_t[size], std::default_delete<uint8_t[]>());
        }

        size_t bucket_size = *it;
        auto it_bucket = buckets_.find(bucket_size);
        auto& bucket = it_bucket->second;

        std::unique_lock<std::mutex> lock(bucket.mutex);
        if (!bucket.buffers.empty()) {
            std::unique_ptr<uint8_t[]> raw_ptr = std::move(bucket.buffers.back());
            bucket.buffers.pop_back();
            lock.unlock();

            std::weak_ptr<BufferPool> weak_pool = shared_from_this();
            return std::shared_ptr<uint8_t[]>(raw_ptr.release(), [weak_pool, bucket_size](uint8_t* ptr) {
                if (auto pool = weak_pool.lock()) {
                    pool->Return(bucket_size, std::unique_ptr<uint8_t[]>(ptr));
                } else {
                    delete[] ptr;
                }
            });
        }

        // Pool empty, check if we can grow
        if (bucket.allocated_count < bucket.config.max_count) {
            bucket.allocated_count++;
            lock.unlock();
            
            try {
                std::weak_ptr<BufferPool> weak_pool = shared_from_this();
                return std::shared_ptr<uint8_t[]>(new uint8_t[bucket_size], [weak_pool, bucket_size](uint8_t* ptr) {
                    if (auto pool = weak_pool.lock()) {
                        pool->Return(bucket_size, std::unique_ptr<uint8_t[]>(ptr));
                    } else {
                        delete[] ptr;
                    }
                });
            } catch (...) {
                std::lock_guard<std::mutex> relock(bucket.mutex);
                bucket.allocated_count--;
                throw;
            }
        }

        // Max reached, allocate temporary
        lock.unlock();
        return std::shared_ptr<uint8_t[]>(new uint8_t[bucket_size], std::default_delete<uint8_t[]>());
    }

    void Return(size_t bucket_size, std::unique_ptr<uint8_t[]> buffer) {
        auto it_bucket = buckets_.find(bucket_size);
        if (it_bucket == buckets_.end()) {
            return;
        }
        auto& bucket = it_bucket->second;
        std::lock_guard<std::mutex> lock(bucket.mutex);
        if (bucket.buffers.size() < bucket.config.max_count) {
            bucket.buffers.push_back(std::move(buffer));
        } else {
            // This shouldn't happen often if allocated_count is tracked correctly,
            // but as a fallback, just let it delete.
            bucket.allocated_count--;
        }
    }

private:
    struct Bucket {
        PoolConfig config;
        std::vector<std::unique_ptr<uint8_t[]>> buffers;
        size_t allocated_count = 0;
        std::mutex mutex;
    };

    std::unordered_map<size_t, Bucket> buckets_;
    std::vector<size_t> bucket_sizes_;
};

} // namespace common
} // namespace cfrp
