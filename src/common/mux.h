#pragma once

#include <asio.hpp>
#include <memory>
#include <vector>
#include <map>
#include <deque>
#include <mutex>
#include <functional>
#include "common/stream.h"

namespace cfrp {
namespace common {

namespace mux {

enum class Type : uint8_t {
    Data = 0x0,
    WindowUpdate = 0x1,
    Ping = 0x2,
    GoAway = 0x3
};

struct Flags {
    static constexpr uint16_t SYN = 0x1;
    static constexpr uint16_t ACK = 0x2;
    static constexpr uint16_t FIN = 0x4;
    static constexpr uint16_t RST = 0x8;
};

struct Header {
    uint8_t version = 0;
    uint8_t type = 0;
    uint16_t flags = 0;
    uint32_t stream_id = 0;
    uint32_t length = 0;

    static constexpr size_t size = 12;

    void encode(uint8_t* buf) const;
    static Header decode(const uint8_t* buf);
};

class Session;

class MuxStream : public AsyncStream {
public:
    MuxStream(uint32_t id, std::shared_ptr<Session> session);
    ~MuxStream() override;

    // AsyncStream interface
    void async_read_some(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) override;
    void async_write(asio::const_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) override;
    void async_read(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) override;
    void async_handshake(asio::ssl::stream_base::handshake_type type, std::function<void(std::error_code)> handler) override;
    void close() override;
    asio::any_io_executor get_executor() override;
    std::string remote_endpoint_string() override;
    std::string protocol_name() override;

    // Internal methods used by Session
    void handle_data(std::vector<uint8_t> data);
    void handle_window_update(uint32_t delta);
    void handle_close();

    uint32_t id() const { return id_; }

private:
    void do_read_from_buffer();

    uint32_t id_;
    std::weak_ptr<Session> session_;
    size_t local_window_size_;
    size_t remote_window_size_;
    std::vector<uint8_t> read_buffer_;
    size_t read_buffer_offset_ = 0;
    std::mutex mutex_;
    
    struct PendingRead {
        asio::mutable_buffer buffer;
        std::function<void(std::error_code, std::size_t)> handler;
        bool read_all;
    };
    std::deque<PendingRead> pending_reads_;
    bool closed_ = false;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(std::shared_ptr<AsyncStream> underlying_stream, bool is_server);
    void start(std::function<void(std::shared_ptr<MuxStream>)> on_new_stream);
    void stop();

    std::shared_ptr<MuxStream> open_stream();
    void async_send_frame(Header h, std::vector<uint8_t> body, std::function<void(std::error_code)> handler = nullptr);

    asio::any_io_executor get_executor() { return underlying_stream_->get_executor(); }
    std::string remote_endpoint_string() { return underlying_stream_->remote_endpoint_string(); }
    std::string protocol_name() { return underlying_stream_->protocol_name(); }

private:
    void do_read_header();
    void do_read_body(Header h);
    void handle_frame(Header h, std::vector<uint8_t> body);
    void do_write();

    std::shared_ptr<AsyncStream> underlying_stream_;
    bool is_server_;
    uint32_t next_stream_id_;
    std::function<void(std::shared_ptr<MuxStream>)> on_new_stream_;
    
    std::map<uint32_t, std::shared_ptr<MuxStream>> streams_;
    std::mutex streams_mutex_;
    
    struct PendingWrite {
        std::vector<uint8_t> data;
        std::function<void(std::error_code)> handler;
    };
    std::deque<std::shared_ptr<PendingWrite>> write_queue_;
    std::mutex write_mutex_;
    bool is_writing_ = false;
    
    uint8_t header_buf_[Header::size];
};

} // namespace mux

} // namespace common
} // namespace cfrp
