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
               std::shared_ptr<RateLimiter> rate_limiter)
    : s1_(std::move(s1)), s2_(std::move(s2)),
      rate_limiter_(std::move(rate_limiter)),
      use_compression_(use_compression),
      compression_level_(compression_level) {}

void Bridge::Start() {
    DoRead(1);
    DoRead(2);
}

void Bridge::DoRead(int direction) {
    auto self(shared_from_this());

    if (!use_compression_) {
        // ---- Passthrough (no compression) ----
        auto from    = (direction == 1) ? s1_ : s2_;
        auto buf     = (direction == 1) ? data1_ : data2_;

        from->async_read_some(asio::buffer(buf, sizeof(data1_)),
            [this, self, direction, buf](std::error_code ec, std::size_t length) {
                if (!ec) {
                    auto to_inner  = (direction == 1) ? s2_ : s1_;

                    auto write_op = [this, self, direction, to_inner, buf, length]() {
                        to_inner->async_write(asio::buffer(buf, length),
                            [this, self, direction](std::error_code ec2, std::size_t) {
                                if (!ec2) {
                                    DoRead(direction);
                                } else {
                                    if (ec2 != asio::error::operation_aborted) {
                                        Logger::Error("[Bridge] Write error (" + std::to_string(direction) +
                                                      "): " + ec2.message());
                                    }
                                    s1_->close(); s2_->close();
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
                    s1_->close(); s2_->close();
                }
            });

    } else if (direction == 1) {
        // ---- Compress & Frame:  s1 (raw) → s2 (work) ----
        s1_->async_read_some(asio::buffer(data1_, sizeof(data1_)),
            [this, self](std::error_code ec, std::size_t length) {
                if (!ec) {
                    size_t const cSizeBound = ZSTD_compressBound(length);
                    std::vector<uint8_t> compressed(cSizeBound);
                    size_t const cSize = ZSTD_compress(
                        compressed.data(), cSizeBound, data1_, length, compression_level_);

                    uint32_t final_header;
                    const void* write_buf;
                    size_t write_len;

                    if (!ZSTD_isError(cSize) && cSize < length) {
                        final_header = asio::detail::socket_ops::host_to_network_long(
                            static_cast<uint32_t>(cSize) | protocol::COMPRESSION_FLAG);
                        write_buf = compressed.data();
                        write_len = cSize;
                    } else {
                        final_header = asio::detail::socket_ops::host_to_network_long(
                            static_cast<uint32_t>(length));
                        write_buf = data1_;
                        write_len = length;
                    }

                    auto packet = std::make_shared<std::vector<uint8_t>>(
                        sizeof(final_header) + write_len);
                    std::memcpy(packet->data(), &final_header, sizeof(final_header));
                    std::memcpy(packet->data() + sizeof(final_header), write_buf, write_len);

                    auto write_op = [this, self, packet]() {
                        s2_->async_write(asio::buffer(*packet),
                            [this, self, packet](std::error_code ec2, std::size_t) {
                                if (!ec2) {
                                    DoRead(1);
                                } else {
                                    s1_->close(); s2_->close();
                                }
                            });
                    };

                    if (rate_limiter_) {
                        rate_limiter_->async_wait(length, std::move(write_op));
                    } else {
                        write_op();
                    }
                } else {
                    s1_->close(); s2_->close();
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
                        s1_->close(); s2_->close(); return;
                    }

                    auto body = std::make_shared<std::vector<uint8_t>>(len);
                    s2_->async_read(asio::buffer(*body),
                        [this, self, body, compressed](std::error_code ec2, std::size_t) {
                            if (!ec2) {
                                if (compressed) {
                                    unsigned long long const decodedSize =
                                        ZSTD_getFrameContentSize(body->data(), body->size());
                                    if (decodedSize == ZSTD_CONTENTSIZE_ERROR ||
                                        decodedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
                                        decodedSize > protocol::MAX_DECOMPRESSED_SIZE) {
                                        s1_->close(); s2_->close(); return;
                                    }
                                    auto decompressed =
                                        std::make_shared<std::vector<uint8_t>>(decodedSize);
                                    size_t const dSize = ZSTD_decompress(
                                        decompressed->data(), decodedSize,
                                        body->data(), body->size());
                                    if (ZSTD_isError(dSize)) {
                                        s1_->close(); s2_->close(); return;
                                    }
                                    decompressed->resize(dSize);

                                    auto write_op = [this, self, decompressed]() {
                                        s1_->async_write(asio::buffer(*decompressed),
                                            [this, self, decompressed](std::error_code ec3, std::size_t) {
                                                if (!ec3) DoRead(2);
                                                else { s1_->close(); s2_->close(); }
                                            });
                                    };

                                    if (rate_limiter_) {
                                        rate_limiter_->async_wait(decompressed->size(),
                                                                   std::move(write_op));
                                    } else {
                                        write_op();
                                    }
                                } else {
                                    auto write_op = [this, self, body]() {
                                        s1_->async_write(asio::buffer(*body),
                                            [this, self, body](std::error_code ec3, std::size_t) {
                                                if (!ec3) DoRead(2);
                                                else { s1_->close(); s2_->close(); }
                                            });
                                    };

                                    if (rate_limiter_) {
                                        rate_limiter_->async_wait(body->size(),
                                                                   std::move(write_op));
                                    } else {
                                        write_op();
                                    }
                                }
                            } else {
                                s1_->close(); s2_->close();
                            }
                        });
                } else {
                    s1_->close(); s2_->close();
                }
            });
    }
}

} // namespace common
} // namespace cfrp
