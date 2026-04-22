#pragma once

#ifndef ASIO_USE_WOLFSSL
#define ASIO_USE_WOLFSSL
#endif

#include <asio.hpp>
#include <wolfssl/options.h>
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
        : stream_(std::move(socket), ctx) {}
    
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

} // namespace common
} // namespace cfrp
