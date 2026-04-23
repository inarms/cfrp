#pragma once

#include "common/stream.h"
#include <vector>
#include <string>
#include <random>

namespace cfrp {
namespace common {

class WebsocketStream : public AsyncStream {
public:
    WebsocketStream(std::shared_ptr<AsyncStream> underlying, bool is_client);

    void async_read_some(asio::mutable_buffer buffer, 
                         std::function<void(std::error_code, std::size_t)> handler) override;
                                 
    void async_write(asio::const_buffer buffer, 
                     std::function<void(std::error_code, std::size_t)> handler) override;

    void async_read(asio::mutable_buffer buffer,
                    std::function<void(std::error_code, std::size_t)> handler) override;

    void async_handshake(ssl::stream_base::handshake_type type,
                         std::function<void(std::error_code)> handler) override;

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
    bool handshaked_ = false;
    
    // Internal buffer for reading WS frames
    std::vector<uint8_t> read_buffer_;
    size_t read_offset_ = 0;
    size_t read_remaining_ = 0;
    
    std::mt19937 rng_;
};

} // namespace common
} // namespace cfrp
