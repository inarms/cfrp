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

#include "common/async_bridge.h"
#include "common/protocol.h"
#include "common/utils.h"
#include <asio/detail/socket_ops.hpp>
#include <zstd.h>
#include <cstring>
#include <vector>

namespace cfrp {
namespace common {

Bridge::Bridge(std::shared_ptr<AsyncStream> s1,
               std::shared_ptr<AsyncStream> s2,
               bool use_compression,
               int compression_level,
               std::shared_ptr<RateLimiter> rate_limiter,
               std::shared_ptr<BufferPool> buffer_pool)
    : s1_(std::move(s1)), s2_(std::move(s2)),
      rate_limiter_(std::move(rate_limiter)),
      buffer_pool_(std::move(buffer_pool)),
      use_compression_(use_compression),
      compression_level_(compression_level) {
    if (!buffer_pool_) buffer_pool_ = BufferPool::CreateDefault();
    data1_ = buffer_pool_->Get(32768);
    data2_ = buffer_pool_->Get(32768);
}

void Bridge::Start() {
    DoRead(1);
    DoRead(2);
}

void Bridge::HandleStop() {
    if (!stopped_.exchange(true)) {
        s1_->close();
        s2_->close();
        if (on_stop_) on_stop_();
    }
}

void Bridge::DoRead(int direction) {
    auto self(shared_from_this());

    if (!use_compression_) {
        // ---- Passthrough (no compression) ----
        auto from    = (direction == 1) ? s1_ : s2_;
        auto buf     = (direction == 1) ? data1_ : data2_;

        from->async_read_some(asio::buffer(buf.get(), 32768),
            [this, self, direction, buf](std::error_code ec, std::size_t length) {
                if (!ec) {
                    auto to_inner  = (direction == 1) ? s2_ : s1_;

                    auto write_op = [this, self, direction, to_inner, buf, length]() {
                        if (direction == 1 && bytes_received_) *bytes_received_ += length;
                        else if (direction == 2 && bytes_sent_) *bytes_sent_ += length;

                        to_inner->async_write(asio::buffer(buf.get(), length),
                            [this, self, direction](std::error_code ec2, std::size_t) {
                                if (!ec2) {
                                    DoRead(direction);
                                } else {
                                    if (ec2 != asio::error::operation_aborted) {
                                        Logger::Error("[Bridge] Write error (" + std::to_string(direction) +
                                                      "): " + ec2.message());
                                    }
                                    HandleStop();
                                }
                            });
                    };

                    if (rate_limiter_) {
                        rate_limiter_->async_wait(length, std::move(write_op));
                    } else {
                        write_op();
                    }
                } else {
                    if (ec != asio::error::operation_aborted && ec != asio::error::eof) {
                        Logger::Error("[Bridge] Connection error (" + std::to_string(direction) +
                                      "): " + ec.message());
                    } else if (ec == asio::error::eof) {
                        Logger::Info("[Bridge] Connection closed by peer (" + std::to_string(direction) + ")");
                    }
                    HandleStop();
                }
            });

    } else if (direction == 1) {
        // ---- Compress & Frame:  s1 (raw) → s2 (work) ----
        s1_->async_read_some(asio::buffer(data1_.get(), 32768),
            [this, self](std::error_code ec, std::size_t length) {
                if (!ec) {
                    size_t const cSizeBound = ZSTD_compressBound(length);
                    auto& compressed_buf = compressor_.get_compress_buf(cSizeBound);
                    size_t const cSize = compressor_.compress(
                        compressed_buf.data(), cSizeBound, data1_.get(), length, compression_level_);

                    uint32_t final_header;
                    const void* write_buf;
                    size_t write_len;

                    if (!ZSTD_isError(cSize) && cSize < length) {
                        final_header = asio::detail::socket_ops::host_to_network_long(
                            static_cast<uint32_t>(cSize) | protocol::COMPRESSION_FLAG);
                        write_buf = compressed_buf.data();
                        write_len = cSize;
                    } else {
                        final_header = asio::detail::socket_ops::host_to_network_long(
                            static_cast<uint32_t>(length));
                        write_buf = data1_.get();
                        write_len = length;
                    }

                    size_t packet_len = sizeof(final_header) + write_len;
                    auto packet = buffer_pool_->Get(packet_len);
                    std::memcpy(packet.get(), &final_header, sizeof(final_header));
                    std::memcpy(packet.get() + sizeof(final_header), write_buf, write_len);

                    auto write_op = [this, self, packet, packet_len, length]() {
                        if (bytes_received_) *bytes_received_ += length;
                        s2_->async_write(asio::buffer(packet.get(), packet_len),
                            [this, self, packet](std::error_code ec2, std::size_t) {
                                if (!ec2) {
                                    DoRead(1);
                                } else {
                                    HandleStop();
                                }
                            });
                    };

                    if (rate_limiter_) {
                        rate_limiter_->async_wait(length, std::move(write_op));
                    } else {
                        write_op();
                    }
                } else {
                    HandleStop();
                }
            });

    } else {
        // ---- Unframe & Decompress:  s2 (work) → s1 (raw) ----
        s2_->async_read(asio::buffer(&header2_, sizeof(header2_)),
            [this, self](std::error_code ec, std::size_t) {
                if (!ec) {
                    uint32_t h2 = asio::detail::socket_ops::network_to_host_long(header2_);
                    uint32_t len = h2 & protocol::LENGTH_MASK;
                    bool compressed = (h2 & protocol::COMPRESSION_FLAG) != 0;

                    if (len > protocol::MAX_MESSAGE_SIZE) {
                        HandleStop(); return;
                    }

                    auto body = buffer_pool_->Get(len);
                    s2_->async_read(asio::buffer(body.get(), len),
                        [this, self, body, len, compressed](std::error_code ec2, std::size_t) {
                            if (!ec2) {
                                if (compressed) {
                                    unsigned long long const decodedSize =
                                        ZSTD_getFrameContentSize(body.get(), len);
                                    if (decodedSize == ZSTD_CONTENTSIZE_ERROR ||
                                        decodedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
                                        decodedSize > protocol::MAX_DECOMPRESSED_SIZE) {
                                        HandleStop(); return;
                                    }
                                    
                                    auto& decompressed_buf = decompressor_.get_decompress_buf(decodedSize);
                                    size_t const dSize = decompressor_.decompress(
                                        decompressed_buf.data(), decodedSize,
                                        body.get(), len);
                                    if (ZSTD_isError(dSize)) {
                                        HandleStop(); return;
                                    }

                                    auto final_decompressed = buffer_pool_->Get(dSize);
                                    std::memcpy(final_decompressed.get(), decompressed_buf.data(), dSize);

                                    auto write_op = [this, self, final_decompressed, dSize]() {
                                        if (bytes_sent_) *bytes_sent_ += dSize;
                                        s1_->async_write(asio::buffer(final_decompressed.get(), dSize),
                                            [this, self, final_decompressed](std::error_code ec3, std::size_t) {
                                                if (!ec3) DoRead(2);
                                                else { HandleStop(); }
                                            });
                                    };

                                    if (rate_limiter_) {
                                        rate_limiter_->async_wait(dSize,
                                                                   std::move(write_op));
                                    } else {
                                        write_op();
                                    }
                                } else {
                                    auto write_op = [this, self, body, len]() {
                                        if (bytes_sent_) *bytes_sent_ += len;
                                        s1_->async_write(asio::buffer(body.get(), len),
                                            [this, self, body](std::error_code ec3, std::size_t) {
                                                if (!ec3) DoRead(2);
                                                else { HandleStop(); }
                                            });
                                    };

                                    if (rate_limiter_) {
                                        rate_limiter_->async_wait(len,
                                                                   std::move(write_op));
                                    } else {
                                        write_op();
                                    }
                                }
                            } else {
                                HandleStop();
                            }
                        });
                } else {
                    HandleStop();
                }
            });
    }
}

} // namespace common
} // namespace cfrp
