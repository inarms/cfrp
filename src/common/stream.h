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

#ifndef ASIO_USE_WOLFSSL
#define ASIO_USE_WOLFSSL
#endif

#include <asio.hpp>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/openssl/ssl.h>
#include <asio/ssl.hpp>
#include <memory>
#include <variant>
#include <functional>

namespace cfrp {
namespace common {

using asio::ip::tcp;
namespace ssl = asio::ssl;

class AsyncStream : public std::enable_shared_from_this<AsyncStream> {
public:
    virtual ~AsyncStream() = default;
    
    virtual void async_read_some(asio::mutable_buffer buffer, 
                                 std::function<void(std::error_code, std::size_t)> handler) = 0;
                                 
    virtual void async_write(asio::const_buffer buffer, 
                             std::function<void(std::error_code, std::size_t)> handler) = 0;

    virtual void async_read(asio::mutable_buffer buffer,
                            std::function<void(std::error_code, std::size_t)> handler) = 0;

    virtual void async_handshake(ssl::stream_base::handshake_type type,
                                 std::function<void(std::error_code)> handler) = 0;

    virtual void set_host_name(const std::string& host_name) {}

    virtual void close() = 0;
    virtual asio::any_io_executor get_executor() = 0;
    virtual std::string remote_endpoint_string() = 0;
    virtual std::string protocol_name() = 0;
};

class TcpStream : public AsyncStream {
public:
    explicit TcpStream(tcp::socket socket) : socket_(std::move(socket)) {}
    
    void async_read_some(asio::mutable_buffer buffer, 
                         std::function<void(std::error_code, std::size_t)> handler) override {
        socket_.async_read_some(buffer, std::move(handler));
    }
    
    void async_write(asio::const_buffer buffer, 
                     std::function<void(std::error_code, std::size_t)> handler) override {
        asio::async_write(socket_, buffer, std::move(handler));
    }

    void async_read(asio::mutable_buffer buffer,
                    std::function<void(std::error_code, std::size_t)> handler) override {
        asio::async_read(socket_, buffer, std::move(handler));
    }

    void async_handshake(ssl::stream_base::handshake_type,
                         std::function<void(std::error_code)> handler) override {
        // No-op for TCP
        asio::post(socket_.get_executor(), [handler]() {
            handler(std::error_code());
        });
    }

    void close() override {
        std::error_code ec;
        socket_.close(ec);
    }

    asio::any_io_executor get_executor() override {
        return socket_.get_executor();
    }

    std::string remote_endpoint_string() override {
        std::error_code ec;
        auto ep = socket_.remote_endpoint(ec);
        if (ec) return "unknown";
        return ep.address().to_string() + ":" + std::to_string(ep.port());
    }

    std::string protocol_name() override {
        return "TCP";
    }

private:
    tcp::socket socket_;
};

class SslStream : public AsyncStream {
public:
    explicit SslStream(tcp::socket socket, ssl::context& ctx) 
        : stream_(std::move(socket), ctx) {
#ifdef ASIO_USE_WOLFSSL
        wolfSSL_set_using_nonblock(stream_.native_handle(), 1);
#endif
    }
    
    void async_read_some(asio::mutable_buffer buffer, 
                         std::function<void(std::error_code, std::size_t)> handler) override {
        stream_.async_read_some(buffer, std::move(handler));
    }
    
    void async_write(asio::const_buffer buffer, 
                     std::function<void(std::error_code, std::size_t)> handler) override {
        asio::async_write(stream_, buffer, std::move(handler));
    }

    void async_read(asio::mutable_buffer buffer,
                    std::function<void(std::error_code, std::size_t)> handler) override {
        asio::async_read(stream_, buffer, std::move(handler));
    }

    void async_handshake(ssl::stream_base::handshake_type type,
                         std::function<void(std::error_code)> handler) override {
        stream_.async_handshake(type, std::move(handler));
    }

    void set_host_name(const std::string& host_name) override {
#ifdef ASIO_USE_WOLFSSL
        wolfSSL_set_tlsext_host_name(stream_.native_handle(), host_name.c_str());
#else
        SSL_set_tlsext_host_name(stream_.native_handle(), host_name.c_str());
#endif
    }

    void close() override {
        std::error_code ec;
        stream_.next_layer().close(ec);
    }

    asio::any_io_executor get_executor() override {
        return stream_.get_executor();
    }

    std::string remote_endpoint_string() override {
        std::error_code ec;
        auto ep = stream_.next_layer().remote_endpoint(ec);
        if (ec) return "unknown";
        return ep.address().to_string() + ":" + std::to_string(ep.port());
    }

    std::string protocol_name() override {
        return "SSL";
    }

private:
    ssl::stream<tcp::socket> stream_;
};

class BufferedStream : public AsyncStream {
public:
    BufferedStream(std::shared_ptr<AsyncStream> underlying, std::vector<uint8_t> initial_data)
        : underlying_(std::move(underlying)), buffer_(std::move(initial_data)) {}

    void async_read_some(asio::mutable_buffer buffer, 
                         std::function<void(std::error_code, std::size_t)> handler) override {
        if (!buffer_.empty()) {
            size_t to_copy = std::min(buffer_.size(), buffer.size());
            std::memcpy(buffer.data(), buffer_.data(), to_copy);
            buffer_.erase(buffer_.begin(), buffer_.begin() + to_copy);
            asio::post(underlying_->get_executor(), [handler, to_copy]() {
                handler(std::error_code(), to_copy);
            });
            return;
        }
        underlying_->async_read_some(buffer, std::move(handler));
    }

    void async_read(asio::mutable_buffer buffer,
                    std::function<void(std::error_code, std::size_t)> handler) override {
        if (!buffer_.empty()) {
            size_t to_copy = std::min(buffer_.size(), buffer.size());
            std::memcpy(buffer.data(), buffer_.data(), to_copy);
            buffer_.erase(buffer_.begin(), buffer_.begin() + to_copy);
            
            if (to_copy < buffer.size()) {
                void* next_ptr = static_cast<uint8_t*>(buffer.data()) + to_copy;
                size_t next_size = buffer.size() - to_copy;
                underlying_->async_read(asio::mutable_buffer(next_ptr, next_size), 
                    [handler, to_copy](std::error_code ec, std::size_t length) {
                        handler(ec, to_copy + length);
                    });
            } else {
                asio::post(underlying_->get_executor(), [handler, to_copy]() {
                    handler(std::error_code(), to_copy);
                });
            }
            return;
        }
        underlying_->async_read(buffer, std::move(handler));
    }

    void async_write(asio::const_buffer buffer, 
                     std::function<void(std::error_code, std::size_t)> handler) override {
        underlying_->async_write(buffer, std::move(handler));
    }

    void async_handshake(ssl::stream_base::handshake_type type,
                         std::function<void(std::error_code)> handler) override {
        underlying_->async_handshake(type, std::move(handler));
    }

    void set_host_name(const std::string& host_name) override {
        underlying_->set_host_name(host_name);
    }

    void close() override { underlying_->close(); }
    asio::any_io_executor get_executor() override { return underlying_->get_executor(); }
    std::string remote_endpoint_string() override { return underlying_->remote_endpoint_string(); }
    std::string protocol_name() override { return underlying_->protocol_name(); }

private:
    std::shared_ptr<AsyncStream> underlying_;
    std::vector<uint8_t> buffer_;
};

} // namespace common
} // namespace cfrp
