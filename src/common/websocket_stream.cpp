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

#include "common/websocket_stream.h"
#include "common/websocket_utils.h"
#include "common/protocol.h"
#include <iostream>
#include <sstream>
#include <array>

namespace cfrp {
namespace common {

WebsocketStream::WebsocketStream(std::shared_ptr<AsyncStream> underlying, bool is_client, bool perform_underlying_handshake, std::shared_ptr<BufferPool> buffer_pool)
    : underlying_(std::move(underlying)), 
      buffer_pool_(std::move(buffer_pool)),
      is_client_(is_client), 
      perform_underlying_handshake_(perform_underlying_handshake) {
    if (!buffer_pool_) buffer_pool_ = BufferPool::CreateDefault();
    std::random_device rd;
    rng_.seed(rd());
}

void WebsocketStream::async_handshake(ssl::stream_base::handshake_type type, std::function<void(std::error_code)> handler) {
    auto self = shared_from_this();
    if (perform_underlying_handshake_) {
        underlying_->async_handshake(type, [this, self, handler](std::error_code ec) {
            if (ec) {
                handler(ec);
                return;
            }
            if (is_client_) {
                DoClientHandshake(handler);
            } else {
                DoServerHandshake(handler);
            }
        });
    } else {
        if (is_client_) {
            DoClientHandshake(handler);
        } else {
            DoServerHandshake(handler);
        }
    }
}

void WebsocketStream::DoClientHandshake(std::function<void(std::error_code)> handler) {
    auto self = shared_from_this();
    std::array<uint8_t, 16> nonce{};
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& b : nonce) {
        b = static_cast<uint8_t>(dist(rng_));
    }
    std::string key = WebSocketUtils::GenerateClientKey(nonce.data(), nonce.size());
    
    std::stringstream ss;
    ss << "GET / HTTP/1.1\r\n";
    ss << "Host: cfrp\r\n";
    ss << "Upgrade: websocket\r\n";
    ss << "Connection: Upgrade\r\n";
    ss << "Sec-WebSocket-Key: " << key << "\r\n";
    ss << "Sec-WebSocket-Version: 13\r\n\r\n";
    
    auto request = std::make_shared<std::string>(ss.str());
    auto key_ptr = std::make_shared<std::string>(key);
    underlying_->async_write(asio::buffer(*request), [this, self, request, key_ptr, handler](std::error_code ec, std::size_t) {
        if (ec) {
            handler(ec);
            return;
        }

        auto response_buf = std::make_shared<std::vector<char>>(1024);
        underlying_->async_read_some(asio::buffer(*response_buf), [this, self, response_buf, key_ptr, handler](std::error_code ec, std::size_t length) {
            if (ec) {
                handler(ec);
                return;
            }
            // Simple check for 101 Switching Protocols
            std::string resp(response_buf->data(), length);
            if (resp.find("101 Switching Protocols") != std::string::npos && WebSocketUtils::HasValidAcceptHeader(resp, *key_ptr)) {
                handshaked_ = true;
                handler(std::error_code());
            } else {
                handler(asio::error::access_denied);
            }
        });
    });
}

void WebsocketStream::DoServerHandshake(std::function<void(std::error_code)> handler) {
    auto request_buf = std::make_shared<std::string>();
    auto temp_buf = std::make_shared<std::vector<char>>(1024);
    DoServerHandshakeRead(request_buf, temp_buf, handler);
}

void WebsocketStream::DoServerHandshakeRead(std::shared_ptr<std::string> request_buf, std::shared_ptr<std::vector<char>> temp_buf, std::function<void(std::error_code)> handler) {
    auto self = shared_from_this();
    underlying_->async_read_some(asio::buffer(*temp_buf), [this, self, request_buf, temp_buf, handler](std::error_code ec, std::size_t length) {
        if (ec) {
            handler(ec);
            return;
        }
        
        request_buf->append(temp_buf->data(), length);
        if (request_buf->find("\r\n\r\n") != std::string::npos) {
            std::string req = *request_buf;
            size_t key_pos = req.find("Sec-WebSocket-Key: ");
            if (key_pos == std::string::npos) {
                handler(asio::error::invalid_argument);
                return;
            }
            
            size_t key_end = req.find("\r\n", key_pos);
            std::string client_key = req.substr(key_pos + 19, key_end - (key_pos + 19));
            std::string accept_key = WebSocketUtils::GenerateAcceptKey(client_key);
            
            std::stringstream ss;
            ss << "HTTP/1.1 101 Switching Protocols\r\n";
            ss << "Upgrade: websocket\r\n";
            ss << "Connection: Upgrade\r\n";
            ss << "Sec-WebSocket-Accept: " << accept_key << "\r\n\r\n";
            
            auto response = std::make_shared<std::string>(ss.str());
            underlying_->async_write(asio::buffer(*response), [this, self, response, handler](std::error_code ec, std::size_t) {
                if (!ec) handshaked_ = true;
                handler(ec);
            });
        } else if (request_buf->size() > 8192) {
            handler(asio::error::message_size);
        } else {
            DoServerHandshakeRead(request_buf, temp_buf, handler);
        }
    });
}

void WebsocketStream::async_write(asio::const_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    std::vector<asio::const_buffer> buffers;
    buffers.push_back(buffer);
    async_write(buffers, handler);
}

void WebsocketStream::async_write(const std::vector<asio::const_buffer>& buffers, std::function<void(std::error_code, std::size_t)> handler) {
    auto self = shared_from_this();
    size_t total_payload_len = 0;
    for (const auto& b : buffers) total_payload_len += b.size();
    
    auto header = std::make_shared<std::vector<uint8_t>>();
    header->push_back(0x82); // FIN + Binary
    
    uint8_t mask_bit = is_client_ ? 0x80 : 0x00;
    
    if (total_payload_len <= 125) {
        header->push_back(mask_bit | (uint8_t)total_payload_len);
    } else if (total_payload_len <= 65535) {
        header->push_back(mask_bit | 126);
        header->push_back((total_payload_len >> 8) & 0xFF);
        header->push_back(total_payload_len & 0xFF);
    } else {
        header->push_back(mask_bit | 127);
        for (int i = 7; i >= 0; --i) {
            header->push_back((total_payload_len >> (i * 8)) & 0xFF);
        }
    }
    
    if (is_client_) {
        uint8_t mask[4];
        std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
        uint32_t m = dist(rng_);
        std::memcpy(mask, &m, 4);
        header->insert(header->end(), mask, mask + 4);

        // Client MUST mask. We have to copy because user's buffers are const and we cannot modify them.
        auto masked_payload = std::make_shared<std::vector<uint8_t>>(total_payload_len);
        size_t offset = 0;
        for (const auto& b : buffers) {
            if (b.size() > 0) {
                std::memcpy(masked_payload->data() + offset, b.data(), b.size());
                offset += b.size();
            }
        }
        for (size_t i = 0; i < total_payload_len; ++i) {
            (*masked_payload)[i] ^= mask[i % 4];
        }

        std::vector<asio::const_buffer> write_buffers;
        write_buffers.push_back(asio::buffer(*header));
        write_buffers.push_back(asio::buffer(*masked_payload));

        underlying_->async_write(write_buffers, [this, self, header, masked_payload, handler, total_payload_len](std::error_code ec, std::size_t) {
            handler(ec, total_payload_len);
        });
    } else {
        // Server side: NO MASKING. True zero-copy scatter-gather!
        std::vector<asio::const_buffer> write_buffers;
        write_buffers.reserve(1 + buffers.size());
        write_buffers.push_back(asio::buffer(*header));
        for (const auto& b : buffers) {
            if (b.size() > 0) {
                write_buffers.push_back(b);
            }
        }

        underlying_->async_write(write_buffers, [this, self, header, handler, total_payload_len](std::error_code ec, std::size_t) {
            handler(ec, total_payload_len);
        });
    }
}

void WebsocketStream::async_read_some(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    // For simplicity, each async_read_some will read one full WS frame.
    // In a production-grade implementation, we'd need to handle fragmentation and multi-frame reads.
    ReadWsFrame(handler, buffer);
}

void WebsocketStream::async_read(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    auto self = shared_from_this();
    if (buffer.size() == 0) {
        asio::post(underlying_->get_executor(), [handler]() {
            handler(std::error_code(), 0);
        });
        return;
    }

    if (read_remaining_ > 0) {
        size_t to_copy = std::min(read_remaining_, buffer.size());
        std::memcpy(buffer.data(), read_buffer_.get() + read_offset_, to_copy);
        read_offset_ += to_copy;
        read_remaining_ -= to_copy;
        
        if (to_copy < buffer.size()) {
            // Still need more data
            void* next_ptr = static_cast<uint8_t*>(buffer.data()) + to_copy;
            size_t next_size = buffer.size() - to_copy;
            underlying_->async_read(asio::mutable_buffer(next_ptr, next_size), [this, self, handler, to_copy](std::error_code ec, std::size_t length) {
                handler(ec, to_copy + length);
            });
        } else {
            asio::post(underlying_->get_executor(), [handler, to_copy]() {
                handler(std::error_code(), to_copy);
            });
        }
        return;
    }

    ReadWsFrame([this, self, buffer, handler](std::error_code ec, std::size_t length) {
        if (ec) {
            handler(ec, 0);
            return;
        }
        
        size_t to_copy = std::min(length, buffer.size());
        std::memcpy(buffer.data(), read_buffer_.get() + read_offset_, to_copy);
        
        if (length > to_copy) {
            read_offset_ += to_copy;
            read_remaining_ = length - to_copy;
        } else {
            read_offset_ = 0;
            read_remaining_ = 0;
        }
        
        if (to_copy < buffer.size()) {
            void* next_ptr = static_cast<uint8_t*>(buffer.data()) + to_copy;
            size_t next_size = buffer.size() - to_copy;
            underlying_->async_read(asio::mutable_buffer(next_ptr, next_size), [this, self, handler, to_copy](std::error_code ec, std::size_t length) {
                handler(ec, to_copy + length);
            });
        } else {
            handler(std::error_code(), to_copy);
        }
    }, buffer);
}

void WebsocketStream::ReadWsFrame(std::function<void(std::error_code, std::size_t)> handler, asio::mutable_buffer user_buffer) {
    auto self = shared_from_this();
    auto header = std::make_shared<std::vector<uint8_t>>(2);
    
    underlying_->async_read(asio::buffer(*header), [this, self, header, handler](std::error_code ec, std::size_t) {
        if (ec) {
            handler(ec, 0);
            return;
        }
        
        uint8_t b0 = (*header)[0];
        uint8_t b1 = (*header)[1];
        
        // FIN + Opcode check (ignore pings/pongs/closes for now)
        // uint8_t opcode = b0 & 0x0F;
        
        bool masked = (b1 & 0x80) != 0;
        uint64_t payload_len = b1 & 0x7F;
        
        auto next_step = [this, self, masked, handler](uint64_t len) {
            if (len > protocol::MAX_MESSAGE_SIZE) {
                close();
                handler(asio::error::no_buffer_space, 0);
                return;
            }
            size_t extra = masked ? 4 : 0;
            auto payload = buffer_pool_->Get(len + extra);
            
            underlying_->async_read(asio::buffer(payload.get(), len + extra), [this, self, payload, masked, len, handler](std::error_code ec, std::size_t) {
                if (ec) {
                    handler(ec, 0);
                    return;
                }
                
                uint8_t* data = payload.get();
                if (masked) {
                    uint8_t mask[4];
                    std::memcpy(mask, data, 4);
                    data += 4;
                    for (size_t i = 0; i < len; ++i) {
                        data[i] ^= mask[i % 4];
                    }
                }
                
                read_buffer_ = payload;
                if (masked) {
                    // Adjust read_buffer_ to point past the mask if we want to be strictly correct,
                    // but since we return shared_ptr, we just need to remember the offset.
                    // Actually, for simplicity, we'll just copy if masked, or use an offset.
                    // Let's use an offset.
                    read_offset_ = 4;
                } else {
                    read_offset_ = 0;
                }
                read_remaining_ = len;
                
                // If we use an offset, we should adjust how async_read uses read_buffer_.get().
                // I've already updated async_read to use read_buffer_.get() + read_offset_.
                
                handler(std::error_code(), len);
            });
        };
        
        if (payload_len == 126) {
            auto ext = std::make_shared<std::vector<uint8_t>>(2);
            underlying_->async_read(asio::buffer(*ext), [this, self, ext, next_step, handler](std::error_code ec, std::size_t) {
                if (ec) { handler(ec, 0); return; }
                uint16_t len = ((*ext)[0] << 8) | (*ext)[1];
                next_step(len);
            });
        } else if (payload_len == 127) {
            auto ext = std::make_shared<std::vector<uint8_t>>(8);
            underlying_->async_read(asio::buffer(*ext), [this, self, ext, next_step, handler](std::error_code ec, std::size_t) {
                if (ec) { handler(ec, 0); return; }
                uint64_t len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | (*ext)[i];
                next_step(len);
            });
        } else {
            next_step(payload_len);
        }
    });
}

void WebsocketStream::set_host_name(const std::string& host_name) {
    underlying_->set_host_name(host_name);
}

void WebsocketStream::close() {
    underlying_->close();
}

asio::any_io_executor WebsocketStream::get_executor() {
    return underlying_->get_executor();
}

std::string WebsocketStream::remote_endpoint_string() {
    return underlying_->remote_endpoint_string();
}

std::string WebsocketStream::protocol_name() {
    return "WebSocket";
}

} // namespace common
} // namespace cfrp
