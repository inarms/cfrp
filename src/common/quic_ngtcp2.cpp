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

#include "common/quic_ngtcp2.h"
#include "common/utils.h"
#include <unordered_map>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/quic.h>
#include <wolfssl/wolfcrypt/random.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>

namespace cfrp {
namespace common {
namespace quic {

// --- QuicStream Implementation ---

QuicStream::QuicStream(int64_t stream_id, std::shared_ptr<QuicSession> session)
    : stream_id_(stream_id), session_(session) {}

QuicStream::~QuicStream() {
    // Synchronous cleanup only in destructor to avoid bad_weak_ptr
    closed_ = true;
    for (auto& pr : pending_reads_) {
        auto h = std::move(pr.handler);
        asio::post(get_executor(), [h]() { h(asio::error::operation_aborted, 0); });
    }
    pending_reads_.clear();
}

void QuicStream::async_read_some(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    auto session = session_.lock();
    if (!session) {
        asio::post(get_executor(), [handler]() { handler(asio::error::connection_reset, 0); });
        return;
    }
    if (!session->strand().running_in_this_thread()) {
        asio::post(session->strand(), [this, self = shared_from_this(), buffer, handler]() {
            async_read_some(buffer, handler);
        });
        return;
    }
    pending_reads_.push_back({buffer, std::move(handler), false});
    do_read();
}

void QuicStream::async_write(asio::const_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    auto session = session_.lock();
    if (!session) {
        asio::post(get_executor(), [handler]() { handler(asio::error::connection_reset, 0); });
        return;
    }

    session->write_stream(stream_id_, static_cast<const uint8_t*>(buffer.data()), buffer.size(), std::move(handler));
}

void QuicStream::async_write(const std::vector<asio::const_buffer>& buffers, std::function<void(std::error_code, std::size_t)> handler) {
    auto session = session_.lock();
    if (!session) {
        asio::post(get_executor(), [handler]() { handler(asio::error::connection_reset, 0); });
        return;
    }

    session->write_stream(stream_id_, buffers, std::move(handler));
}

void QuicStream::async_read(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    auto session = session_.lock();
    if (!session) {
        asio::post(get_executor(), [handler]() { handler(asio::error::connection_reset, 0); });
        return;
    }
    if (!session->strand().running_in_this_thread()) {
        asio::post(session->strand(), [this, self = shared_from_this(), buffer, handler]() {
            async_read(buffer, handler);
        });
        return;
    }
    pending_reads_.push_back({buffer, std::move(handler), true});
    do_read();
}

void QuicStream::async_handshake(ssl::stream_base::handshake_type, std::function<void(std::error_code)> handler) {
    asio::post(get_executor(), [handler]() { handler(std::error_code()); });
}

void QuicStream::close() {
    auto session = session_.lock();
    if (!session) return;
    if (!session->strand().running_in_this_thread()) {
        asio::post(session->strand(), [this, self = shared_from_this()]() {
            close();
        });
        return;
    }
    if (closed_) return;
    closed_ = true;
    session->close_stream(stream_id_);
}

asio::any_io_executor QuicStream::get_executor() {
    auto session = session_.lock();
    if (session) return session->get_executor();
    // Return a default executor instead of throwing to avoid crashing during cleanup
    return asio::system_executor();
}

std::string QuicStream::remote_endpoint_string() {
    auto session = session_.lock();
    return session ? session->remote_endpoint_string() : "unknown";
}

std::string QuicStream::protocol_name() { return "QUIC"; }

void QuicStream::handle_data(std::shared_ptr<uint8_t[]> data, size_t len) {
    read_queue_.push_back({data, len});
    do_read();
}

void QuicStream::handle_close() {
    closed_ = true;
    do_read();
}

void QuicStream::do_read() {
    auto session = session_.lock();
    if (!session) return;

    while (!pending_reads_.empty()) {
        auto& pr = pending_reads_.front();
        
        if (read_queue_.empty()) {
            if (closed_) {
                auto h = std::move(pr.handler);
                pending_reads_.pop_front();
                asio::post(session->strand(), [h]() { h(asio::error::eof, 0); });
                continue;
            }
            break;
        }

        size_t total_copied = 0;
        size_t buffer_remaining = pr.buffer.size();

        // Check if we can fulfill read_all
        if (pr.read_all) {
            size_t available = 0;
            for (const auto& block : read_queue_) available += block.length;
            available -= read_queue_offset_;
            
            if (available < buffer_remaining && !closed_) {
                break; // Wait for more data
            }
        }

        while (buffer_remaining > 0 && !read_queue_.empty()) {
            auto& block = read_queue_.front();
            size_t block_available = block.length - read_queue_offset_;
            size_t to_copy = std::min(buffer_remaining, block_available);

            std::memcpy(static_cast<uint8_t*>(pr.buffer.data()) + total_copied,
                        block.data.get() + read_queue_offset_, to_copy);

            total_copied += to_copy;
            buffer_remaining -= to_copy;
            read_queue_offset_ += to_copy;

            if (read_queue_offset_ == block.length) {
                read_queue_.pop_front();
                read_queue_offset_ = 0;
            }
        }

        auto h = std::move(pr.handler);
        pending_reads_.pop_front();
        
        asio::post(session->strand(), [h, total_copied]() {
            h(std::error_code(), total_copied);
        });

        // Update QUIC flow control
        if (session->conn() && total_copied > 0) {
            ngtcp2_conn_extend_max_stream_offset(session->conn(), stream_id_, total_copied);
        }
    }
}

// --- QuicSession Implementation ---

namespace {
    ngtcp2_conn* get_conn(ngtcp2_crypto_conn_ref *conn_ref) {
        return static_cast<QuicSession*>(conn_ref->user_data)->conn();
    }

    int recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id, uint64_t offset, const uint8_t *data, size_t datalen, void *user_data, void *stream_user_data) {
        auto session = static_cast<QuicSession*>(user_data);
        session->on_stream_data(stream_id, data, datalen);
        if (flags & NGTCP2_STREAM_DATA_FLAG_FIN) {
            session->on_stream_close(stream_id);
        }
        return 0;
    }
    int stream_close(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id, uint64_t app_error_code, void *user_data, void *stream_user_data) {
        static_cast<QuicSession*>(user_data)->on_stream_close(stream_id);
        return 0;
    }
    int handshake_completed(ngtcp2_conn *conn, void *user_data) {
        Logger::Info("[QUIC] Handshake/Verification: SUCCESS");
        static_cast<QuicSession*>(user_data)->trigger_connected();
        return 0;
    }
    int get_new_connection_id(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token, size_t cidlen, void *user_data) {
        WC_RNG rng;
        if (wc_InitRng(&rng) != 0) return NGTCP2_ERR_CALLBACK_FAILURE;
        wc_RNG_GenerateBlock(&rng, cid->data, (word32)cidlen);
        cid->datalen = cidlen;
        wc_RNG_GenerateBlock(&rng, token, NGTCP2_STATELESS_RESET_TOKENLEN);
        wc_FreeRng(&rng);
        return 0;
    }
    void rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *rctx) {
        WC_RNG rng;
        if (wc_InitRng(&rng) != 0) return;
        wc_RNG_GenerateBlock(&rng, dest, (word32)destlen);
        wc_FreeRng(&rng);
    }
}

QuicSession::QuicSession(asio::ip::udp::socket& socket, asio::ip::udp::endpoint remote_endpoint, bool is_server, std::shared_ptr<BufferPool> buffer_pool)
    : socket_(socket), 
      strand_(asio::make_strand(socket.get_executor())), 
      buffer_pool_(buffer_pool),
      remote_endpoint_(remote_endpoint), 
      is_server_(is_server), 
      timer_(strand_) {
    
    if (!buffer_pool_) buffer_pool_ = BufferPool::CreateDefault();
    
    auto local_ep = socket_.local_endpoint();
    std::memcpy(&local_addr_, local_ep.data(), local_ep.size());
    std::memcpy(&remote_addr_, remote_endpoint_.data(), remote_endpoint_.size());

    path_.local.addr = (ngtcp2_sockaddr*)&local_addr_;
    path_.local.addrlen = static_cast<ngtcp2_socklen>(local_ep.size());
    path_.remote.addr = (ngtcp2_sockaddr*)&remote_addr_;
    path_.remote.addrlen = static_cast<ngtcp2_socklen>(remote_endpoint_.size());
}

QuicSession::~QuicSession() {
    for (auto& pair : streams_) {
        pair.second->handle_close();
    }
    if (conn_) ngtcp2_conn_del(conn_);
    if (ssl_) {
        auto conn_ref = static_cast<ngtcp2_crypto_conn_ref*>(wolfSSL_get_app_data(ssl_));
        delete conn_ref;
        wolfSSL_free(ssl_);
    }
}

void QuicSession::init(WOLFSSL_CTX* ssl_ctx, const ngtcp2_cid* client_dcid, const ngtcp2_cid* client_scid) {
    if (!ssl_ctx) return;

    if (is_server_) {
        ngtcp2_crypto_wolfssl_configure_server_context(ssl_ctx);
    } else {
        ngtcp2_crypto_wolfssl_configure_client_context(ssl_ctx);
    }

    ssl_ = wolfSSL_new(ssl_ctx);
    if (is_server_) wolfSSL_set_accept_state(ssl_); else wolfSSL_set_connect_state(ssl_);
    
    wolfSSL_UseALPN(ssl_, (char*)"cfrp", 4, WOLFSSL_ALPN_FAILED_ON_MISMATCH);

    auto conn_ref = new ngtcp2_crypto_conn_ref();
    conn_ref->get_conn = get_conn;
    conn_ref->user_data = this;
    wolfSSL_set_app_data(ssl_, conn_ref);

    ngtcp2_settings settings; ngtcp2_settings_default(&settings);
    settings.initial_ts = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
    
    ngtcp2_transport_params params; ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 10 * 1024 * 1024;
    params.initial_max_stream_data_bidi_remote = 10 * 1024 * 1024;
    params.initial_max_stream_data_uni = 10 * 1024 * 1024;
    params.initial_max_data = 20 * 1024 * 1024;
    params.initial_max_streams_bidi = 1000;
    params.initial_max_streams_uni = 1000;
    params.max_idle_timeout = 60 * NGTCP2_SECONDS;
    params.active_connection_id_limit = 8;

    ngtcp2_cid dcid, scid; 
    if (is_server_ && client_dcid && client_scid) {
        dcid = *client_scid;
        scid.datalen = 8;
        WC_RNG rng; wc_InitRng(&rng);
        wc_RNG_GenerateBlock(&rng, scid.data, 8);
        wc_FreeRng(&rng);
        params.original_dcid = *client_dcid;
        params.original_dcid_present = TRUE;
    } else {
        dcid.datalen = 8; scid.datalen = 8;
        WC_RNG rng; wc_InitRng(&rng);
        wc_RNG_GenerateBlock(&rng, dcid.data, 8); wc_RNG_GenerateBlock(&rng, scid.data, 8);
        wc_FreeRng(&rng);
    }

    ngtcp2_callbacks callbacks{};
    callbacks.recv_stream_data = recv_stream_data;
    callbacks.stream_close = stream_close;
    callbacks.get_new_connection_id = get_new_connection_id;
    callbacks.handshake_completed = handshake_completed;
    callbacks.rand = rand_cb;
    
    // Manually fill crypto callbacks with core functions from ngtcp2_crypto
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;

    uint32_t version = NGTCP2_PROTO_VER_V1;
    int res;
    if (is_server_) {
        callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
        res = ngtcp2_conn_server_new(&conn_, &dcid, &scid, &path_, version, &callbacks, &settings, &params, nullptr, this);
    } else {
        res = ngtcp2_conn_client_new(&conn_, &dcid, &scid, &path_, version, &callbacks, &settings, &params, nullptr, this);
    }
    
    if (res != 0) {
        Logger::Error("[QUIC] Failed to create ngtcp2 connection: " + std::to_string(res) + " (" + ngtcp2_strerror(res) + ")");
        return;
    }

    ngtcp2_conn_set_tls_native_handle(conn_, (void*)ssl_);
}

void QuicSession::handle_packet(const uint8_t* data, size_t len) {
    if (!strand_.running_in_this_thread()) {
        auto buf = std::make_shared<std::vector<uint8_t>>(data, data + len);
        asio::post(strand_, [this, self = shared_from_this(), buf]() {
            handle_packet(buf->data(), buf->size());
        });
        return;
    }

    std::lock_guard lock(ngtcp2_mutex_);
    if (!conn_) return;
    ngtcp2_tstamp ts = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
    int res = ngtcp2_conn_read_pkt(conn_, &path_, nullptr, data, len, ts);
    if (res < 0) {
        if (res != NGTCP2_ERR_DRAINING) {
            Logger::Error("[QUIC] Packet read error: " + std::string(ngtcp2_strerror(res)) + " (" + std::to_string(res) + ")");
        }
        if (res == NGTCP2_ERR_DROP_CONN) {
            check_closed();
            return;
        }
        
        // Protocol error, send connection close
        uint8_t buf[1500];
        ngtcp2_pkt_info pi;
        ngtcp2_ccerr ccerr;
        ngtcp2_ccerr_default(&ccerr);
        ngtcp2_ssize cc_res = ngtcp2_conn_write_connection_close(conn_, &path_, &pi, buf, sizeof(buf), &ccerr, ts);
        if (cc_res > 0) {
            std::error_code ec;
            if (is_server_) {
                socket_.send_to(asio::buffer(buf, (size_t)cc_res), remote_endpoint_, 0, ec);
            } else {
                socket_.send(asio::buffer(buf, (size_t)cc_res), 0, ec);
            }
        }
        check_closed();
        return;
    }
    send_packets();
    check_closed();
}

void QuicSession::send_packets() {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this()]() {
            send_packets();
        });
        return;
    }

    std::lock_guard lock(ngtcp2_mutex_);
    if (!conn_) return;
    uint8_t buf[1500];
    ngtcp2_tstamp ts = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
    
    // Limit to 32 packets per turn to keep event loop responsive
    for (int i = 0; i < 32; ++i) {
        ngtcp2_pkt_info pi;
        ngtcp2_ssize res = ngtcp2_conn_write_pkt(conn_, &path_, &pi, buf, sizeof(buf), ts);
        if (res <= 0) {
            break;
        }
        std::error_code ec;
        if (is_server_) {
            socket_.send_to(asio::buffer(buf, (size_t)res), remote_endpoint_, 0, ec);
        } else {
            socket_.send(asio::buffer(buf, (size_t)res), 0, ec);
        }
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                Logger::Error("[QUIC] send_to error: " + ec.message());
            }
            break;
        }
    }
    
    // Also try to send queued stream data
    do_write();

    schedule_timer();
    check_closed();
}

std::shared_ptr<QuicStream> QuicSession::open_stream() {
    std::lock_guard lock(ngtcp2_mutex_);
    if (!conn_) return nullptr;
    int64_t stream_id;
    if (ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr) != 0) return nullptr;
    auto stream = std::make_shared<QuicStream>(stream_id, shared_from_this());
    streams_[stream_id] = stream;
    Logger::Debug("[QUIC] Local stream opened: " + std::to_string(stream_id));
    return stream;
}

void QuicSession::close_session() {
    std::lock_guard lock(ngtcp2_mutex_);
    if (!conn_ || closed_notified_) return;
    
    uint8_t buf[1500];
    ngtcp2_pkt_info pi;
    ngtcp2_ccerr ccerr;
    ngtcp2_ccerr_default(&ccerr);
    
    ngtcp2_tstamp ts = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
    ngtcp2_ssize res = ngtcp2_conn_write_connection_close(conn_, &path_, &pi, buf, sizeof(buf), &ccerr, ts);
    if (res > 0) {
        std::error_code ec;
        if (is_server_) {
            socket_.send_to(asio::buffer(buf, (size_t)res), remote_endpoint_, 0, ec);
        } else {
            socket_.send(asio::buffer(buf, (size_t)res), 0, ec);
        }
    }
    
    // Explicitly trigger closure notification
    closed_notified_ = true;
    auto streams_to_close = std::move(streams_);
    streams_.clear();
    for (auto& pair : streams_to_close) {
        pair.second->handle_close();
    }
    
    if (on_closed_cb_) {
        auto cb = on_closed_cb_;
        asio::post(strand_, [this, cb, self = shared_from_this()]() {
            cb(self);
        });
    }
}

void QuicSession::close_stream(int64_t stream_id) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), stream_id]() {
            close_stream(stream_id);
        });
        return;
    }
    {
        std::lock_guard lock(ngtcp2_mutex_);
        if (!conn_) return;
        ngtcp2_conn_shutdown_stream(conn_, 0, stream_id, 0);
    }
    send_packets();
}

void QuicSession::write_stream(int64_t stream_id, const uint8_t* data, size_t len, std::function<void(std::error_code, std::size_t)> handler) {
    std::vector<asio::const_buffer> buffers;
    if (len > 0) {
        auto pooled_data = buffer_pool_->Get(len);
        std::memcpy(pooled_data.get(), data, len);
        buffers.push_back(asio::buffer(pooled_data.get(), len));
        write_stream(stream_id, buffers, [handler, pooled_data](std::error_code ec, std::size_t n) {
            handler(ec, n);
        });
    } else {
        write_stream(stream_id, buffers, std::move(handler));
    }
}

void QuicSession::write_stream(int64_t stream_id, const std::vector<asio::const_buffer>& buffers, std::function<void(std::error_code, std::size_t)> handler) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), stream_id, buffers, handler]() {
            write_stream(stream_id, buffers, handler);
        });
        return;
    }

    if (!conn_) {
        asio::post(strand_, [handler]() { handler(asio::error::connection_reset, 0); });
        return;
    }

    std::lock_guard lock(ngtcp2_mutex_);
    auto pw = std::make_shared<PendingWrite>();
    pw->stream_id = stream_id;
    pw->bodies = buffers;
    pw->handler = std::move(handler);

    write_queue_.push_back(pw);
    
    do_write();
}

void QuicSession::do_write() {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this()]() {
            do_write();
        });
        return;
    }

    std::lock_guard lock(ngtcp2_mutex_);
    if (!conn_ || closed_notified_) return;
    if (is_writing_) return;
    is_writing_ = true;

    auto self = shared_from_this();
    // Limit to 16 packets per turn to yield to other tasks
    int packets_sent = 0;
    while (!write_queue_.empty() && packets_sent < 16) {
        auto pw = write_queue_.front();
        
        // Prepare ngtcp2_vec for writev
        std::vector<ngtcp2_vec> v;
        size_t skip = pw->consumed;
        size_t total_to_write = 0;
        for (const auto& b : pw->bodies) {
            if (skip >= b.size()) {
                skip -= b.size();
                continue;
            }
            v.push_back({(uint8_t*)b.data() + skip, b.size() - skip});
            total_to_write += b.size() - skip;
            skip = 0;
        }

        if (v.empty() && !pw->bodies.empty()) {
            // All data already consumed? This shouldn't happen if write_queue management is correct.
            write_queue_.pop_front();
            continue;
        }

        ngtcp2_ssize consumed_datalen = 0;
        ngtcp2_pkt_info pi;
        uint8_t buf[1500];
        
        ngtcp2_tstamp ts = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
        ngtcp2_ssize res = ngtcp2_conn_writev_stream(conn_, &path_, &pi, buf, sizeof(buf), &consumed_datalen, 0, pw->stream_id, v.data(), v.size(), ts);
        
        if (res > 0) {
            packets_sent++;
            std::error_code ec;
            if (is_server_) {
                socket_.send_to(asio::buffer(buf, (size_t)res), remote_endpoint_, 0, ec);
            } else {
                socket_.send(asio::buffer(buf, (size_t)res), 0, ec);
            }
            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    Logger::Error("[QUIC] do_write send_to error: " + ec.message());
                }
                break;
            }
            if (consumed_datalen > 0) {
                pw->consumed += consumed_datalen;
                size_t total_orig = 0;
                for (const auto& b : pw->bodies) total_orig += b.size();

                if (pw->consumed >= total_orig) {
                    if (pw->handler) {
                        auto h = std::move(pw->handler);
                        asio::post(strand_, [h, total_orig]() { h(std::error_code(), total_orig); });
                    }
                    write_queue_.pop_front();
                }
            } else {
                break;
            }
        } else {
            break;
        }
    }
    is_writing_ = false;

    if (!write_queue_.empty() && packets_sent == 16) {
        asio::post(strand_, [this, self]() { do_write(); });
    }
}

int QuicSession::on_stream_data(int64_t stream_id, const uint8_t* data, size_t len) {
    std::shared_ptr<QuicStream> stream;
    {
        std::lock_guard lock(ngtcp2_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            stream = it->second;
        }
    }

    if (!stream) {
        // New peer-initiated stream
        stream = std::make_shared<QuicStream>(stream_id, shared_from_this());
        {
            std::lock_guard lock(ngtcp2_mutex_);
            streams_[stream_id] = stream;
        }
        Logger::Debug("[QUIC] Peer stream started: " + std::to_string(stream_id));
        if (on_new_stream_cb_) {
            auto cb = on_new_stream_cb_;
            asio::post(strand_, [cb, stream]() { cb(stream); });
        }
    }
    
    if (stream) {
        auto pooled_data = buffer_pool_->Get(len);
        std::memcpy(pooled_data.get(), data, len);
        stream->handle_data(pooled_data, len);
    }
    return 0;
}

int QuicSession::on_stream_close(int64_t stream_id) {
    std::shared_ptr<QuicStream> stream;
    {
        std::lock_guard lock(ngtcp2_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            stream = it->second;
            streams_.erase(it);
        }
    }
    if (stream) {
        stream->handle_close();
    }
    return 0;
}

void QuicSession::schedule_timer() {
    if (!conn_) return;
    ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(conn_);
    ngtcp2_tstamp now = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
    std::weak_ptr<QuicSession> weak_self = shared_from_this();
    if (expiry <= now) {
        asio::post(strand_, [this, weak_self]() {
            auto self = weak_self.lock();
            if (!self) return;
            std::lock_guard lock(ngtcp2_mutex_);
            if (!conn_) return;
            ngtcp2_conn_handle_expiry(conn_, ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count()));
            send_packets();
            check_closed();
        });
    } else if (expiry != UINT64_MAX) {
        timer_.expires_after(std::chrono::nanoseconds(expiry - now));
        timer_.async_wait(asio::bind_executor(strand_, [this, weak_self](std::error_code ec) {
            auto self = weak_self.lock();
            if (self && !ec) {
                std::lock_guard lock(ngtcp2_mutex_);
                if (conn_) {
                    ngtcp2_conn_handle_expiry(conn_, ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count()));
                    send_packets();
                    check_closed();
                }
            }
        }));
    }
}

void QuicSession::check_closed() {
    std::lock_guard lock(ngtcp2_mutex_);
    if (!conn_ || closed_notified_) return;
    if (ngtcp2_conn_in_closing_period(conn_) || ngtcp2_conn_in_draining_period(conn_)) {
        closed_notified_ = true;

        std::unordered_map<int64_t, std::shared_ptr<QuicStream>> streams_to_close;
        streams_to_close = std::move(streams_);
        streams_.clear();

        // Notify all streams that the session is closed
        for (auto& pair : streams_to_close) {
            pair.second->handle_close();
        }

        if (on_closed_cb_) {
            auto cb = on_closed_cb_;
            asio::post(strand_, [this, cb, self = shared_from_this()]() {
                cb(self);
            });
        }
    }
}

} // namespace quic
} // namespace common
} // namespace cfrp
