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

#include "server.h"
#include "common/quic_ngtcp2.h"
#include "common/ssl_utils.h"
#include "common/websocket_stream.h"
#include "common/utils.h"
#include <iostream>
#include <chrono>
#include <zstd.h>
#include <sstream>

namespace cfrp {
namespace server {

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
                                    if (ec != asio::error::operation_aborted) {
                                        std::cerr << "[Bridge] Write error (" << direction << "): " << ec.message() << std::endl;
                                    }
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
UdpBridge::UdpBridge(asio::io_context& io_context, std::shared_ptr<common::AsyncStream> stream, udp::socket& socket, udp::endpoint remote_endpoint, bool use_compression, std::shared_ptr<common::RateLimiter> rate_limiter)
    : stream_(std::move(stream)), rate_limiter_(std::move(rate_limiter)), socket_(socket), remote_endpoint_(remote_endpoint), use_compression_(use_compression) {
    read_buf_.resize(65535);
}

void UdpBridge::Start() {
    DoReadFromStream();
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
                                socket_.async_send_to(asio::buffer(send_buf, send_len), remote_endpoint_,
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

void UdpBridge::DoWriteToStream(const std::vector<uint8_t>& data) {
    auto self(shared_from_this());
    auto buf = std::make_shared<std::vector<uint8_t>>();
    
    uint16_t header;
    const void* write_data = data.data();
    size_t write_len = data.size();
    std::vector<uint8_t> compressed;

    if (use_compression_) {
        size_t const cSizeBound = ZSTD_compressBound(data.size());
        compressed.resize(cSizeBound);
        size_t const cSize = ZSTD_compress(compressed.data(), cSizeBound, data.data(), data.size(), 1);
        if (!ZSTD_isError(cSize) && cSize < data.size()) {
            header = static_cast<uint16_t>(cSize) | 0x8000;
            write_data = compressed.data();
            write_len = cSize;
        } else {
            header = static_cast<uint16_t>(data.size());
        }
    } else {
        header = static_cast<uint16_t>(data.size());
    }

    uint16_t n_header = asio::detail::socket_ops::host_to_network_short(header);
    buf->resize(sizeof(n_header) + write_len);
    std::memcpy(buf->data(), &n_header, sizeof(n_header));
    std::memcpy(buf->data() + sizeof(n_header), write_data, write_len);

    auto write_op = [this, self, buf]() {
        stream_->async_write(asio::buffer(*buf), [this, self, buf](std::error_code ec, std::size_t) {
            if (ec) {
                stream_->close();
            }
        });
    };

    if (rate_limiter_) {
        rate_limiter_->async_wait(data.size(), std::move(write_op));
    } else {
        write_op();
    }
}

void UdpBridge::HandleUdpPacket(const std::vector<uint8_t>& data) {
    DoWriteToStream(data);
}

// --- UdpProxyListener ---
UdpProxyListener::UdpProxyListener(Server& server, asio::io_context& io_context, uint16_t port, std::shared_ptr<ControlSession> session, const std::string& proxy_name)
    : server_(server),
      socket_(io_context, udp::endpoint(udp::v4(), port)),
      session_(session),
      proxy_name_(proxy_name) {
    std::cout << "UDP Proxy listener started for [" << proxy_name_ << "] on port " << port << std::endl;
}

void UdpProxyListener::Start() {
    DoReceive();
}

void UdpProxyListener::Stop() {
    std::error_code ec;
    socket_.close(ec);
    std::cout << "UDP Proxy listener for [" << proxy_name_ << "] stopped." << std::endl;
}

void UdpProxyListener::DoReceive() {
    auto self(shared_from_this());
    socket_.async_receive_from(asio::buffer(recv_buf_, sizeof(recv_buf_)), sender_endpoint_,
        [this, self](std::error_code ec, std::size_t length) {
            if (!ec) {
                std::string ticket;
                auto it = endpoint_to_ticket_.find(sender_endpoint_);
                if (it == endpoint_to_ticket_.end()) {
                    ticket = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "_" + std::to_string(rand());
                    endpoint_to_ticket_[sender_endpoint_] = ticket;
                    std::cout << "New UDP session for [" << proxy_name_ << "] from " << sender_endpoint_ << ", ticket: " << ticket << std::endl;
                    
                    server_.RegisterUdpSession(ticket, shared_from_this(), sender_endpoint_, proxy_name_);

                    if (auto session = session_.lock()) {
                        protocol::NewUserConnMessage m;
                        m.proxy_name = proxy_name_;
                        m.ticket = ticket;
                        session->SendMessage(protocol::MessageType::NewUserConn, m.Serialize());
                    }
                } else {
                    ticket = it->second;
                }
                
                DoReceive();
            }
        });
}

void UdpProxyListener::SendTo(const std::vector<uint8_t>& data, const udp::endpoint& endpoint) {
    socket_.async_send_to(asio::buffer(data), endpoint, [](std::error_code, std::size_t) {});
}

// --- ProxyListener ---
ProxyListener::ProxyListener(Server& server, asio::io_context& io_context, uint16_t port, std::shared_ptr<ControlSession> session, const std::string& proxy_name)
    : server_(server),
      acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
      session_(session),
      proxy_name_(proxy_name) {
    std::cout << "Proxy listener started for [" << proxy_name_ << "] on port " << port << std::endl;
}

void ProxyListener::Start() {
    DoAccept();
}

void ProxyListener::Stop() {
    std::cout << "Stopping proxy listener for [" << proxy_name_ << "]" << std::endl;
    std::error_code ec;
    acceptor_.close(ec);
    std::cout << "Proxy listener for [" << proxy_name_ << "] stopped." << std::endl;
}

void ProxyListener::DoAccept() {
    auto self(shared_from_this());
    acceptor_.async_accept([this, self](std::error_code ec, tcp::socket socket) {
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                std::cerr << "Proxy accept error: " << ec.message() << std::endl;
            }
            return;
        }

        common::SetTcpKeepalive(socket);
        std::string ticket = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::cout << "New user connection for proxy [" << proxy_name_ << "], ticket: " << ticket << std::endl;
        
        server_.RegisterUserConn(ticket, std::move(socket), proxy_name_);

        if (auto session = session_.lock()) {
            protocol::NewUserConnMessage m;
            m.proxy_name = proxy_name_;
            m.ticket = ticket;
            session->SendMessage(protocol::MessageType::NewUserConn, m.Serialize());
        }
        
        DoAccept();
    });
}

// --- ControlSession ---
void ControlSession::Start() {
    client_endpoint_ = stream_->remote_endpoint_string();
    std::cout << "[Server] New " << stream_->protocol_name() << " client connecting from " << client_endpoint_ << "..." << std::endl;
    DoReadHeader();
}

void ControlSession::Stop() {
    std::cout << "[Server] Client disconnected: " << client_endpoint_ << " [" << client_name_ << "]" << std::endl;
    server_.ReleaseClientName(client_name_);
    for (auto& proxy : proxies_) {
        proxy->Stop();
    }
    proxies_.clear();
    for (auto& proxy : udp_proxies_) {
        proxy->Stop();
    }
    udp_proxies_.clear();
    for (const auto& domain : registered_domains_) {
        server_.RemoveVhostRoute(domain);
    }
    registered_domains_.clear();
    stream_->close();
}

void ControlSession::SendMessage(protocol::MessageType type, const std::vector<uint8_t>& body) {
    protocol::Message msg{type, body};
    std::vector<uint8_t> encoded = msg.Encode();
    uint32_t final_len = static_cast<uint32_t>(encoded.size());
    std::vector<uint8_t> to_send_body = encoded;

    if (compression_enabled_) {
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

    auto self(shared_from_this());
    stream_->async_write(asio::buffer(*data), [this, self, data](std::error_code ec, std::size_t) {
        if (ec) {
            std::cerr << "Failed to send message: " << ec.message() << std::endl;
        }
    });
}

void ControlSession::DoReadHeader() {
    auto self(shared_from_this());
    stream_->async_read(asio::buffer(&header_, sizeof(header_)),
        [this, self](std::error_code ec, std::size_t) {
            if (!ec) {
                uint32_t h = asio::detail::socket_ops::network_to_host_long(header_.body_length);
                header_.body_length = h; // Store host-byte order back
                uint32_t length = h & protocol::LENGTH_MASK;
                DoReadBody(length);
            } else {
                std::cout << "Control session closed: " << ec.message() << std::endl;
                Stop();
            }
        });
}
void ControlSession::DoReadBody(uint32_t length) {
    bool is_compressed = (header_.body_length & protocol::COMPRESSION_FLAG) != 0;
    if (is_compressed) compression_enabled_ = true;

    body_data_.resize(length);
    auto self(shared_from_this());
    stream_->async_read(asio::buffer(body_data_),
        [this, self, is_compressed](std::error_code ec, std::size_t) {
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
                DoReadHeader();
            } else {
                std::cout << "Control session closed: " << ec.message() << std::endl;
                Stop();
            }
        });
}

void ControlSession::HandleMessage(const protocol::Message& msg) {
    if (msg.type == protocol::MessageType::Login) {
        HandleLogin(msg.body);
        return;
    }

    if (!authenticated_) {
        std::cerr << "Unauthenticated message received: " << static_cast<int>(msg.type) << std::endl;
        Stop();
        return;
    }

    if (msg.type == protocol::MessageType::RegisterProxy) {
        auto m = protocol::RegisterProxyMessage::Deserialize(msg.body);
        std::string name = m.name;
        uint16_t remote_port = m.remote_port;
        std::string type = m.type;
        if (type.empty()) type = "tcp";
        std::vector<std::string> custom_domains = m.custom_domains;

        if (m.bandwidth_limit > 0) {
            server_.CreateRateLimiter(name, m.bandwidth_limit);
        }

        if (type != "http" && type != "https" && !server_.IsPortAllowed(remote_port)) {
            std::cerr << "[Server] Proxy registration rejected: port " << remote_port << " is not in allowed ranges." << std::endl;
            protocol::RegisterProxyRespMessage resp;
            resp.status = "error";
            resp.message = "port not allowed";
            resp.name = name;
            SendMessage(protocol::MessageType::RegisterProxyResp, resp.Serialize());
            return;
        }
        
        try {
            if (type == "udp") {
                auto listener = std::make_shared<UdpProxyListener>(server_, io_context_, remote_port, shared_from_this(), name);
                listener->Start();
                udp_proxies_.push_back(listener);
            } else if (type == "http" || type == "https") {
                for (const auto& domain : custom_domains) {
                    server_.AddVhostRoute(domain, shared_from_this(), name, type);
                    registered_domains_.push_back(domain);
                }
            } else {
                auto listener = std::make_shared<ProxyListener>(server_, io_context_, remote_port, shared_from_this(), name);
                listener->Start();
                proxies_.push_back(listener);
            }
            
            protocol::RegisterProxyRespMessage resp;
            resp.status = "ok";
            resp.name = name;
            SendMessage(protocol::MessageType::RegisterProxyResp, resp.Serialize());
        } catch (const std::exception& e) {
            std::cerr << "Failed to start proxy listener: " << e.what() << std::endl;
            protocol::RegisterProxyRespMessage resp;
            resp.status = "error";
            resp.message = e.what();
            SendMessage(protocol::MessageType::RegisterProxyResp, resp.Serialize());
        }
    } else if (msg.type == protocol::MessageType::UnregisterProxy) {
        auto m = protocol::UnregisterProxyMessage::Deserialize(msg.body);
        std::string name = m.name;
        std::cout << "[Server] Unregistering proxy [" << name << "]" << std::endl;
        
        auto it = std::find_if(proxies_.begin(), proxies_.end(), [&](const std::shared_ptr<ProxyListener>& p) {
            return p->name() == name;
        });
        if (it != proxies_.end()) {
            (*it)->Stop();
            proxies_.erase(it);
            return;
        }

        auto it_udp = std::find_if(udp_proxies_.begin(), udp_proxies_.end(), [&](const std::shared_ptr<UdpProxyListener>& p) {
            return p->name() == name;
        });
        if (it_udp != udp_proxies_.end()) {
            (*it_udp)->Stop();
            udp_proxies_.erase(it_udp);
            return;
        }
    }
}

void ControlSession::HandleLogin(const std::vector<uint8_t>& body) {
    auto m = protocol::LoginMessage::Deserialize(body);
    std::string token = m.token;
    std::string requested_name = m.name;
    protocol::LoginRespMessage resp;

    if (token != server_.GetToken()) {
        std::cout << "Client authentication failed: invalid token." << std::endl;
        resp.status = "error";
        resp.message = "Invalid token";
        SendMessage(protocol::MessageType::LoginResp, resp.Serialize());
        Stop();
        return;
    }

    if (!server_.IsClientAllowed(requested_name)) {
        std::cout << "[Server] Client registration rejected: name [" << requested_name << "] is not in whitelist." << std::endl;
        resp.status = "error";
        resp.message = "client name not allowed";
        SendMessage(protocol::MessageType::LoginResp, resp.Serialize());
        Stop();
        return;
    }

    client_name_ = server_.AllocateClientName(requested_name);
    std::cout << "[Server] Client authenticated successfully (" << stream_->protocol_name() << "): " 
                << client_endpoint_ << " as [" << client_name_ << "]" << std::endl;
    std::cout << "[Server] Client [" << client_name_ << "] is READY." << std::endl;
    authenticated_ = true;
    resp.status = "ok";
    resp.name = client_name_;
    SendMessage(protocol::MessageType::LoginResp, resp.Serialize());
}

// --- Server ---
Server::Server(asio::io_context& io_context, const std::string& bind_addr, uint16_t bind_port, const std::string& token, const SslConfig& ssl_config, const std::string& protocol, const std::vector<PortRange>& allowed_ports, const std::vector<std::string>& allowed_clients)
    : io_context_(io_context),
      acceptor_(io_context_, tcp::endpoint(asio::ip::make_address(bind_addr), bind_port)),
      udp_socket_(io_context_, udp::endpoint(asio::ip::make_address(bind_addr), bind_port)),
      token_(token),
      protocol_(protocol),
      ssl_config_(ssl_config),
      allowed_ports_(allowed_ports),
      allowed_clients_(allowed_clients) {
    
    if (ssl_config_.enable || protocol_ == "quic" || protocol_ == "auto") {
        if (ssl_config_.auto_generate) {
            common::CertConfig cert_config;
            cert_config.ca_cert_file = ssl_config_.ca_file;
            cert_config.server_cert_file = ssl_config_.cert_file;
            cert_config.server_key_file = ssl_config_.key_file;
            // Derive ca_key_file from ca_cert_file by changing extension to .key if not specified differently
            // but for now we'll just use a default or derive it.
            cert_config.ca_key_file = std::string(ssl_config_.ca_file).replace(ssl_config_.ca_file.find(".crt"), 4, ".key");
            
            common::SslUtils::EnsureCertificates(cert_config);
        }

        ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv12);
        ssl_ctx_->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);
        
        std::string cert = ssl_config_.cert_file;
        std::string key = ssl_config_.key_file;

        try {
            ssl_ctx_->use_certificate_chain_file(cert);
            ssl_ctx_->use_private_key_file(key, asio::ssl::context::pem);
            std::cout << "SSL/QUIC certificate loaded: " << cert << std::endl;
        } catch (const std::exception& e) {
            if (protocol_ == "quic" || protocol_ == "auto") {
                std::cerr << "Warning: Failed to load certificate for QUIC/SSL: " << e.what() << std::endl;
                std::cerr << "QUIC requires a certificate to function. Please check config_server.toml" << std::endl;
            }
        }
    }
    
    std::string display_proto = protocol_;
    if (protocol_ == "auto") display_proto = "auto (TCP/QUIC)";
    std::cout << "Server initialized on " << bind_addr << ":" << bind_port << " (" << display_proto << " Mux Enabled)" << std::endl;
}

void Server::Run() {
    std::cout << "Starting cfrp server loop..." << std::endl;
    DoAccept();
    if (protocol_ == "quic" || protocol_ == "auto") {
        DoUdpRead();
    }

    if (vhost_http_port_ > 0) {
        try {
            vhost_http_acceptor_ = std::make_unique<tcp::acceptor>(io_context_, tcp::endpoint(tcp::v4(), vhost_http_port_));
            std::cout << "[Server] Vhost HTTP listener started on port " << vhost_http_port_ << std::endl;
            DoVhostAccept(vhost_http_acceptor_, "http");
        } catch (const std::exception& e) {
            std::cerr << "[Server] Failed to start Vhost HTTP listener: " << e.what() << std::endl;
        }
    }

    if (vhost_https_port_ > 0) {
        try {
            vhost_https_acceptor_ = std::make_unique<tcp::acceptor>(io_context_, tcp::endpoint(tcp::v4(), vhost_https_port_));
            std::cout << "[Server] Vhost HTTPS listener started on port " << vhost_https_port_ << std::endl;
            DoVhostAccept(vhost_https_acceptor_, "https");
        } catch (const std::exception& e) {
            std::cerr << "[Server] Failed to start Vhost HTTPS listener: " << e.what() << std::endl;
        }
    }
}

void Server::Stop() {
    std::cout << "Stopping server..." << std::endl;
    std::error_code ec;

    // 1. Stop accepting new connections
    acceptor_.close(ec);
    if (vhost_http_acceptor_) vhost_http_acceptor_->close(ec);
    if (vhost_https_acceptor_) vhost_https_acceptor_->close(ec);

    // 2. Close QUIC sessions BEFORE closing the UDP socket they use
    // We collect them first to avoid iterator invalidation when callbacks are triggered
    std::vector<std::shared_ptr<common::quic::QuicSession>> sessions_to_close;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        for (auto& pair : quic_sessions_) {
            sessions_to_close.push_back(pair.second);
        }
    }

    for (auto& session : sessions_to_close) {
        session->set_on_closed(nullptr); // Prevent callback from modifying quic_sessions_
        session->close_session();
    }

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        quic_sessions_.clear();
    }

    // 3. Now it's safe to close the UDP socket
    udp_socket_.close(ec);
}
void Server::DoUdpRead() {
    auto endpoint = std::make_shared<udp::endpoint>();
    udp_socket_.async_receive_from(asio::buffer(udp_recv_buf_, sizeof(udp_recv_buf_)), *endpoint,
        [this, endpoint](std::error_code ec, std::size_t length) {
            if (!ec) {
                auto it = quic_sessions_.find(*endpoint);
                if (it == quic_sessions_.end()) {
                    // Parse QUIC header to get CIDs
                    ngtcp2_version_cid vcid;
                    int res = ngtcp2_pkt_decode_version_cid(&vcid, udp_recv_buf_, length, 8);
                    
                    if (res != 0) {
                        DoUdpRead();
                        return;
                    }

                    ngtcp2_cid n_dcid, n_scid;
                    ngtcp2_cid_init(&n_dcid, vcid.dcid, vcid.dcidlen);
                    ngtcp2_cid_init(&n_scid, vcid.scid, vcid.scidlen);

                    auto session = std::make_shared<common::quic::QuicSession>(udp_socket_, *endpoint, true);
                    
                    if (!ssl_ctx_) {
                        ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv13);
                    }
                    
                    session->set_on_new_stream([this](std::shared_ptr<common::quic::QuicStream> quic_stream) {
                        auto mux_session = std::make_shared<common::mux::Session>(quic_stream, true);
                        mux_session->start([this, mux_session](std::shared_ptr<common::mux::MuxStream> new_stream) {
                            HandleNewMuxStream(mux_session, new_stream);
                        });
                    });

                    session->set_on_closed([this, endpoint](std::shared_ptr<common::quic::QuicSession> s) {
                        std::cout << "[Server] QUIC session closed for " << *endpoint << std::endl;
                        std::lock_guard<std::mutex> lock(map_mutex_);
                        quic_sessions_.erase(*endpoint);
                    });

                    session->init(ssl_ctx_->native_handle(), &n_dcid, &n_scid);
                    {
                        std::lock_guard<std::mutex> lock(map_mutex_);
                        quic_sessions_[*endpoint] = session;
                    }
                    it = quic_sessions_.find(*endpoint);
                }
                it->second->handle_packet(udp_recv_buf_, length);
                DoUdpRead();
            }
        });
}

void Server::RegisterUserConn(const std::string& ticket, tcp::socket socket, const std::string& proxy_name, const std::vector<uint8_t>& initial_data) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    pending_user_conns_.emplace(ticket, TcpSessionInfo{std::move(socket), initial_data, proxy_name});
}

void Server::RegisterUdpSession(const std::string& ticket, std::shared_ptr<UdpProxyListener> listener, udp::endpoint endpoint, const std::string& proxy_name) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    pending_udp_sessions_.emplace(ticket, UdpSessionInfo{listener, endpoint, proxy_name});
}

std::string Server::AllocateClientName(const std::string& requested_name) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    std::string name = requested_name;
    if (name.empty()) {
        name = "client";
    }

    std::string final_name = name;
    int suffix = 1;
    while (std::find(active_client_names_.begin(), active_client_names_.end(), final_name) != active_client_names_.end()) {
        final_name = name + "_" + std::to_string(suffix++);
    }
    active_client_names_.push_back(final_name);
    return final_name;
}

void Server::ReleaseClientName(const std::string& name) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = std::find(active_client_names_.begin(), active_client_names_.end(), name);
    if (it != active_client_names_.end()) {
        active_client_names_.erase(it);
    }
}

void Server::DoAccept() {
    acceptor_.async_accept(
        [this](std::error_code ec, tcp::socket socket) {
            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    std::cerr << "Accept error: " << ec.message() << std::endl;
                    DoAccept();
                }
                return;
            }

            common::SetTcpKeepalive(socket);

            std::shared_ptr<common::AsyncStream> stream;            if (ssl_config_.enable) {
                stream = std::make_shared<common::SslStream>(std::move(socket), *ssl_ctx_);
            } else {
                stream = std::make_shared<common::TcpStream>(std::move(socket));
            }

            if (protocol_ == "websocket") {
                stream = std::make_shared<common::WebsocketStream>(stream, false);
            }

            stream->async_handshake(asio::ssl::stream_base::server, [this, stream](std::error_code ec) {
                if (!ec) {
                    auto start_mux = [this](std::shared_ptr<common::AsyncStream> s) {
                        auto mux_session = std::make_shared<common::mux::Session>(s, true);
                        mux_session->start([this, mux_session](std::shared_ptr<common::mux::MuxStream> new_stream) {
                            HandleNewMuxStream(mux_session, new_stream);
                        });
                    };

                    if (protocol_ == "auto") {
                        auto first_byte = std::make_shared<uint8_t>(0);
                        stream->async_read(asio::buffer(first_byte.get(), 1), [this, stream, first_byte, start_mux](std::error_code ec, std::size_t) {
                            if (!ec) {
                                std::shared_ptr<common::AsyncStream> buffered = std::make_shared<common::BufferedStream>(stream, std::vector<uint8_t>{*first_byte});
                                if (*first_byte == 'G') { // 'G' from GET (WebSocket)
                                    auto ws_stream = std::make_shared<common::WebsocketStream>(buffered, false);
                                    ws_stream->async_handshake(asio::ssl::stream_base::server, [ws_stream, start_mux](std::error_code ec) {
                                        if (!ec) {
                                            start_mux(ws_stream);
                                        }
                                    });
                                } else {
                                    start_mux(buffered);
                                }
                            }
                        });
                    } else if (protocol_ == "websocket") {
                        // Already wrapped, but we need to complete the WS handshake
                        stream->async_handshake(asio::ssl::stream_base::server, [stream, start_mux](std::error_code ec) {
                            if (!ec) {
                                start_mux(stream);
                            }
                        });
                    } else {
                        start_mux(stream);
                    }
                } else {
                    std::cerr << "SSL handshake failed: " << ec.message() << std::endl;
                }
            });
            
            DoAccept();
        });
}

void Server::HandleNewMuxStream(std::shared_ptr<common::mux::Session> mux_session, std::shared_ptr<common::mux::MuxStream> stream) {
    if (stream->id() == 1) {
        std::cout << "Control stream (ID 1) requested. Starting session..." << std::endl;
        std::make_shared<ControlSession>(*this, stream, io_context_)->Start();
    } else {
        // Work connection
        auto ticket_ptr = std::make_shared<std::vector<uint8_t>>();
        ticket_ptr->resize(65);
        
        stream->async_read(asio::buffer(*ticket_ptr),
            [this, stream, ticket_ptr](std::error_code ec, std::size_t) {
                if (!ec) {
                    bool use_compression = ((*ticket_ptr)[0] == 0x01);
                    std::string ticket(reinterpret_cast<char*>(ticket_ptr->data() + 1), 64);
                    ticket.erase(ticket.find_last_not_of(" \n\r\t") + 1);

                    std::lock_guard<std::mutex> lock(map_mutex_);
                    auto it_tcp = pending_user_conns_.find(ticket);
                    if (it_tcp != pending_user_conns_.end()) {
                        std::cout << "Splicing user TCP connection and mux work stream for ticket: " << ticket << " (Compressed: " << use_compression << ")" << std::endl;
                        auto initial_data = std::make_shared<std::vector<uint8_t>>(std::move(it_tcp->second.initial_data));
                        auto user_socket = std::make_shared<tcp::socket>(std::move(it_tcp->second.socket));
                        auto proxy_name = it_tcp->second.proxy_name;
                        pending_user_conns_.erase(it_tcp);

                        std::shared_ptr<common::RateLimiter> rl;
                        auto it_rl = proxy_rate_limiters_.find(proxy_name);
                        if (it_rl != proxy_rate_limiters_.end()) rl = it_rl->second;

                        if (!initial_data->empty()) {
                            // If compression is enabled, we MUST wrap initial_data in a compressed header
                            // because the client Bridge expects it.
                            uint32_t final_header;
                            const void* write_buf = initial_data->data();
                            size_t write_len = initial_data->size();
                            std::vector<uint8_t> compressed;

                            if (use_compression) {
                                size_t const cSizeBound = ZSTD_compressBound(initial_data->size());
                                compressed.resize(cSizeBound);
                                size_t const cSize = ZSTD_compress(compressed.data(), cSizeBound, initial_data->data(), initial_data->size(), 1);
                                if (!ZSTD_isError(cSize) && cSize < initial_data->size()) {
                                    final_header = asio::detail::socket_ops::host_to_network_long(static_cast<uint32_t>(cSize) | protocol::COMPRESSION_FLAG);
                                    write_buf = compressed.data();
                                    write_len = cSize;
                                } else {
                                    final_header = asio::detail::socket_ops::host_to_network_long(static_cast<uint32_t>(initial_data->size()));
                                }
                            } else {
                                // Mux is raw, but we don't use this branch if use_compression is true.
                                // Actually, Bridge only exists if we use compression or not.
                                // If use_compression is false, Bridge uses async_read_some (no headers).
                            }

                            if (use_compression) {
                                auto packet = std::make_shared<std::vector<uint8_t>>();
                                packet->resize(sizeof(final_header) + write_len);
                                std::memcpy(packet->data(), &final_header, sizeof(final_header));
                                std::memcpy(packet->data() + sizeof(final_header), write_buf, write_len);

                                stream->async_write(asio::buffer(*packet), [this, stream, packet, user_socket, use_compression, rl](std::error_code ec, std::size_t) {
                                    if (!ec) {
                                        auto user_stream = std::make_shared<common::TcpStream>(std::move(*user_socket));
                                        auto bridge = std::make_shared<Bridge>(user_stream, stream, use_compression, rl);
                                        bridge->Start();
                                    } else {
                                        stream->close();
                                    }
                                });
                            } else {
                                stream->async_write(asio::buffer(*initial_data), [this, stream, initial_data, user_socket, use_compression, rl](std::error_code ec, std::size_t) {
                                    if (!ec) {
                                        auto user_stream = std::make_shared<common::TcpStream>(std::move(*user_socket));
                                        auto bridge = std::make_shared<Bridge>(user_stream, stream, use_compression, rl);
                                        bridge->Start();
                                    } else {
                                        stream->close();
                                    }
                                });
                            }
                        } else {
                            auto user_stream = std::make_shared<common::TcpStream>(std::move(*user_socket));
                            auto bridge = std::make_shared<Bridge>(user_stream, stream, use_compression, rl);
                            bridge->Start();
                        }
                    } else {
                        auto it_udp = pending_udp_sessions_.find(ticket);
                        if (it_udp != pending_udp_sessions_.end()) {
                            std::cout << "Splicing user UDP session and mux work stream for ticket: " << ticket << " (Compressed: " << use_compression << ")" << std::endl;
                            std::shared_ptr<common::RateLimiter> rl;
                            auto it_rl = proxy_rate_limiters_.find(it_udp->second.proxy_name);
                            if (it_rl != proxy_rate_limiters_.end()) rl = it_rl->second;
                            
                            auto bridge = std::make_shared<UdpBridge>(io_context_, stream, it_udp->second.listener->socket(), it_udp->second.endpoint, use_compression, rl);
                            bridge->Start();
                            pending_udp_sessions_.erase(it_udp);
                        } else {
                            std::cerr << "No pending connection/session for ticket: " << ticket << std::endl;
                            stream->close();
                        }
                    }
                }
            });
    }
}

void Server::AddVhostRoute(const std::string& domain, std::shared_ptr<ControlSession> session, const std::string& proxy_name, const std::string& type) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    vhost_routes_[domain] = {session, proxy_name, type};
    std::cout << "[Server] Added vhost route: " << domain << " -> " << proxy_name << " (" << type << ")" << std::endl;
}

void Server::RemoveVhostRoute(const std::string& domain) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    vhost_routes_.erase(domain);
}

std::shared_ptr<common::RateLimiter> Server::GetRateLimiter(const std::string& proxy_name) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = proxy_rate_limiters_.find(proxy_name);
    if (it != proxy_rate_limiters_.end()) return it->second;
    return nullptr;
}

void Server::CreateRateLimiter(const std::string& proxy_name, int64_t bytes_per_sec) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    if (proxy_rate_limiters_.find(proxy_name) == proxy_rate_limiters_.end()) {
        proxy_rate_limiters_[proxy_name] = std::make_shared<common::RateLimiter>(io_context_, bytes_per_sec);
    } else {
        proxy_rate_limiters_[proxy_name]->set_rate(bytes_per_sec);
    }
}

void Server::DoVhostAccept(std::unique_ptr<tcp::acceptor>& acceptor, const std::string& type) {
    auto socket = std::make_shared<tcp::socket>(io_context_);
    acceptor->async_accept(*socket, [this, socket, &acceptor, type](std::error_code ec) {
        if (!ec) {
            auto buffer = std::make_shared<std::vector<uint8_t>>(4096);
            socket->async_read_some(asio::buffer(*buffer), [this, socket, buffer, type](std::error_code ec, std::size_t length) {
                if (!ec) {
                    buffer->resize(length);
                    std::string domain;
                    if (type == "http") {
                        std::string data(reinterpret_cast<char*>(buffer->data()), length);
                        size_t pos = data.find("Host: ");
                        if (pos != std::string::npos) {
                            size_t end = data.find("\r\n", pos);
                            if (end != std::string::npos) {
                                domain = data.substr(pos + 6, end - (pos + 6));
                                size_t colon = domain.find(":");
                                if (colon != std::string::npos) domain = domain.substr(0, colon);
                                // Trim whitespace
                                domain.erase(0, domain.find_first_not_of(" \t\r\n"));
                                domain.erase(domain.find_last_not_of(" \t\r\n") + 1);
                            }
                        }
                    } else if (type == "https") {
                        // Basic SNI parser
                        const uint8_t* data = buffer->data();
                        size_t size = buffer->size();
                        if (size > 5 && data[0] == 0x16) { // Handshake
                            size_t len = (data[3] << 8) | data[4];
                            if (len + 5 <= size && data[5] == 0x01) { // ClientHello
                                size_t pos = 5 + 4; // Skip Handshake header
                                pos += 2; // Version
                                pos += 32; // Random
                                if (pos < size) {
                                    size_t sess_id_len = data[pos];
                                    pos += 1 + sess_id_len;
                                    if (pos + 2 <= size) {
                                        size_t cipher_len = (data[pos] << 8) | data[pos+1];
                                        pos += 2 + cipher_len;
                                        if (pos + 1 <= size) {
                                            size_t comp_len = data[pos];
                                            pos += 1 + comp_len;
                                            if (pos + 2 <= size) {
                                                size_t ext_len = (data[pos] << 8) | data[pos+1];
                                                pos += 2;
                                                size_t end_ext = pos + ext_len;
                                                while (pos + 4 <= size && pos + 4 <= end_ext) {
                                                    size_t etype = (data[pos] << 8) | data[pos+1];
                                                    size_t elen = (data[pos+2] << 8) | data[pos+3];
                                                    pos += 4;
                                                    if (etype == 0x0000) { // SNI
                                                        if (pos + 2 <= size) {
                                                            size_t list_len = (data[pos] << 8) | data[pos+1];
                                                            pos += 2;
                                                            if (pos + 3 <= size && data[pos] == 0x00) { // Name type: host_name
                                                                size_t name_len = (data[pos+1] << 8) | data[pos+2];
                                                                pos += 3;
                                                                if (pos + name_len <= size) {
                                                                    domain = std::string(reinterpret_cast<const char*>(data + pos), name_len);
                                                                }
                                                            }
                                                        }
                                                        break;
                                                    }
                                                    pos += elen;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    std::shared_ptr<ControlSession> session;
                    std::string proxy_name;
                    {
                        std::lock_guard<std::mutex> lock(map_mutex_);
                        auto it = vhost_routes_.find(domain);
                        if (it != vhost_routes_.end()) {
                            session = it->second.session.lock();
                            proxy_name = it->second.proxy_name;
                        }
                    }

                    if (session) {
                        std::string ticket = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "_vhost_" + std::to_string(rand());
                        RegisterUserConn(ticket, std::move(*socket), proxy_name, *buffer);
                        protocol::NewUserConnMessage m;
                        m.proxy_name = proxy_name;
                        m.ticket = ticket;
                        session->SendMessage(protocol::MessageType::NewUserConn, m.Serialize());
                    } else {
                        // std::cerr << "[Server] No vhost route for domain: [" << domain << "] (" << type << ")" << std::endl;
                    }
                }
            });
        }
        DoVhostAccept(acceptor, type);
    });
}

bool Server::IsPortAllowed(uint16_t port) const {
    if (allowed_ports_.empty()) return true;
    for (const auto& range : allowed_ports_) {
        if (port >= range.start && port <= range.end) {
            return true;
        }
    }
    return false;
}

bool Server::IsClientAllowed(const std::string& name) const {
    if (allowed_clients_.empty()) return true;
    return std::find(allowed_clients_.begin(), allowed_clients_.end(), name) != allowed_clients_.end();
}

} // namespace server
} // namespace cfrp
