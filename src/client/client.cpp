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
#include <iostream>
#include <zstd.h>
#include <filesystem>
#include <fstream>
#include <toml++/toml.h>

namespace fs = std::filesystem;

namespace cfrp {
namespace client {

// --- Bridge ---
Bridge::Bridge(std::shared_ptr<common::AsyncStream> s1, std::shared_ptr<common::AsyncStream> s2, bool use_compression, std::shared_ptr<common::RateLimiter> rate_limiter)
    : s1_(std::move(s1)), s2_(std::move(s2)), rate_limiter_(std::move(rate_limiter)), use_compression_(use_compression) {}

void Bridge::Start() {
    DoRead(1);
    DoRead(2);
}

void Bridge::DoRead(int direction) {
    auto self(shared_from_this());
    if (!use_compression_) {
        auto from = (direction == 1) ? s1_ : s2_;
        auto buf = (direction == 1) ? data1_ : data2_;

        from->async_read_some(asio::buffer(buf, sizeof(data1_)),
            [this, self, direction](std::error_code ec, std::size_t length) {
                if (!ec) {
                    auto to_inner = (direction == 1) ? s2_ : s1_;
                    auto buf_inner = (direction == 1) ? data1_ : data2_;
                    
                    auto write_op = [this, self, direction, to_inner, buf_inner, length]() {
                        to_inner->async_write(asio::buffer(buf_inner, length),
                            [this, self, direction](std::error_code ec, std::size_t) {
                                if (!ec) {
                                    DoRead(direction);
                                } else {
                                    s1_->close(); s2_->close();
                                }
                            });
                    };

                    if (rate_limiter_) {
                        rate_limiter_->async_wait(length, std::move(write_op));
                    } else {
                        write_op();
                    }
                } else {
                    if (ec != asio::error::operation_aborted && ec != asio::error::eof) {
                        std::cerr << "[Bridge] Connection error (" << direction << "): " << ec.message() << std::endl;
                    } else if (ec == asio::error::eof) {
                        std::cout << "[Bridge] Connection closed by peer (" << direction << ")" << std::endl;
                    }
                    s1_->close(); s2_->close();
                }
            });
    } else {
        if (direction == 1) { // Raw -> Work (Compress & Frame)
            s1_->async_read_some(asio::buffer(data1_, sizeof(data1_)),
                [this, self](std::error_code ec, std::size_t length) {
                    if (!ec) {
                        size_t const cSizeBound = ZSTD_compressBound(length);
                        std::vector<uint8_t> compressed(cSizeBound);
                        size_t const cSize = ZSTD_compress(compressed.data(), cSizeBound, data1_, length, 1);
                        
                        uint32_t final_header;
                        const void* write_buf;
                        size_t write_len;

                        if (!ZSTD_isError(cSize) && cSize < length) {
                            final_header = asio::detail::socket_ops::host_to_network_long(static_cast<uint32_t>(cSize) | protocol::COMPRESSION_FLAG);
                            write_buf = compressed.data();
                            write_len = cSize;
                        } else {
                            final_header = asio::detail::socket_ops::host_to_network_long(static_cast<uint32_t>(length));
                            write_buf = data1_;
                            write_len = length;
                        }

                        auto packet = std::make_shared<std::vector<uint8_t>>();
                        packet->resize(sizeof(final_header) + write_len);
                        std::memcpy(packet->data(), &final_header, sizeof(final_header));
                        std::memcpy(packet->data() + sizeof(final_header), write_buf, write_len);

                        auto write_op = [this, self, packet]() {
                            s2_->async_write(asio::buffer(*packet), [this, self, packet](std::error_code ec, std::size_t) {
                                if (!ec) {
                                    DoRead(1);
                                } else {
                                    s1_->close(); s2_->close();
                                }
                            });
                        };

                        if (rate_limiter_) {
                            rate_limiter_->async_wait(length, std::move(write_op));
                        } else {
                            write_op();
                        }
                    } else {
                        s1_->close(); s2_->close();
                    }
                });
        } else { // Work -> Raw (Unframe & Decompress)
            s2_->async_read(asio::buffer(&header2_, sizeof(header2_)),
                [this, self](std::error_code ec, std::size_t) {
                    if (!ec) {
                        uint32_t h2 = asio::detail::socket_ops::network_to_host_long(header2_);
                        uint32_t len = h2 & protocol::LENGTH_MASK;
                        bool compressed = (h2 & protocol::COMPRESSION_FLAG) != 0;
                        if (len > 1024 * 1024) { s1_->close(); s2_->close(); return; }
                        auto body = std::make_shared<std::vector<uint8_t>>(len);
                        s2_->async_read(asio::buffer(*body), [this, self, body, compressed](std::error_code ec, std::size_t) {
                            if (!ec) {
                                if (compressed) {
                                    unsigned long long const decodedSize = ZSTD_getFrameContentSize(body->data(), body->size());
                                    if (decodedSize == ZSTD_CONTENTSIZE_ERROR || decodedSize == ZSTD_CONTENTSIZE_UNKNOWN) { 
                                        s1_->close(); s2_->close(); return; 
                                    }
                                    auto decompressed = std::make_shared<std::vector<uint8_t>>(decodedSize);
                                    size_t const dSize = ZSTD_decompress(decompressed->data(), decodedSize, body->data(), body->size());
                                    if (ZSTD_isError(dSize)) { s1_->close(); s2_->close(); return; }
                                    
                                    auto write_op = [this, self, decompressed]() {
                                        s1_->async_write(asio::buffer(*decompressed), [this, self, decompressed](std::error_code ec, std::size_t) {
                                            if (!ec) DoRead(2); else { s1_->close(); s2_->close(); }
                                        });
                                    };

                                    if (rate_limiter_) {
                                        rate_limiter_->async_wait(decompressed->size(), std::move(write_op));
                                    } else {
                                        write_op();
                                    }
                                } else {
                                    auto write_op = [this, self, body]() {
                                        s1_->async_write(asio::buffer(*body), [this, self, body](std::error_code ec, std::size_t) {
                                            if (!ec) DoRead(2); else { s1_->close(); s2_->close(); }
                                        });
                                    };

                                    if (rate_limiter_) {
                                        rate_limiter_->async_wait(body->size(), std::move(write_op));
                                    } else {
                                        write_op();
                                    }
                                }
                            } else {
                                s1_->close(); s2_->close();
                            }
                        });
                    } else {
                        s1_->close(); s2_->close();
                    }
                });
        }
    }
}

// --- UdpBridge ---
UdpBridge::UdpBridge(asio::io_context& io_context, std::shared_ptr<common::AsyncStream> stream, udp::endpoint local_endpoint, bool use_compression, std::shared_ptr<common::RateLimiter> rate_limiter)
    : stream_(std::move(stream)), rate_limiter_(std::move(rate_limiter)), socket_(io_context, udp::endpoint(udp::v4(), 0)), local_endpoint_(local_endpoint), use_compression_(use_compression) {
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
                            const void* send_buf = read_buf_.data();
                            size_t send_len = len;
                            std::vector<uint8_t> decompressed;

                            if (compressed) {
                                unsigned long long const decodedSize = ZSTD_getFrameContentSize(read_buf_.data(), len);
                                decompressed.resize(decodedSize);
                                ZSTD_decompress(decompressed.data(), decodedSize, read_buf_.data(), len);
                                send_buf = decompressed.data();
                                send_len = decodedSize;
                            }

                            auto send_op = [this, self, send_buf, send_len]() {
                                socket_.async_send_to(asio::buffer(send_buf, send_len), local_endpoint_,
                                    [this, self](std::error_code ec, std::size_t) {
                                        if (!ec) {
                                            DoReadFromStream();
                                        }
                                    });
                            };

                            if (rate_limiter_) {
                                rate_limiter_->async_wait(send_len, std::move(send_op));
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
    socket_.async_receive_from(asio::buffer(local_recv_buf_, sizeof(local_recv_buf_)), local_endpoint_,
        [this, self](std::error_code ec, std::size_t length) {
            if (!ec) {
                auto buf = std::make_shared<std::vector<uint8_t>>();
                
                uint16_t header;
                const void* write_data = local_recv_buf_;
                size_t write_len = length;
                std::vector<uint8_t> compressed;

                if (use_compression_) {
                    size_t const cSizeBound = ZSTD_compressBound(length);
                    compressed.resize(cSizeBound);
                    size_t const cSize = ZSTD_compress(compressed.data(), cSizeBound, local_recv_buf_, length, 1);
                    if (!ZSTD_isError(cSize) && cSize < length) {
                        header = static_cast<uint16_t>(cSize) | 0x8000;
                        write_data = compressed.data();
                        write_len = cSize;
                    } else {
                        header = static_cast<uint16_t>(length);
                    }
                } else {
                    header = static_cast<uint16_t>(length);
                }

                uint16_t n_header = asio::detail::socket_ops::host_to_network_short(header);
                buf->resize(sizeof(n_header) + write_len);
                std::memcpy(buf->data(), &n_header, sizeof(n_header));
                std::memcpy(buf->data() + sizeof(n_header), write_data, write_len);

                auto write_op = [this, self, buf]() {
                    stream_->async_write(asio::buffer(*buf), [this, self, buf](std::error_code ec, std::size_t) {
                        if (!ec) {
                            DoReadFromLocal();
                        } else {
                            stream_->close();
                        }
                    });
                };

                if (rate_limiter_) {
                    rate_limiter_->async_wait(length, std::move(write_op));
                } else {
                    write_op();
                }
            } else {
                stream_->close();
            }
        });
}

// --- Client ---
Client::Client(asio::io_context& io_context, const std::string& server_addr, uint16_t server_port, const std::string& token, const std::string& name, const SslConfig& ssl_config, bool compression, const std::string& conf_d_path, const std::string& protocol)
    : io_context_(io_context),
      server_addr_(server_addr),
      server_port_(server_port),
      token_(token),
      name_(name),
      protocol_(protocol),
      ssl_config_(ssl_config),
      compression_(compression),
      conf_d_path_(conf_d_path),
      conf_timer_(io_context_),
      endpoint_(tcp::v4(), server_port),
      udp_endpoint_(udp::v4(), server_port),
      udp_socket_(io_context_, udp::endpoint(udp::v4(), 0)),
      reconnect_timer_(io_context_),
      handshake_timer_(io_context_) {
    
    current_protocol_ = protocol_;
    if (current_protocol_ == "auto") {
        current_protocol_ = "quic";
    }

    if (ssl_config_.enable) {
        ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv12);
        ssl_ctx_->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3);
        
        if (ssl_config_.verify_peer) {
            ssl_ctx_->set_verify_mode(asio::ssl::verify_peer);
            if (!ssl_config_.ca_file.empty()) {
                ssl_ctx_->load_verify_file(ssl_config_.ca_file);
            } else {
                ssl_ctx_->set_default_verify_paths();
            }
        } else {
            ssl_ctx_->set_verify_mode(asio::ssl::verify_none);
        }
        std::cout << "SSL enabled on client (TLSv1.2)." << std::endl;
    }
    
    std::cout << "Client initialized (" << protocol_ << " Mux Enabled). Target: " << server_addr << ":" << server_port << std::endl;
}

void Client::Run() {
    DoConnect();
}

void Client::Stop() {
    stopping_ = true;
    HandleDisconnect("Client stopping...");
}

void Client::AddProxy(const ProxyConfig& proxy) {
    proxies_.push_back(proxy);
}

void Client::DoConnect() {
    connection_id_++;
    int conn_id = connection_id_;

    auto resolver = std::make_shared<tcp::resolver>(io_context_);
    resolver->async_resolve(server_addr_, std::to_string(server_port_),
        [this, conn_id, resolver](std::error_code ec, tcp::resolver::results_type results) {
            if (conn_id != connection_id_) return;
            if (ec) {
                HandleDisconnect("DNS resolution failed for " + server_addr_ + ": " + ec.message());
                return;
            }

            endpoint_ = *results.begin();
            udp_endpoint_ = udp::endpoint(endpoint_.address(), endpoint_.port());

            if (current_protocol_ == "quic") {
                DoQuicConnect(conn_id);
                return;
            }

            std::cout << "Connecting to server " << server_addr_ << " (" << endpoint_.address().to_string() << ":" << server_port_ << ") (TCP)..." << std::endl;
            
            tcp::socket socket(io_context_);
            auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
            
            asio::async_connect(*socket_ptr, results,
                [this, socket_ptr, conn_id](std::error_code ec, tcp::endpoint const& connected_endpoint) {
                    if (conn_id != connection_id_) return;
                    if (!ec) {
                        endpoint_ = connected_endpoint;
                        common::SetTcpKeepalive(*socket_ptr);
                        std::shared_ptr<common::AsyncStream> stream;
                        if (ssl_config_.enable) {
                            stream = std::make_shared<common::SslStream>(std::move(*socket_ptr), *ssl_ctx_);
                        } else {
                            stream = std::make_shared<common::TcpStream>(std::move(*socket_ptr));
                        }

                        if (current_protocol_ == "websocket") {
                            stream = std::make_shared<common::WebsocketStream>(stream, true);
                        }

                        stream->set_host_name(server_addr_);
                        stream->async_handshake(asio::ssl::stream_base::client, [this, stream, conn_id](std::error_code ec) {                            if (conn_id != connection_id_) return;
                            OnConnect(ec, stream);
                        });
                    } else {
                        HandleDisconnect("Connect failed: " + ec.message());
                    }
                });
        });
}

void Client::DoQuicConnect(int conn_id) {
    std::cout << "Connecting to server via QUIC " << server_addr_ << ":" << server_port_ << "..." << std::endl;
    quic_session_ = std::make_shared<common::quic::QuicSession>(udp_socket_, udp_endpoint_, false);

    if (!ssl_ctx_) {
        ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv13);
    }
    quic_session_->init(ssl_ctx_->native_handle());

    if (protocol_ == "auto") {
        handshake_timer_.expires_after(std::chrono::seconds(5));
        handshake_timer_.async_wait([this, conn_id](std::error_code ec) {
            if (conn_id != connection_id_) return;
            if (!ec && current_protocol_ == "quic") {
                std::cout << "QUIC handshake timed out, failing over to TCP..." << std::endl;
                HandleDisconnect("QUIC_TIMEOUT");
            }
        });
    }

    quic_session_->set_on_connected([this, conn_id](std::shared_ptr<common::quic::QuicSession> session) {
        if (conn_id != connection_id_) return;
        handshake_timer_.cancel();
        std::cout << "QUIC handshake completed. Opening control stream..." << std::endl;
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
        HandleDisconnect("QUIC session closed by peer");
    });

    quic_session_->send_packets(); // Start handshake
    DoUdpRead();
}
void Client::DoUdpRead() {
    auto endpoint = std::make_shared<udp::endpoint>();
    udp_socket_.async_receive_from(asio::buffer(udp_recv_buf_, sizeof(udp_recv_buf_)), *endpoint,
        [this, endpoint](std::error_code ec, std::size_t length) {
            if (!ec) {
                if (quic_session_) {
                    quic_session_->handle_packet(udp_recv_buf_, length);
                }
                DoUdpRead();
            }
        });
}

void Client::OnConnect(const std::error_code& ec, std::shared_ptr<common::AsyncStream> underlying_stream) {
    handshake_timer_.cancel();
    if (!ec) {
        std::cout << "Connected to server via " << underlying_stream->protocol_name() << ". Initializing MuxSession..." << std::endl;
        reconnect_delay_sec_ = 0;
        
        mux_session_ = std::make_shared<common::mux::Session>(underlying_stream, false);
        mux_session_->start([](std::shared_ptr<common::mux::MuxStream>) {
            // Client doesn't expect server to open streams in this model
        });
        
        std::cout << "MuxSession initialized. Opening control stream..." << std::endl;
        control_stream_ = mux_session_->open_stream();
        std::cout << "Control stream opened. Sending login request..." << std::endl;
        DoLogin();
        DoReadHeader(connection_id_);
    } else {
        std::cerr << "SSL Handshake/Connect failed: " << ec.message() << " (Category: " << ec.category().name() << " Code: " << ec.value() << ")" << std::endl;
#ifdef ASIO_USE_WOLFSSL
        char errBuf[160];
        unsigned long err;
        while ((err = wolfSSL_ERR_get_error()) != 0) {
            std::cerr << "WolfSSL Error: " << err << " - " << wolfSSL_ERR_error_string(err, errBuf) << std::endl;
        }
#endif
        HandleDisconnect("Handshake/Connect failed");
    }
}

void Client::HandleDisconnect(const std::string& reason) {
    connection_id_++;
    if (reason != "QUIC_TIMEOUT") {
        std::cout << reason << std::endl;
    }

    if (mux_session_) {
        mux_session_->stop();
        mux_session_.reset();
    }
    if (quic_session_) {
        quic_session_->close_session();
        quic_session_.reset();
    }

    if (stopping_) return;

    if (protocol_ == "auto") {
        if (current_protocol_ == "quic") {
            std::cout << "Switching to TCP failover..." << std::endl;
            current_protocol_ = "tcp";
            reconnect_delay_sec_ = 0; // Immediate failover
        } else if (current_protocol_ == "tcp") {
            std::cout << "Switching to WebSocket failover..." << std::endl;
            current_protocol_ = "websocket";
            reconnect_delay_sec_ = 0; // Immediate failover
        } else {
            // Already on WebSocket, try QUIC again for the next reconnection cycle
            current_protocol_ = "quic";
        }
    }

    ScheduleReconnect();
}

void Client::ScheduleReconnect() {
    if (stopping_) return;

    if (reconnect_delay_sec_ < 600) {
        reconnect_delay_sec_ += 10;
    }
    
    std::cout << "Reconnecting in " << reconnect_delay_sec_ << " seconds..." << std::endl;
    
    reconnect_timer_.expires_after(std::chrono::seconds(reconnect_delay_sec_));
    reconnect_timer_.async_wait([this](std::error_code ec) {
        if (!ec) {
            DoConnect();
        }
    });
}

void Client::SendMessage(protocol::MessageType type, const std::vector<uint8_t>& body) {
    auto self(shared_from_this());
    protocol::Message msg{type, body};
    std::vector<uint8_t> encoded = msg.Encode();
    uint32_t final_len = static_cast<uint32_t>(encoded.size());
    std::vector<uint8_t> to_send_body = encoded;

    if (compression_) {
        size_t const cSizeBound = ZSTD_compressBound(encoded.size());
        std::vector<uint8_t> compressed(cSizeBound);
        size_t const cSize = ZSTD_compress(compressed.data(), cSizeBound, encoded.data(), encoded.size(), 1);
        if (!ZSTD_isError(cSize)) {
            compressed.resize(cSize);
            to_send_body = compressed;
            final_len = static_cast<uint32_t>(cSize) | protocol::COMPRESSION_FLAG;
        }
    }

    uint32_t net_len = asio::detail::socket_ops::host_to_network_long(final_len);
    auto data = std::make_shared<std::vector<uint8_t>>();
    data->resize(sizeof(net_len) + to_send_body.size());
    std::memcpy(data->data(), &net_len, sizeof(net_len));
    std::memcpy(data->data() + sizeof(net_len), to_send_body.data(), to_send_body.size());

    if (control_stream_) {
        control_stream_->async_write(asio::buffer(*data), [this, self, data](std::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "Failed to send message: " << ec.message() << std::endl;
            }
        });
    }
}

void Client::DoReadHeader(int conn_id) {
    auto self(shared_from_this());
    control_stream_->async_read(asio::buffer(&header_, sizeof(header_)),
        [this, self, conn_id](std::error_code ec, std::size_t) {
            if (conn_id != connection_id_) return;
            if (!ec) {
                uint32_t h = asio::detail::socket_ops::network_to_host_long(header_.body_length);
                header_.body_length = h; // Store host-byte order back
                uint32_t length = h & protocol::LENGTH_MASK;
                DoReadBody(length, conn_id);
            } else {
                HandleDisconnect("Control stream closed: " + ec.message());
            }
        });
}
void Client::DoReadBody(uint32_t length, int conn_id) {
    auto self(shared_from_this());
    bool is_compressed = (header_.body_length & protocol::COMPRESSION_FLAG) != 0;
    body_data_.resize(length);
    control_stream_->async_read(asio::buffer(body_data_),
        [this, self, is_compressed, conn_id](std::error_code ec, std::size_t) {
            if (conn_id != connection_id_) return;
            if (!ec) {
                try {
                    std::vector<uint8_t> data(body_data_.begin(), body_data_.end());
                    if (is_compressed) {
                        unsigned long long const decodedSize = ZSTD_getFrameContentSize(data.data(), data.size());
                        if (decodedSize != ZSTD_CONTENTSIZE_ERROR && decodedSize != ZSTD_CONTENTSIZE_UNKNOWN) {
                            std::vector<uint8_t> decompressed(decodedSize);
                            size_t const dSize = ZSTD_decompress(decompressed.data(), decodedSize, data.data(), data.size());
                            if (!ZSTD_isError(dSize)) {
                                data = decompressed;
                            }
                        }
                    }
                    auto msg = protocol::Message::Decode(data);
                    HandleMessage(msg);
                } catch (const std::exception& e) {
                    std::cerr << "Failed to decode message: " << e.what() << std::endl;
                }
                DoReadHeader(conn_id);
            } else {
                HandleDisconnect("Control stream closed: " + ec.message());
            }
        });
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
            std::cout << "Authenticated successfully as [" << name_ << "]" << std::endl;
            RegisterProxies();
        } else {
            std::cerr << "Authentication failed: " << (resp.message.empty() ? "unknown error" : resp.message) << std::endl;
        }
    } else if (msg.type == protocol::MessageType::RegisterProxyResp) {
        auto resp = protocol::RegisterProxyRespMessage::Deserialize(msg.body);
        std::cout << "Proxy registration response: " << resp.status << " for " << resp.name << std::endl;
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
    std::map<std::string, ProxyConfig> new_proxies;
    
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
                        std::cerr << "[Client] Error parsing [" << entry.path().filename() << "]: " << e.what() << std::endl;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[Client] Error scanning conf.d: " << e.what() << std::endl;
    }

    // Diff
    for (auto const& [name, pc] : dynamic_proxies_) {
        if (new_proxies.find(name) == new_proxies.end()) {
            std::cout << "[Client] Removing dynamic proxy [" << name << "]" << std::endl;
            UnregisterProxy(name);
        }
    }

    for (auto const& [name, pc] : new_proxies) {
        auto it = dynamic_proxies_.find(name);
        if (it == dynamic_proxies_.end()) {
            std::cout << "[Client] Adding dynamic proxy [" << name << "]" << std::endl;
            RegisterProxy(pc);
        } else {
            bool changed = (it->second.type != pc.type || 
                            it->second.local_ip != pc.local_ip ||
                            it->second.local_port != pc.local_port ||
                            it->second.remote_port != pc.remote_port);
            if (changed) {
                std::cout << "[Client] Updating dynamic proxy [" << name << "]" << std::endl;
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
        std::cerr << "Unknown proxy name: " << proxy_name << std::endl;
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
                                        std::cout << "Bridging local service and mux work stream (Compressed: " << compression_ << ")" << std::endl;
                                        auto user_stream = std::make_shared<common::TcpStream>(std::move(*local_socket));
                                        
                                        std::shared_ptr<common::RateLimiter> rl;
                                        auto it = proxy_rate_limiters_.find(pc.name);
                                        if (it != proxy_rate_limiters_.end()) rl = it->second;

                                        auto bridge = std::make_shared<Bridge>(user_stream, work_stream, compression_, rl);
                                        bridge->Start();
                                    } else {
                                        std::cerr << "Failed to send ticket over mux stream" << std::endl;
                                    }
                                });
                        } else {
                            std::cerr << "Failed to connect to local service (" << pc.local_ip << ":" << pc.local_port << "): " << ec.message() << std::endl;
                        }
                    });
            } else {
                std::cerr << "Failed to resolve local service (" << pc.local_ip << "): " << ec.message() << std::endl;
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
                std::cout << "Bridging local UDP service and mux work stream (Compressed: " << compression_ << ")" << std::endl;
                
                std::shared_ptr<common::RateLimiter> rl;
                auto it = proxy_rate_limiters_.find(pc.name);
                if (it != proxy_rate_limiters_.end()) rl = it->second;

                auto bridge = std::make_shared<UdpBridge>(io_context_, work_stream, *results.begin(), compression_, rl);
                bridge->Start();
            } else {
                std::cerr << "Failed to resolve local UDP service (" << pc.local_ip << "): " << (ec ? ec.message() : "No results") << std::endl;
            }
        });
            } else {
                std::cerr << "Failed to send ticket over mux stream for UDP" << std::endl;
            }
        });
}

} // namespace client
} // namespace cfrp
