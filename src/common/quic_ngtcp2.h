/*
 * Copyright 2026 neesonqk
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
#include <asio.hpp>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <ngtcp2/ngtcp2.h>
#include <memory>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <sys/socket.h>

namespace cfrp {
namespace common {
namespace quic {

class QuicSession;

class QuicStream : public AsyncStream {
public:
    QuicStream(int64_t stream_id, std::shared_ptr<QuicSession> session);
    ~QuicStream() override;

    void async_read_some(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) override;
    void async_write(asio::const_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) override;
    void async_read(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) override;
    void async_handshake(ssl::stream_base::handshake_type type, std::function<void(std::error_code)> handler) override;
    void close() override;
    asio::any_io_executor get_executor() override;
    std::string remote_endpoint_string() override;
    std::string protocol_name() override;

    // Internal
    void handle_data(const uint8_t* data, size_t len);
    void handle_close();

private:
    void do_read();

    int64_t stream_id_;
    std::weak_ptr<QuicSession> session_;
    std::deque<uint8_t> read_buf_;
    struct PendingRead {
        asio::mutable_buffer buffer;
        std::function<void(std::error_code, std::size_t)> handler;
        bool read_all;
    };
    std::deque<PendingRead> pending_reads_;
    bool closed_ = false;
    std::mutex mutex_;
};

class QuicSession : public std::enable_shared_from_this<QuicSession> {
public:
    QuicSession(asio::ip::udp::socket& socket, asio::ip::udp::endpoint remote_endpoint, bool is_server);
    ~QuicSession();

    void init(WOLFSSL_CTX* ssl_ctx, const ngtcp2_cid* dcid = nullptr, const ngtcp2_cid* scid = nullptr);
    void handle_packet(const uint8_t* data, size_t len);
    void send_packets();
    
    void set_on_connected(std::function<void(std::shared_ptr<QuicSession>)> cb) { on_connected_cb_ = std::move(cb); }
    void set_on_new_stream(std::function<void(std::shared_ptr<QuicStream>)> cb) { on_new_stream_cb_ = std::move(cb); }
    void set_on_closed(std::function<void(std::shared_ptr<QuicSession>)> cb) { on_closed_cb_ = std::move(cb); }
    void trigger_connected() { if (on_connected_cb_) on_connected_cb_(shared_from_this()); }
    
    std::shared_ptr<QuicStream> open_stream();
    void close_session();
    void close_stream(int64_t stream_id);
    void write_stream(int64_t stream_id, const uint8_t* data, size_t len);

    asio::any_io_executor get_executor() { return socket_.get_executor(); }
    std::string remote_endpoint_string() { return remote_endpoint_.address().to_string() + ":" + std::to_string(remote_endpoint_.port()); }

    // Internal callbacks
    int on_stream_data(int64_t stream_id, const uint8_t* data, size_t len);
    int on_stream_close(int64_t stream_id);

    ngtcp2_conn* conn() { return conn_; }

private:
    void schedule_timer();
    void check_closed();

    asio::ip::udp::socket& socket_;
    asio::ip::udp::endpoint remote_endpoint_;
    bool is_server_;
    bool closed_notified_ = false;
    
    ngtcp2_conn* conn_ = nullptr;
    WOLFSSL* ssl_ = nullptr;
    
    // Persistent storage for ngtcp2_path to avoid temporary pointer issues
    ngtcp2_path path_;
    struct sockaddr_storage local_addr_;
    struct sockaddr_storage remote_addr_;

    std::map<int64_t, std::shared_ptr<QuicStream>> streams_;
    std::mutex streams_mutex_;
    
    asio::steady_timer timer_;
    std::function<void(std::shared_ptr<QuicSession>)> on_connected_cb_;
    std::function<void(std::shared_ptr<QuicStream>)> on_new_stream_cb_;
    std::function<void(std::shared_ptr<QuicSession>)> on_closed_cb_;
};

} // namespace quic
} // namespace common
} // namespace cfrp
