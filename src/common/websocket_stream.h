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
#include <vector>
#include <string>
#include <random>

namespace cfrp {
namespace common {

class WebsocketStream : public AsyncStream {
public:
    WebsocketStream(std::shared_ptr<AsyncStream> underlying, bool is_client, bool perform_underlying_handshake = true);

    void async_read_some(asio::mutable_buffer buffer, 
                         std::function<void(std::error_code, std::size_t)> handler) override;
                                 
    void async_write(asio::const_buffer buffer, 
                     std::function<void(std::error_code, std::size_t)> handler) override;

    void async_read(asio::mutable_buffer buffer,
                    std::function<void(std::error_code, std::size_t)> handler) override;

    void async_handshake(ssl::stream_base::handshake_type type,
                         std::function<void(std::error_code)> handler) override;

    virtual void set_host_name(const std::string& host_name) override;

    void close() override;
    asio::any_io_executor get_executor() override;
    std::string remote_endpoint_string() override;
    std::string protocol_name() override;

private:
    void DoClientHandshake(std::function<void(std::error_code)> handler);
    void DoServerHandshake(std::function<void(std::error_code)> handler);
    
    void ReadWsFrame(std::function<void(std::error_code, std::size_t)> handler, asio::mutable_buffer user_buffer);

    std::shared_ptr<AsyncStream> underlying_;
    bool is_client_;
    bool perform_underlying_handshake_;
    bool handshaked_ = false;
    
    // Internal buffer for reading WS frames
    std::vector<uint8_t> read_buffer_;
    size_t read_offset_ = 0;
    size_t read_remaining_ = 0;
    
    std::mt19937 rng_;
};

} // namespace common
} // namespace cfrp
