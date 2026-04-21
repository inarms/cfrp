#pragma once

#include <asio.hpp>
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
    virtual tcp::socket& lowest_layer() = 0;
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

    tcp::socket& lowest_layer() override {
        return socket_;
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

    tcp::socket& lowest_layer() override {
        return stream_.next_layer();
    }

private:
    ssl::stream<tcp::socket> stream_;
};

} // namespace common
} // namespace cfrp
