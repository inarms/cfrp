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

#include "client.h"
#include "common/quic_ngtcp2.h"
#include "common/utils.h"
#include "common/websocket_stream.h"
#include <algorithm>
#include <iostream>
#include <zstd.h>
#include <filesystem>
#include <fstream>
#include <toml++/toml.h>

namespace fs = std::filesystem;

namespace cfrp {
namespace client {

// --- UdpBridge ---
UdpBridge::UdpBridge(asio::io_context& io_context, std::shared_ptr<common::AsyncStream> stream, udp::endpoint local_endpoint, bool use_compression, int compression_level, std::shared_ptr<common::RateLimiter> rate_limiter, std::shared_ptr<common::BufferPool> buffer_pool)
    : stream_(std::move(stream)), rate_limiter_(std::move(rate_limiter)), buffer_pool_(std::move(buffer_pool)), socket_(io_context, udp::endpoint(udp::v4(), 0)), local_endpoint_(local_endpoint), use_compression_(use_compression), compression_level_(compression_level) {
    if (!buffer_pool_) buffer_pool_ = common::BufferPool::CreateDefault();
    read_buf_.resize(65535);
}

void UdpBridge::Start() {
    DoReadFromStream();
    DoReadFromLocal();
}

void UdpBridge::DoReadFromStream() {
    auto self(shared_from_this());
    stream_->async_read(asio::buffer(&packet_len_, sizeof(packet_len_)),
        [this, self](std::error_code ec, std::size_t) {
            if (!ec) {
                uint16_t header = asio::detail::socket_ops::network_to_host_short(packet_len_);
                uint16_t len = header & 0x7FFF;
                bool compressed = (header & 0x8000) != 0;

                if (len > read_buf_.size()) {
                    stream_->close();
                    return;
                }
                stream_->async_read(asio::buffer(read_buf_.data(), len),
                    [this, self, len, compressed](std::error_code ec, std::size_t) {
                        if (!ec) {
                            std::shared_ptr<uint8_t[]> final_buf;
                            size_t final_len = 0;

                            if (compressed) {
                                unsigned long long const decodedSize = ZSTD_getFrameContentSize(read_buf_.data(), len);
                                if (decodedSize == ZSTD_CONTENTSIZE_ERROR ||
                                    decodedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
                                    decodedSize > 65535) {
                                    stream_->close();
                                    return;
                                }
                                
                                auto& decompressed_buf = dctx_.get_decompress_buf(decodedSize);
                                size_t const dSize = dctx_.decompress(decompressed_buf.data(), decodedSize, read_buf_.data(), len);
                                if (ZSTD_isError(dSize)) {
                                    stream_->close();
                                    return;
                                }
                                final_buf = buffer_pool_->Get(dSize);
                                std::memcpy(final_buf.get(), decompressed_buf.data(), dSize);
                                final_len = dSize;
                            } else {
                                final_buf = buffer_pool_->Get(len);
                                std::memcpy(final_buf.get(), read_buf_.data(), len);
                                final_len = len;
                            }

                            auto send_op = [this, self, final_buf, final_len]() {
                                socket_.async_send_to(asio::buffer(final_buf.get(), final_len), local_endpoint_,
                                    [this, self, final_buf](std::error_code ec, std::size_t) {
                                        if (!ec) {
                                            DoReadFromStream();
                                        }
                                    });
                            };

                            if (rate_limiter_) {
                                rate_limiter_->async_wait(final_len, std::move(send_op));
                            } else {
                                send_op();
                            }
                        } else {
                            stream_->close();
                        }
                    });
            } else {
                stream_->close();
            }
        });
}

void UdpBridge::DoReadFromLocal() {
    auto self(shared_from_this());
    auto buffer = buffer_pool_->Get(65535);
    socket_.async_receive_from(asio::buffer(buffer.get(), 65535), local_endpoint_,
        [this, self, buffer](std::error_code ec, std::size_t length) {
            if (!ec) {
                std::shared_ptr<uint8_t[]> buf;
                
                uint16_t header;
                const void* write_data = buffer.get();
                size_t write_len = length;

                if (use_compression_) {
                    size_t const cSizeBound = ZSTD_compressBound(length);
                    auto& compressed_buf = cctx_.get_compress_buf(cSizeBound);
                    size_t const cSize = cctx_.compress(compressed_buf.data(), cSizeBound, buffer.get(), length, compression_level_);
                    if (!ZSTD_isError(cSize) && cSize < length) {
                        header = static_cast<uint16_t>(cSize) | 0x8000;
                        write_data = compressed_buf.data();
                        write_len = cSize;
                    } else {
                        header = static_cast<uint16_t>(length);
                    }
                } else {
                    header = static_cast<uint16_t>(length);
                }

                uint16_t n_header = asio::detail::socket_ops::host_to_network_short(header);
                size_t total_len = sizeof(n_header) + write_len;
                buf = buffer_pool_->Get(total_len);
                std::memcpy(buf.get(), &n_header, sizeof(n_header));
                std::memcpy(buf.get() + sizeof(n_header), write_data, write_len);

                auto write_op = [this, self, buf, total_len]() {
                    stream_->async_write(asio::buffer(buf.get(), total_len), [this, self, buf](std::error_code ec, std::size_t) {
                        if (!ec) {
                            DoReadFromLocal();
                        } else {
                            stream_->close();
                        }
                    });
                };

                if (rate_limiter_) {
                    rate_limiter_->async_wait(total_len, std::move(write_op));
                } else {
                    write_op();
                }
            } else {
                stream_->close();
            }
        });
}

// --- Client ---
Client::Client(asio::io_context& io_context, const std::string& server_addr, uint16_t server_port, const std::string& token, const std::string& name, const SslConfig& ssl_config, bool compression, int compression_level, const std::string& conf_d_path, const std::string& protocol, std::shared_ptr<common::BufferPool> buffer_pool)
    : io_context_(io_context),
      strand_(asio::make_strand(io_context)),
      server_addr_(server_addr),
      server_port_(server_port),
      token_(token),
      name_(name),
      protocol_(protocol),
      ssl_config_(ssl_config),
      compression_(compression),
      compression_level_(compression_level),
      buffer_pool_(buffer_pool),
      conf_d_path_(conf_d_path),
      conf_timer_(io_context_),
      endpoint_(tcp::v4(), server_port),
      udp_endpoint_(udp::v4(), server_port),
    udp_socket_(io_context_),
      reconnect_timer_(io_context_),
      handshake_timer_(io_context_) {
    
    if (!buffer_pool_) buffer_pool_ = common::BufferPool::CreateDefault();

    current_protocol_ = protocol_;
    if (current_protocol_ == "auto") {
        current_protocol_ = "quic";
    }

    if (ssl_config_.enable) {
        ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv13);
        ssl_ctx_->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3);
        
        if (ssl_config_.verify_peer) {
            ssl_ctx_->set_verify_mode(asio::ssl::verify_peer);
            if (!ssl_config_.ca_file.empty()) {
                ssl_ctx_->load_verify_file(ssl_config_.ca_file);
            } else {
                ssl_ctx_->set_default_verify_paths();
            }

            if (ssl_config_.verify_host) {
                ssl_ctx_->set_verify_callback(asio::ssl::host_name_verification(server_addr));
            }
        } else {
            ssl_ctx_->set_verify_mode(asio::ssl::verify_none);
        }
    }
    
    std::cout << "Client initialized (" << protocol_ << " Mux Enabled). Target: " << server_addr << ":" << server_port << std::endl;
}

void Client::Run() {
    DoConnect();
}

void Client::Stop() {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this()]() { Stop(); });
        return;
    }
    stopping_ = true;
    HandleDisconnect("Client stopping...");
}

void Client::AddProxy(const ProxyConfig& proxy) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), proxy]() { AddProxy(proxy); });
        return;
    }
    proxies_.push_back(proxy);
}

void Client::DoConnect() {
    connection_id_++;
    int conn_id = connection_id_;

    auto resolver = std::make_shared<tcp::resolver>(io_context_);
    resolver->async_resolve(server_addr_, std::to_string(server_port_),
        asio::bind_executor(strand_, [this, conn_id, resolver](std::error_code ec, tcp::resolver::results_type results) {
            if (conn_id != connection_id_) return;
            if (ec) {
                HandleDisconnect("DNS resolution failed for " + server_addr_ + ": " + ec.message());
                return;
            }

            endpoint_ = *results.begin();

            if (current_protocol_ == "quic") {
                quic_endpoints_.clear();
                for (const auto& result : results) {
                    auto tcp_ep = result.endpoint();
                    udp::endpoint candidate(tcp_ep.address(), tcp_ep.port());
                    auto dup = std::find(quic_endpoints_.begin(), quic_endpoints_.end(), candidate);
                    if (dup == quic_endpoints_.end()) {
                        quic_endpoints_.push_back(candidate);
                    }
                }

                quic_endpoint_index_ = 0;
                if (!TryNextQuicEndpoint(conn_id, "initial resolve")) {
                    HandleDisconnect("No usable QUIC endpoint from DNS results");
                }
                return;
            }

            common::Logger::Info("Connecting to server " + server_addr_ + " (" + endpoint_.address().to_string() + ":" + std::to_string(server_port_) + ") (TCP)...");
            
            tcp::socket socket(io_context_);
            auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
            
            asio::async_connect(*socket_ptr, results,
                asio::bind_executor(strand_, [this, socket_ptr, conn_id](std::error_code ec, tcp::endpoint const& connected_endpoint) {
                    if (conn_id != connection_id_) return;
                    if (!ec) {
                        endpoint_ = connected_endpoint;
                        std::error_code local_ec;
                        auto local_endpoint = socket_ptr->local_endpoint(local_ec);
                        if (!local_ec) {
                            common::Logger::Info("Client TCP local endpoint: " + local_endpoint.address().to_string() + ":" + std::to_string(local_endpoint.port()) +
                                                " -> " + connected_endpoint.address().to_string() + ":" + std::to_string(connected_endpoint.port()));
                        }
                        common::SetTcpKeepalive(*socket_ptr);
                        std::shared_ptr<common::AsyncStream> stream;
                        if (ssl_config_.enable) {
                            auto ssl_stream = std::make_shared<common::SslStream>(std::move(*socket_ptr), *ssl_ctx_);
                            
                            // Set SNI for wolfSSL
                            wolfSSL_UseSNI(static_cast<WOLFSSL*>(ssl_stream->get_native_handle()), 
                                           WOLFSSL_SNI_HOST_NAME, server_addr_.c_str(), 
                                           static_cast<word16>(server_addr_.size()));

                            stream = ssl_stream;
                            common::Logger::Info("SSL Peer Verification: " + std::string(ssl_config_.verify_peer ? "ENABLED" : "DISABLED (Insecure)"));
                            if (ssl_config_.verify_peer) {
                                common::Logger::Info("SSL CA File: " + (ssl_config_.ca_file.empty() ? "[System Default]" : ssl_config_.ca_file));
                                common::Logger::Info("SSL Host Verification: " + std::string(ssl_config_.verify_host ? "ENABLED (Target: " + server_addr_ + ")" : "DISABLED"));
                            }
                        } else {
                            stream = std::make_shared<common::TcpStream>(std::move(*socket_ptr));
                        }

                        if (current_protocol_ == "websocket") {
                            stream = std::make_shared<common::WebsocketStream>(stream, true, true, buffer_pool_);
                        }

                        stream->async_handshake(asio::ssl::stream_base::client, asio::bind_executor(strand_, [this, stream, conn_id](std::error_code ec) {
                            if (conn_id != connection_id_) return;
                            if (ssl_config_.enable) {
                                if (!ec) {
                                    common::Logger::Info("SSL Handshake/Verification: SUCCESS");
                                } else {
                                    common::Logger::Error("SSL Handshake/Verification: FAILED: " + ec.message());
                                }
                            }
                            OnConnect(ec, stream);
                        }));
                    } else {
                        HandleDisconnect("Connect failed: " + ec.message());
                    }
                }));
        }));
}

bool Client::RebindUdpSocketForEndpoint(const udp::endpoint& endpoint) {
    std::error_code ec;
    if (udp_socket_.is_open()) {
        udp_socket_.close(ec);
    }

    udp_socket_.open(endpoint.protocol(), ec);
    if (ec) {
        common::Logger::Error("Failed to open UDP socket for QUIC endpoint " + endpoint.address().to_string() + ":" + std::to_string(endpoint.port()) + ": " + ec.message());
        return false;
    }

    udp::endpoint bind_ep(endpoint.protocol(), 0);
    udp_socket_.bind(bind_ep, ec);
    if (ec) {
        common::Logger::Error("Failed to bind UDP socket for QUIC endpoint " + endpoint.address().to_string() + ":" + std::to_string(endpoint.port()) + ": " + ec.message());
        std::error_code close_ec;
        udp_socket_.close(close_ec);
        return false;
    }

    udp_socket_.connect(endpoint, ec);
    if (ec) {
        common::Logger::Error("Failed to connect UDP socket for QUIC endpoint " + endpoint.address().to_string() + ":" + std::to_string(endpoint.port()) + ": " + ec.message());
        std::error_code close_ec;
        udp_socket_.close(close_ec);
        return false;
    }

    return true;
}

bool Client::TryNextQuicEndpoint(int conn_id, const char* reason) {
    if (conn_id != connection_id_) return false;

    handshake_timer_.cancel();
    if (quic_session_) {
        quic_session_->close_session();
        quic_session_.reset();
    }

    while (quic_endpoint_index_ < quic_endpoints_.size()) {
        udp_endpoint_ = quic_endpoints_[quic_endpoint_index_++];
        if (!RebindUdpSocketForEndpoint(udp_endpoint_)) {
            continue;
        }

        common::Logger::Info("Trying QUIC endpoint " + udp_endpoint_.address().to_string() + ":" + std::to_string(udp_endpoint_.port()) + " (" + std::string(reason) + ")");
        DoQuicConnect(conn_id);
        return true;
    }

    return false;
}

void Client::DoQuicConnect(int conn_id) {
    common::Logger::Info("Connecting to server via QUIC " + udp_endpoint_.address().to_string() + ":" + std::to_string(udp_endpoint_.port()) + "...");
    quic_session_ = std::make_shared<common::quic::QuicSession>(udp_socket_, udp_endpoint_, false, buffer_pool_);

    if (!quic_ssl_ctx_) {
        quic_ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv13);
        quic_ssl_ctx_->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3);

        if (ssl_config_.verify_peer) {
            common::Logger::Info("QUIC Peer Verification: ENABLED");
            common::Logger::Info("QUIC CA File: " + (ssl_config_.ca_file.empty() ? "[System Default]" : ssl_config_.ca_file));
            quic_ssl_ctx_->set_verify_mode(asio::ssl::verify_peer);
            if (!ssl_config_.ca_file.empty()) {
                quic_ssl_ctx_->load_verify_file(ssl_config_.ca_file);
            } else {
                quic_ssl_ctx_->set_default_verify_paths();
            }
        } else {
            common::Logger::Info("QUIC Peer Verification: DISABLED (Insecure)");
            quic_ssl_ctx_->set_verify_mode(asio::ssl::verify_none);
        }
    }
    quic_session_->init(quic_ssl_ctx_->native_handle());

    if (ssl_config_.enable && ssl_config_.verify_peer && ssl_config_.verify_host) {
        common::Logger::Info("QUIC Host Verification: ENABLED (Target: " + server_addr_ + ")");
        auto ssl = quic_session_->get_ssl();
        wolfSSL_check_domain_name(ssl, server_addr_.c_str());
        wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, server_addr_.c_str(), static_cast<word16>(server_addr_.size()));
    } else if (ssl_config_.enable) {
        common::Logger::Info("QUIC Host Verification: DISABLED");
        // Still use SNI even if host verification is disabled, for server SNI routing
        auto ssl = quic_session_->get_ssl();
        wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, server_addr_.c_str(), static_cast<word16>(server_addr_.size()));
    }

    handshake_timer_.expires_after(std::chrono::seconds(5));
    handshake_timer_.async_wait([this, conn_id](std::error_code ec) {
        if (conn_id != connection_id_) return;
        if (ec || current_protocol_ != "quic") return;

        if (protocol_ == "auto") {
            if (TryNextQuicEndpoint(conn_id, "handshake timeout")) {
                return;
            }
            common::Logger::Info("QUIC handshake timed out, failing over to TCP...");
            HandleDisconnect("QUIC_TIMEOUT");
            return;
        }

        common::Logger::Error("QUIC handshake timed out");
        HandleDisconnect("QUIC handshake timed out");
    });

    quic_session_->set_on_connected([this, conn_id](std::shared_ptr<common::quic::QuicSession> session) {
        if (conn_id != connection_id_) return;
        handshake_timer_.cancel();
        common::Logger::Info("QUIC handshake completed. Opening control stream...");
        auto stream = session->open_stream();
        if (stream) {
            OnConnect(std::error_code(), stream);
        } else {
            HandleDisconnect("Failed to open QUIC stream after handshake");
        }
    });

    quic_session_->set_on_closed([this, conn_id](std::shared_ptr<common::quic::QuicSession> session) {
        if (conn_id != connection_id_) return;
        handshake_timer_.cancel();
        if (TryNextQuicEndpoint(conn_id, "session closed")) {
            return;
        }
        HandleDisconnect("QUIC session closed by peer");
    });

    quic_session_->send_packets(); // Start handshake
    DoUdpRead();
}
void Client::DoUdpRead() {
    auto endpoint = std::make_shared<udp::endpoint>();
    auto buffer = buffer_pool_->Get(65535);
    udp_socket_.async_receive_from(asio::buffer(buffer.get(), 65535), *endpoint,
        [this, endpoint, buffer](std::error_code ec, std::size_t length) {
            if (!ec) {
                if (quic_session_) {
                    auto s = quic_session_;
                    asio::post(s->strand(), [s, buffer, length]() {
                        s->handle_packet(buffer.get(), length);
                    });
                }
                DoUdpRead();
                return;
            }

            if (ec != asio::error::operation_aborted) {
                common::Logger::Error("Client UDP receive error on QUIC socket: " + ec.message());
                if (ec == asio::error::connection_refused || ec == asio::error::connection_reset) {
                    HandleDisconnect("QUIC UDP connection refused/reset");
                    return;
                }
                if (udp_socket_.is_open()) {
                    DoUdpRead();
                }
            }
        });
}

void Client::OnConnect(const std::error_code& ec, std::shared_ptr<common::AsyncStream> underlying_stream) {
    handshake_timer_.cancel();
    if (!ec) {
        common::Logger::Info("Connected to server via " + underlying_stream->protocol_name() + ". Initializing MuxSession...");
        reconnect_delay_sec_ = 0;
        
        mux_session_ = std::make_shared<common::mux::Session>(underlying_stream, false, buffer_pool_);

        mux_session_->start([](std::shared_ptr<common::mux::MuxStream>) {
            // Client doesn't expect server to open streams in this model
        });
        
        common::Logger::Info("MuxSession initialized. Opening control stream...");
        control_stream_ = mux_session_->open_stream();
        common::Logger::Info("Control stream opened. Sending login request...");
        DoLogin();
        DoReadHeader(connection_id_);
    } else {
        common::Logger::Error("SSL Handshake/Connect failed: " + ec.message() + " (Category: " + ec.category().name() + " Code: " + std::to_string(ec.value()) + ")");
#ifdef ASIO_USE_WOLFSSL
        char errBuf[160];
        unsigned long err;
        while ((err = wolfSSL_ERR_get_error()) != 0) {
            common::Logger::Error("WolfSSL Error: " + std::to_string(err) + " - " + wolfSSL_ERR_error_string(err, errBuf));
        }
#endif
        HandleDisconnect("Handshake/Connect failed");
    }
}

void Client::HandleDisconnect(const std::string& reason) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), reason]() { HandleDisconnect(reason); });
        return;
    }

    if (reason != "QUIC_TIMEOUT" && !reason.empty()) {
        common::Logger::Info(reason);
    }

    connection_id_++;

    if (mux_session_) {
        mux_session_->stop();
        mux_session_.reset();
    }
    if (quic_session_) {
        quic_session_->close_session();
        quic_session_.reset();
    }
    std::error_code udp_close_ec;
    udp_socket_.close(udp_close_ec);
    if (control_stream_) {
        control_stream_.reset();
    }

    if (stopping_) return;

    bool immediate_failover = false;
    if (protocol_ == "auto") {
        if (current_protocol_ == "quic") {
            common::Logger::Info("Switching to TCP failover...");
            current_protocol_ = "tcp";
            immediate_failover = true;
        } else if (current_protocol_ == "tcp") {
            common::Logger::Info("Switching to WebSocket failover...");
            current_protocol_ = "websocket";
            immediate_failover = true;
        } else {
            // Already on WebSocket, try QUIC again for the next reconnection cycle
            current_protocol_ = "quic";
        }
    }

    if (immediate_failover) {
        asio::post(strand_, [this]() {
            if (!stopping_) {
                DoConnect();
            }
        });
        return;
    }

    ScheduleReconnect();
}

void Client::ScheduleReconnect() {
    if (stopping_) return;

    int delay = reconnect_delay_sec_;
    if (reconnect_delay_sec_ < 600) {
        reconnect_delay_sec_ += 10;
    }

    if (delay == 0) {
        common::Logger::Info("Reconnecting immediately...");
    } else {
        common::Logger::Info("Reconnecting in " + std::to_string(delay) + " seconds...");
    }

    reconnect_timer_.expires_after(std::chrono::seconds(delay));
    std::weak_ptr<Client> weak_self = shared_from_this();
    reconnect_timer_.async_wait(asio::bind_executor(strand_, [this, weak_self](std::error_code ec) {
        if (!ec) {
            auto self = weak_self.lock();
            if (self) {
                DoConnect();
            }
        }
    }));
}

void Client::SendMessage(protocol::MessageType type, const std::vector<uint8_t>& body) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), type, body]() { SendMessage(type, body); });
        return;
    }
    if (!control_stream_) return;

    protocol::Message msg{type, body};
    std::vector<uint8_t> encoded = msg.Encode();
    uint32_t final_len = static_cast<uint32_t>(encoded.size());
    std::vector<uint8_t> to_send_body = encoded;

    if (compression_) {
        size_t const cSizeBound = ZSTD_compressBound(encoded.size());
        auto& compressed_buf = cctx_.get_compress_buf(cSizeBound);
        size_t const cSize = cctx_.compress(compressed_buf.data(), cSizeBound, encoded.data(), encoded.size(), compression_level_);
        if (!ZSTD_isError(cSize)) {
            to_send_body.assign(compressed_buf.begin(), compressed_buf.begin() + cSize);
            final_len = static_cast<uint32_t>(cSize) | protocol::COMPRESSION_FLAG;
        }
    }

    uint32_t net_len = asio::detail::socket_ops::host_to_network_long(final_len);
    auto data = std::make_shared<std::vector<uint8_t>>();
    data->resize(sizeof(net_len) + to_send_body.size());
    std::memcpy(data->data(), &net_len, sizeof(net_len));
    std::memcpy(data->data() + sizeof(net_len), to_send_body.data(), to_send_body.size());

    auto self(shared_from_this());
    int conn_id = connection_id_;
    control_stream_->async_write(asio::buffer(*data), asio::bind_executor(strand_, [this, self, data, conn_id](std::error_code ec, std::size_t) {
        if (ec) {
            if (conn_id != connection_id_) return;
            common::Logger::Error("Failed to send message: " + ec.message());
            HandleDisconnect("Failed to send control message");
        }
    }));
}

void Client::DoReadHeader(int conn_id) {
    auto self(shared_from_this());
    control_stream_->async_read(asio::buffer(&header_, sizeof(header_)),
        asio::bind_executor(strand_, [this, self, conn_id](std::error_code ec, std::size_t) {
            if (conn_id != connection_id_) return;
            if (!ec) {
                uint32_t h = asio::detail::socket_ops::network_to_host_long(header_.body_length);
                header_.body_length = h; // Store host-byte order back
                uint32_t length = h & protocol::LENGTH_MASK;
                if (length > protocol::MAX_CONTROL_MESSAGE_SIZE) {
                    HandleDisconnect("Control message too large");
                    return;
                }
                DoReadBody(length, conn_id);
            } else {
                HandleDisconnect("Control stream closed: " + ec.message());
            }
        }));
}
void Client::DoReadBody(uint32_t length, int conn_id) {
    auto self(shared_from_this());
    bool is_compressed = (header_.body_length & protocol::COMPRESSION_FLAG) != 0;
    body_data_.resize(length);
    control_stream_->async_read(asio::buffer(body_data_),
        asio::bind_executor(strand_, [this, self, is_compressed, conn_id](std::error_code ec, std::size_t) {
            if (conn_id != connection_id_) return;
            if (!ec) {
                try {
                    std::vector<uint8_t> data(body_data_.begin(), body_data_.end());
                    if (is_compressed) {
                        unsigned long long const decodedSize = ZSTD_getFrameContentSize(data.data(), data.size());
                        if (decodedSize == ZSTD_CONTENTSIZE_ERROR ||
                            decodedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
                            decodedSize > protocol::MAX_DECOMPRESSED_SIZE) {
                            HandleDisconnect("Invalid compressed control message size");
                            return;
                        }
                        
                        auto& decompressed_buf = dctx_.get_decompress_buf(decodedSize);
                        size_t const dSize = dctx_.decompress(decompressed_buf.data(), decodedSize, data.data(), data.size());
                        if (ZSTD_isError(dSize)) {
                            HandleDisconnect("Failed to decompress control message");
                            return;
                        }
                        data.assign(decompressed_buf.begin(), decompressed_buf.begin() + dSize);
                    }
                    auto msg = protocol::Message::Decode(data);
                    HandleMessage(msg);
                } catch (const std::exception& e) {
                    common::Logger::Error("Failed to decode message: " + std::string(e.what()));
                }
                DoReadHeader(conn_id);
            } else {
                HandleDisconnect("Control stream closed: " + ec.message());
            }
        }));
}

void Client::DoLogin() {
    protocol::LoginMessage msg;
    msg.token = token_;
    msg.name = name_;
    SendMessage(protocol::MessageType::Login, msg.Serialize());
}

void Client::HandleMessage(const protocol::Message& msg) {
    if (msg.type == protocol::MessageType::LoginResp) {
        auto resp = protocol::LoginRespMessage::Deserialize(msg.body);
        if (resp.status == "ok") {
            if (!resp.name.empty()) {
                name_ = resp.name;
            }
            common::Logger::Info("Authenticated successfully as [" + name_ + "]");
            RegisterProxies();
        } else {
            common::Logger::Error("Authentication failed: " + (resp.message.empty() ? "unknown error" : resp.message));
        }
    } else if (msg.type == protocol::MessageType::RegisterProxyResp) {
        auto resp = protocol::RegisterProxyRespMessage::Deserialize(msg.body);
        common::Logger::Info("Proxy registration response: " + resp.status + " for " + resp.name);
    } else if (msg.type == protocol::MessageType::NewUserConn) {
        auto m = protocol::NewUserConnMessage::Deserialize(msg.body);
        HandleNewUserConn(m.proxy_name, m.ticket);
    }
}

void Client::RegisterProxies() {
    // 1. Register static proxies from main config
    for (const auto& proxy : proxies_) {
        RegisterProxy(proxy);
    }
    // 2. Start monitoring dynamic proxies (if configured)
    if (!conf_d_path_.empty()) {
        StartConfMonitor();
    }
}

void Client::RegisterProxy(const ProxyConfig& pc) {
    protocol::RegisterProxyMessage msg;
    msg.name = pc.name;
    msg.type = pc.type;
    msg.remote_port = pc.remote_port;
    msg.custom_domains = pc.custom_domains;
    msg.bandwidth_limit = pc.bandwidth_limit;

    if (pc.bandwidth_limit > 0) {
        if (proxy_rate_limiters_.find(pc.name) == proxy_rate_limiters_.end()) {
            proxy_rate_limiters_[pc.name] = std::make_shared<common::RateLimiter>(io_context_, pc.bandwidth_limit);
        } else {
            proxy_rate_limiters_[pc.name]->set_rate(pc.bandwidth_limit);
        }
    }
    SendMessage(protocol::MessageType::RegisterProxy, msg.Serialize());
}

void Client::UnregisterProxy(const std::string& name) {
    protocol::UnregisterProxyMessage msg;
    msg.name = name;
    SendMessage(protocol::MessageType::UnregisterProxy, msg.Serialize());
}

void Client::StartConfMonitor() {
    PollConfDirectory();
}

void Client::PollConfDirectory() {
    auto self(shared_from_this());
    std::unordered_map<std::string, ProxyConfig> new_proxies;
    
    try {
        if (fs::exists(conf_d_path_)) {
            for (auto const& entry : fs::directory_iterator(conf_d_path_)) {
                if (entry.is_regular_file() && entry.path().extension() == ".toml") {
                    try {
                        auto data = toml::parse_file(entry.path().string());
                        ProxyConfig pc;
                        pc.name = data["name"].value_or("");
                        if (pc.name.empty()) {
                            pc.name = entry.path().stem().string();
                        }
                        pc.type = data["type"].value_or("tcp");
                        pc.local_ip = data["local_ip"].value_or("127.0.0.1");
                        pc.local_port = static_cast<uint16_t>(data["local_port"].value_or(0));
                        pc.remote_port = static_cast<uint16_t>(data["remote_port"].value_or(0));
                        
                        if (auto bw = data["bandwidth_limit"].as_string()) {
                            pc.bandwidth_limit = common::ParseBandwidth(bw->get());
                        } else if (auto bw_int = data["bandwidth_limit"].as_integer()) {
                            pc.bandwidth_limit = bw_int->get();
                        }

                        if (!pc.name.empty()) {
                            new_proxies[pc.name] = pc;
                        }
                    } catch (const std::exception& e) {
                        common::Logger::Error("[Client] Error parsing [" + entry.path().filename().string() + "]: " + e.what());
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        common::Logger::Error("[Client] Error scanning conf.d: " + std::string(e.what()));
    }

    // Diff
    for (auto const& [name, pc] : dynamic_proxies_) {
        if (new_proxies.find(name) == new_proxies.end()) {
            common::Logger::Info("[Client] Removing dynamic proxy [" + name + "]");
            UnregisterProxy(name);
        }
    }

    for (auto const& [name, pc] : new_proxies) {
        auto it = dynamic_proxies_.find(name);
        if (it == dynamic_proxies_.end()) {
            common::Logger::Info("[Client] Adding dynamic proxy [" + name + "]");
            RegisterProxy(pc);
        } else {
            bool changed = (it->second.type != pc.type || 
                            it->second.local_ip != pc.local_ip ||
                            it->second.local_port != pc.local_port ||
                            it->second.remote_port != pc.remote_port);
            if (changed) {
                common::Logger::Info("[Client] Updating dynamic proxy [" + name + "]");
                UnregisterProxy(name);
                RegisterProxy(pc);
            }
        }
    }

    dynamic_proxies_ = new_proxies;

    // Cross-platform polling every 5 seconds
    conf_timer_.expires_after(std::chrono::seconds(5));
    conf_timer_.async_wait([this, self](std::error_code ec) {
        if (!ec) {
            PollConfDirectory();
        }
    });
}

void Client::HandleNewUserConn(const std::string& proxy_name, const std::string& ticket) {
    auto self(shared_from_this());
    auto it = std::find_if(proxies_.begin(), proxies_.end(), [&](const ProxyConfig& pc) {
        return pc.name == proxy_name;
    });

    ProxyConfig pc;
    bool found = false;

    if (it != proxies_.end()) {
        pc = *it;
        found = true;
    } else {
        auto it_dyn = dynamic_proxies_.find(proxy_name);
        if (it_dyn != dynamic_proxies_.end()) {
            pc = it_dyn->second;
            found = true;
        }
    }

    if (!found) {
        common::Logger::Error("Unknown proxy name: " + proxy_name);
        return;
    }

    if (pc.type == "udp") {
        HandleNewUdpUserConn(pc, ticket);
        return;
    }

    auto local_socket = std::make_shared<tcp::socket>(io_context_);
    auto resolver = std::make_shared<tcp::resolver>(io_context_);
    
    resolver->async_resolve(pc.local_ip, std::to_string(pc.local_port),
        [this, self, local_socket, ticket, pc, resolver](std::error_code ec, tcp::resolver::results_type results) {
            if (!ec) {
                asio::async_connect(*local_socket, results,
                    [this, self, local_socket, ticket, pc](std::error_code ec, tcp::endpoint) {
                        if (!ec) {
                            common::SetTcpKeepalive(*local_socket);
                            if (!mux_session_) return;
                            auto work_stream = mux_session_->open_stream();
                            
                            auto ticket_buf = std::make_shared<std::vector<uint8_t>>();
                            ticket_buf->push_back(compression_ ? 0x01 : 0x00);
                            ticket_buf->insert(ticket_buf->end(), ticket.begin(), ticket.end());
                            ticket_buf->resize(65, ' ');
                            
                            work_stream->async_write(asio::buffer(*ticket_buf),
                                [this, self, local_socket, work_stream, ticket_buf, pc](std::error_code ec, std::size_t) {
                                    if (!ec) {
                                        common::Logger::Info("Bridging local service and mux work stream (Compressed: " + std::string(compression_ ? "true" : "false") + ")");
                                        auto user_stream = std::make_shared<common::TcpStream>(std::move(*local_socket));
                                        
                                        std::shared_ptr<common::RateLimiter> rl;
                                        auto it = proxy_rate_limiters_.find(pc.name);
                                        if (it != proxy_rate_limiters_.end()) rl = it->second;

                                        auto bridge = std::make_shared<common::Bridge>(user_stream, work_stream, compression_, compression_level_, rl, buffer_pool_);
                                        bridge->Start();
                                    } else {
                                        common::Logger::Error("Failed to send ticket over mux stream");
                                    }
                                });
                        } else {
                            common::Logger::Error("Failed to connect to local service (" + pc.local_ip + ":" + std::to_string(pc.local_port) + "): " + ec.message());
                        }
                    });
            } else {
                common::Logger::Error("Failed to resolve local service (" + pc.local_ip + "): " + ec.message());
            }
        });
}

void Client::HandleNewUdpUserConn(const ProxyConfig& pc, const std::string& ticket) {
    if (!mux_session_) return;
    auto self(shared_from_this());
    auto work_stream = mux_session_->open_stream();
    
    auto ticket_buf = std::make_shared<std::vector<uint8_t>>();
    ticket_buf->push_back(compression_ ? 0x01 : 0x00);
    ticket_buf->insert(ticket_buf->end(), ticket.begin(), ticket.end());
    ticket_buf->resize(65, ' ');
    
    work_stream->async_write(asio::buffer(*ticket_buf),
        [this, self, work_stream, ticket_buf, pc](std::error_code ec, std::size_t) {
            if (!ec) {
    auto resolver = std::make_shared<udp::resolver>(io_context_);
    resolver->async_resolve(pc.local_ip, std::to_string(pc.local_port),
        [this, self, work_stream, ticket_buf, pc, resolver](std::error_code ec, udp::resolver::results_type results) {
            if (!ec && !results.empty()) {
                common::Logger::Info("Bridging local UDP service and mux work stream (Compressed: " + std::string(compression_ ? "true" : "false") + ")");
                
                std::shared_ptr<common::RateLimiter> rl;
                auto it = proxy_rate_limiters_.find(pc.name);
                if (it != proxy_rate_limiters_.end()) rl = it->second;

                auto bridge = std::make_shared<UdpBridge>(io_context_, work_stream, *results.begin(), compression_, compression_level_, rl);
                bridge->Start();
            } else {
                common::Logger::Error("Failed to resolve local UDP service (" + pc.local_ip + "): " + (ec ? ec.message() : "No results"));
            }
        });
            } else {
                common::Logger::Error("Failed to send ticket over mux stream for UDP");
            }
        });
}

} // namespace client
} // namespace cfrp
