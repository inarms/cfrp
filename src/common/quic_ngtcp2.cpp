#include "common/quic_ngtcp2.h"
#include <iostream>
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
    close();
}

void QuicStream::async_read_some(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_reads_.push_back({buffer, std::move(handler), false});
    do_read();
}

void QuicStream::async_write(asio::const_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    auto session = session_.lock();
    if (!session) {
        asio::post(get_executor(), [handler]() { handler(asio::error::connection_reset, 0); });
        return;
    }

    session->write_stream(stream_id_, static_cast<const uint8_t*>(buffer.data()), buffer.size());
    size_t len = buffer.size();
    asio::post(get_executor(), [handler, len]() { handler(std::error_code(), len); });
}

void QuicStream::async_read(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_reads_.push_back({buffer, std::move(handler), true});
    do_read();
}

void QuicStream::async_handshake(ssl::stream_base::handshake_type, std::function<void(std::error_code)> handler) {
    asio::post(get_executor(), [handler]() { handler(std::error_code()); });
}

void QuicStream::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    closed_ = true;
    auto session = session_.lock();
    if (session) session->close_stream(stream_id_);
}

asio::any_io_executor QuicStream::get_executor() {
    auto session = session_.lock();
    if (session) return session->get_executor();
    throw asio::system_error(asio::error::operation_aborted);
}

std::string QuicStream::remote_endpoint_string() {
    auto session = session_.lock();
    return session ? session->remote_endpoint_string() : "unknown";
}

std::string QuicStream::protocol_name() { return "QUIC"; }

void QuicStream::handle_data(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    read_buf_.insert(read_buf_.end(), data, data + len);
    do_read();
}

void QuicStream::handle_close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    do_read();
}

void QuicStream::do_read() {
    while (!pending_reads_.empty()) {
        auto& pr = pending_reads_.front();
        if (read_buf_.empty()) {
            if (closed_) {
                auto h = std::move(pr.handler);
                pending_reads_.pop_front();
                asio::post(get_executor(), [h]() { h(asio::error::eof, 0); });
                continue;
            }
            break;
        }
        size_t to_copy = std::min(pr.buffer.size(), read_buf_.size());
        if (pr.read_all && to_copy < pr.buffer.size()) break;
        std::copy(read_buf_.begin(), read_buf_.begin() + to_copy, static_cast<uint8_t*>(pr.buffer.data()));
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + to_copy);
        auto h = std::move(pr.handler);
        pending_reads_.pop_front();
        asio::post(get_executor(), [h, to_copy]() { h(std::error_code(), to_copy); });
    }
}

// --- QuicSession Implementation ---

namespace {
    ngtcp2_conn* get_conn(ngtcp2_crypto_conn_ref *conn_ref) {
        return static_cast<QuicSession*>(conn_ref->user_data)->conn();
    }

    int recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id, uint64_t offset, const uint8_t *data, size_t datalen, void *user_data, void *stream_user_data) {
        static_cast<QuicSession*>(user_data)->on_stream_data(stream_id, data, datalen);
        return 0;
    }
    int stream_close(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id, uint64_t app_error_code, void *user_data, void *stream_user_data) {
        static_cast<QuicSession*>(user_data)->on_stream_close(stream_id);
        return 0;
    }
    int handshake_completed(ngtcp2_conn *conn, void *user_data) {
        std::cout << "[QUIC] Handshake completed successfully!" << std::endl;
        static_cast<QuicSession*>(user_data)->trigger_connected();
        return 0;
    }
    int get_new_connection_id(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token, size_t cidlen, void *user_data) {
        WC_RNG rng; wc_InitRng(&rng);
        wc_RNG_GenerateBlock(&rng, cid->data, (word32)cidlen);
        cid->datalen = cidlen;
        wc_RNG_GenerateBlock(&rng, token, NGTCP2_STATELESS_RESET_TOKENLEN);
        wc_FreeRng(&rng);
        return 0;
    }
    void rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *rctx) {
        WC_RNG rng; wc_InitRng(&rng);
        wc_RNG_GenerateBlock(&rng, dest, (word32)destlen);
        wc_FreeRng(&rng);
    }
}

QuicSession::QuicSession(asio::ip::udp::socket& socket, asio::ip::udp::endpoint remote_endpoint, bool is_server)
    : socket_(socket), remote_endpoint_(remote_endpoint), is_server_(is_server), timer_(socket.get_executor()) {
    
    auto local_ep = socket_.local_endpoint();
    std::memcpy(&local_addr_, local_ep.data(), local_ep.size());
    std::memcpy(&remote_addr_, remote_endpoint_.data(), remote_endpoint_.size());

    path_.local.addr = (ngtcp2_sockaddr*)&local_addr_;
    path_.local.addrlen = local_ep.size();
    path_.remote.addr = (ngtcp2_sockaddr*)&remote_addr_;
    path_.remote.addrlen = remote_endpoint_.size();
}

QuicSession::~QuicSession() {
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
    
    if (!is_server_) {
        wolfSSL_set_verify(ssl_, WOLFSSL_VERIFY_NONE, NULL);
    }

    wolfSSL_UseALPN(ssl_, (char*)"cfrp", 4, WOLFSSL_ALPN_FAILED_ON_MISMATCH);

    auto conn_ref = new ngtcp2_crypto_conn_ref();
    conn_ref->get_conn = get_conn;
    conn_ref->user_data = this;
    wolfSSL_set_app_data(ssl_, conn_ref);

    ngtcp2_settings settings; ngtcp2_settings_default(&settings);
    settings.initial_ts = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
    
    ngtcp2_transport_params params; ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_stream_data_uni = 256 * 1024;
    params.initial_max_data = 1024 * 1024;
    params.initial_max_streams_bidi = 100;
    params.initial_max_streams_uni = 100;
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;
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
        std::cerr << "[QUIC] Failed to create ngtcp2 connection: " << res << " (" << ngtcp2_strerror(res) << ")" << std::endl;
        return;
    }

    ngtcp2_conn_set_tls_native_handle(conn_, (void*)ssl_);
}

void QuicSession::handle_packet(const uint8_t* data, size_t len) {
    if (!conn_) return;
    ngtcp2_tstamp ts = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
    int res = ngtcp2_conn_read_pkt(conn_, &path_, nullptr, data, len, ts);
    if (res != 0) {
        std::cerr << "[QUIC] Packet read error: " << ngtcp2_strerror(res) << std::endl;
    }
    send_packets();
}

void QuicSession::send_packets() {
    if (!conn_) return;
    uint8_t buf[1500];
    ngtcp2_tstamp ts = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
    for (;;) {
        ngtcp2_pkt_info pi;
        ngtcp2_ssize res = ngtcp2_conn_write_pkt(conn_, &path_, &pi, buf, sizeof(buf), ts);
        if (res <= 0) break;
        socket_.send_to(asio::buffer(buf, (size_t)res), remote_endpoint_);
    }
    schedule_timer();
}

std::shared_ptr<QuicStream> QuicSession::open_stream() {
    if (!conn_) return nullptr;
    int64_t stream_id;
    if (ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr) != 0) return nullptr;
    auto stream = std::make_shared<QuicStream>(stream_id, shared_from_this());
    { std::lock_guard<std::mutex> lock(streams_mutex_); streams_[stream_id] = stream; }
    std::cout << "[QUIC] Local stream opened: " << stream_id << std::endl;
    return stream;
}

void QuicSession::close_stream(int64_t stream_id) {
    if (!conn_) return;
    ngtcp2_conn_shutdown_stream(conn_, stream_id, 0, 0);
    send_packets();
}

void QuicSession::write_stream(int64_t stream_id, const uint8_t* data, size_t len) {
    if (!conn_) return;
    ngtcp2_vec v{(uint8_t*)data, len};
    ngtcp2_ssize consumed_datalen;
    ngtcp2_pkt_info pi;
    uint8_t buf[1500];
    
    ngtcp2_tstamp ts = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
    ngtcp2_ssize res = ngtcp2_conn_writev_stream(conn_, &path_, &pi, buf, sizeof(buf), &consumed_datalen, 0, stream_id, &v, 1, ts);
    
    if (res > 0) {
        socket_.send_to(asio::buffer(buf, (size_t)res), remote_endpoint_);
    }
    send_packets();
}

int QuicSession::on_stream_data(int64_t stream_id, const uint8_t* data, size_t len) {
    std::shared_ptr<QuicStream> stream;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            // New peer-initiated stream
            stream = std::make_shared<QuicStream>(stream_id, shared_from_this());
            streams_[stream_id] = stream;
            std::cout << "[QUIC] Peer stream started: " << stream_id << std::endl;
            if (on_new_stream_cb_) {
                auto cb = on_new_stream_cb_;
                asio::post(get_executor(), [cb, stream]() { cb(stream); });
            }
        } else {
            stream = it->second;
        }
    }
    if (stream) stream->handle_data(data, len);
    return 0;
}

int QuicSession::on_stream_close(int64_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) { it->second->handle_close(); streams_.erase(it); }
    return 0;
}

void QuicSession::schedule_timer() {
    if (!conn_) return;
    ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(conn_);
    ngtcp2_tstamp now = ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count());
    if (expiry <= now) {
        asio::post(get_executor(), [this, self = shared_from_this()]() {
            if (!conn_) return;
            ngtcp2_conn_handle_expiry(conn_, ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count()));
            send_packets();
        });
    } else {
        timer_.expires_after(std::chrono::nanoseconds(expiry - now));
        timer_.async_wait([this, self = shared_from_this()](std::error_code ec) {
            if (!ec && conn_) {
                ngtcp2_conn_handle_expiry(conn_, ngtcp2_tstamp(std::chrono::steady_clock::now().time_since_epoch().count()));
                send_packets();
            }
        });
    }
}

} // namespace quic
} // namespace common
} // namespace cfrp
