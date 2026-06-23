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

// --- UdpBridge ---
UdpBridge::UdpBridge(asio::io_context& io_context, std::shared_ptr<common::AsyncStream> stream, udp::socket& socket, udp::endpoint remote_endpoint, bool use_compression, std::shared_ptr<common::RateLimiter> rate_limiter, std::shared_ptr<common::BufferPool> buffer_pool)
    : stream_(std::move(stream)), rate_limiter_(std::move(rate_limiter)), buffer_pool_(std::move(buffer_pool)), socket_(socket), remote_endpoint_(remote_endpoint), use_compression_(use_compression) {
    if (!buffer_pool_) buffer_pool_ = common::BufferPool::CreateDefault();
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
                                if (bytes_sent_) *bytes_sent_ += final_len;
                                socket_.async_send_to(asio::buffer(final_buf.get(), final_len), remote_endpoint_,
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

// --- UdpProxyListener ---
UdpProxyListener::UdpProxyListener(Server& server, asio::io_context& io_context, uint16_t port, std::shared_ptr<ControlSession> session, const std::string& proxy_name, std::shared_ptr<common::BufferPool> buffer_pool)
    : server_(server),
      buffer_pool_(std::move(buffer_pool)),
      socket_(io_context, udp::endpoint(udp::v4(), port)),
      port_(port),
      session_(session),
      proxy_name_(proxy_name) {
    if (!buffer_pool_) buffer_pool_ = common::BufferPool::CreateDefault();
    common::Logger::Info("UDP Proxy listener started for [" + proxy_name_ + "] on port " + std::to_string(port));
}

void UdpProxyListener::Start() {
    DoReceive();
}

void UdpProxyListener::Stop() {
    std::error_code ec;
    socket_.close(ec);
    common::Logger::Info("UDP Proxy listener for [" + proxy_name_ + "] stopped.");
}

ProxyStats UdpProxyListener::GetStats() const {
    ProxyStats s;
    s.name = proxy_name_;
    s.type = "udp";
    s.port = port_;
    s.active_conns = 0;
    s.total_conns = total_conns_.load();
    s.bytes_sent = stats_.bytes_sent.load();
    s.bytes_received = stats_.bytes_received.load();
    return s;
}

void UdpProxyListener::DoReceive() {
    auto self(shared_from_this());
    auto buffer = buffer_pool_->Get(65535);
    socket_.async_receive_from(asio::buffer(buffer.get(), 65535), sender_endpoint_,
        [this, self, buffer](std::error_code ec, std::size_t length) {
            if (!ec) {
                stats_.bytes_received += length;
                std::string ticket;
                auto it = endpoint_to_ticket_.find(sender_endpoint_);
                if (it == endpoint_to_ticket_.end()) {
                    total_conns_++;
                    ticket = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "_" + std::to_string(rand());
                    endpoint_to_ticket_[sender_endpoint_] = ticket;
                    common::Logger::Info("New UDP session for [" + proxy_name_ + "] from " + sender_endpoint_.address().to_string() + ":" + std::to_string(sender_endpoint_.port()) + ", ticket: " + ticket);

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
                return;
            }

            if (ec != asio::error::operation_aborted && socket_.is_open()) {
                common::Logger::Error("UDP receive error for [" + proxy_name_ + "]: " + ec.message());
                auto timer = std::make_shared<asio::steady_timer>(server_.get_io_context());
                timer->expires_after(std::chrono::milliseconds(100));
                timer->async_wait([this, self, timer](std::error_code) {
                    if (socket_.is_open()) DoReceive();
                });
            }
        });
}

void UdpProxyListener::SendTo(const std::vector<uint8_t>& data, const udp::endpoint& endpoint) {
    socket_.async_send_to(asio::buffer(data), endpoint, [](std::error_code, std::size_t) {});
}

void UdpProxyListener::RemoveEndpoint(const udp::endpoint& endpoint) {
    endpoint_to_ticket_.erase(endpoint);
}

// --- ProxyListener ---
ProxyListener::ProxyListener(Server& server, asio::io_context& io_context, uint16_t port, std::shared_ptr<ControlSession> session, const std::string& proxy_name)
    : server_(server),
      acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
      port_(port),
      strand_(asio::make_strand(io_context)),
      session_(session),
      proxy_name_(proxy_name) {
    common::Logger::Info("Proxy listener started for [" + proxy_name_ + "] on port " + std::to_string(port));
}

void ProxyListener::Start() {
    DoAccept();
}

void ProxyListener::Stop() {
    common::Logger::Info("Stopping proxy listener for [" + proxy_name_ + "]");
    std::error_code ec;
    acceptor_.close(ec);
    common::Logger::Info("Proxy listener for [" + proxy_name_ + "] stopped.");
}

ProxyStats ProxyListener::GetStats() const {
    ProxyStats s;
    s.name = proxy_name_;
    s.type = "tcp";
    s.port = port_;
    s.active_conns = active_conns_.load();
    s.total_conns = total_conns_.load();
    s.bytes_sent = stats_.bytes_sent.load();
    s.bytes_received = stats_.bytes_received.load();
    return s;
}

void ProxyListener::DoAccept() {
    auto self(shared_from_this());
    acceptor_.async_accept(asio::bind_executor(strand_, [this, self](std::error_code ec, tcp::socket socket) {
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                common::Logger::Error("Proxy accept error: " + ec.message());
                auto timer = std::make_shared<asio::steady_timer>(server_.get_io_context());
                timer->expires_after(std::chrono::milliseconds(100));
                timer->async_wait(asio::bind_executor(strand_, [this, self, timer](std::error_code) {
                    DoAccept();
                }));
            }
            return;
        }

        total_conns_++;
        active_conns_++;

        common::SetTcpKeepalive(socket);
        std::string ticket = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        common::Logger::Info("New user connection for proxy [" + proxy_name_ + "], ticket: " + ticket);

        server_.RegisterUserConn(ticket, std::move(socket), proxy_name_);

        if (auto session = session_.lock()) {
            protocol::NewUserConnMessage m;
            m.proxy_name = proxy_name_;
            m.ticket = ticket;
            session->SendMessage(protocol::MessageType::NewUserConn, m.Serialize());
        }

        DoAccept();
    }));
}

ClientInfo ControlSession::GetInfo() const {
    ClientInfo info;
    info.name = client_name_;
    info.endpoint = client_endpoint_;
    info.protocol = stream_->protocol_name();
    
    for (const auto& p : proxies_) {
        info.proxies.push_back(p->GetStats());
    }
    for (const auto& p : udp_proxies_) {
        info.proxies.push_back(p->GetStats());
    }
    return info;
}

// --- ControlSession ---
std::shared_ptr<ProxyListener> ControlSession::FindProxy(const std::string& name) {
    for (auto& p : proxies_) if (p->name() == name) return p;
    return nullptr;
}

std::shared_ptr<UdpProxyListener> ControlSession::FindUdpProxy(const std::string& name) {
    for (auto& p : udp_proxies_) if (p->name() == name) return p;
    return nullptr;
}

void ControlSession::Start() {
    client_endpoint_ = stream_->remote_endpoint_string();
    common::Logger::Info("[Server] New " + stream_->protocol_name() + " client connecting from " + client_endpoint_ + "...");
    DoReadHeader();
}

void ControlSession::Stop() {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this()]() { Stop(); });
        return;
    }

    if (authenticated_) {
        common::Logger::Info("[Server] Client disconnected: " + client_endpoint_ + " [" + client_name_ + "]");
        server_.ReleaseClientName(client_name_);
    } else {
        common::Logger::Info("[Server] Unauthenticated client disconnected: " + client_endpoint_);
    }
    
    if (mux_session_) {
        server_.RemoveMuxControl(mux_session_);
    }

    for (auto& proxy : proxies_) {
        common::Logger::Info("[Server] Cleaning up proxy [" + proxy->name() + "] for client [" + client_name_ + "]");
        server_.ClearPendingForProxy(proxy->name());
        server_.RemoveRateLimiter(proxy->name());
        proxy->Stop();
    }
    proxies_.clear();
    for (auto& proxy : udp_proxies_) {
        common::Logger::Info("[Server] Cleaning up UDP proxy [" + proxy->name() + "] for client [" + client_name_ + "]");
        server_.ClearPendingForProxy(proxy->name());
        server_.RemoveRateLimiter(proxy->name());
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
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), type, body]() { SendMessage(type, body); });
        return;
    }
    protocol::Message msg{type, body};
    std::vector<uint8_t> encoded = msg.Encode();
    uint32_t final_len = static_cast<uint32_t>(encoded.size());
    std::vector<uint8_t> to_send_body = encoded;

    if (compression_enabled_) {
        size_t const cSizeBound = ZSTD_compressBound(encoded.size());
        auto& compressed_buf = cctx_.get_compress_buf(cSizeBound);
        size_t const cSize = cctx_.compress(compressed_buf.data(), cSizeBound, encoded.data(), encoded.size(), 1);
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
    stream_->async_write(asio::buffer(*data), asio::bind_executor(strand_, [this, self, data](std::error_code ec, std::size_t) {
        if (ec) {
            common::Logger::Error("Failed to send message: " + ec.message());
            Stop();
        }
    }));
}

void ControlSession::DoReadHeader() {
    auto self(shared_from_this());
    stream_->async_read(asio::buffer(&header_, sizeof(header_)),
        asio::bind_executor(strand_, [this, self](std::error_code ec, std::size_t) {
            if (!ec) {
                uint32_t h = asio::detail::socket_ops::network_to_host_long(header_.body_length);
                header_.body_length = h; // Store host-byte order back
                uint32_t length = h & protocol::LENGTH_MASK;
                if (length > protocol::MAX_CONTROL_MESSAGE_SIZE) {
                    common::Logger::Error("Control message too large: " + std::to_string(length) + " from " + client_endpoint_);
                    Stop();
                    return;
                }
                DoReadBody(length);
            } else {
                common::Logger::Info("Control session closed: " + ec.message() + " for " + client_endpoint_);
                Stop();
            }
        }));
}
void ControlSession::DoReadBody(uint32_t length) {
    bool is_compressed = (header_.body_length & protocol::COMPRESSION_FLAG) != 0;
    if (is_compressed) compression_enabled_ = true;

    body_data_.resize(length);
    auto self(shared_from_this());
    stream_->async_read(asio::buffer(body_data_),
        asio::bind_executor(strand_, [this, self, is_compressed](std::error_code ec, std::size_t) {
            if (!ec) {
                try {
                    std::vector<uint8_t> data(body_data_.begin(), body_data_.end());
                    if (is_compressed) {
                        unsigned long long const decodedSize = ZSTD_getFrameContentSize(data.data(), data.size());
                        if (decodedSize == ZSTD_CONTENTSIZE_ERROR ||
                            decodedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
                            decodedSize > protocol::MAX_DECOMPRESSED_SIZE) {
                            common::Logger::Error("Invalid compressed control message size from " + client_endpoint_);
                            Stop();
                            return;
                        }
                        
                        auto& decompressed_buf = dctx_.get_decompress_buf(decodedSize);
                        size_t const dSize = dctx_.decompress(decompressed_buf.data(), decodedSize, data.data(), data.size());
                        if (ZSTD_isError(dSize)) {
                            common::Logger::Error("Failed to decompress control message from " + client_endpoint_);
                            Stop();
                            return;
                        }
                        data.assign(decompressed_buf.begin(), decompressed_buf.begin() + dSize);
                    }
                    auto msg = protocol::Message::Decode(data);
                    HandleMessage(msg);
                } catch (const std::exception& e) {
                    common::Logger::Error("Failed to decode control message from " + client_endpoint_ + ": " + std::string(e.what()));
                    Stop();
                    return;
                }
                DoReadHeader();
            } else {
                common::Logger::Info("Control session closed: " + ec.message() + " for " + client_endpoint_);
                Stop();
            }
        }));
}

void ControlSession::HandleMessage(const protocol::Message& msg) {
    if (msg.type == protocol::MessageType::Login) {
        HandleLogin(msg.body);
        return;
    }

    if (!authenticated_) {
        common::Logger::Error("Unauthenticated message received from " + client_endpoint_ + ": " + std::to_string(static_cast<int>(msg.type)));
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
            common::Logger::Error("[Server] Proxy registration rejected for client " + client_name_ + " (" + client_endpoint_ + "): port " + std::to_string(remote_port) + " is not in allowed ranges.");
            protocol::RegisterProxyRespMessage resp;
            resp.status = "error";
            resp.message = "port not allowed";
            resp.name = name;
            SendMessage(protocol::MessageType::RegisterProxyResp, resp.Serialize());
            return;
        }
        
        try {
            if (type == "udp") {
                auto listener = std::make_shared<UdpProxyListener>(server_, io_context_, remote_port, shared_from_this(), name, server_.buffer_pool());
                listener->Start();
                udp_proxies_.push_back(listener);
            } else if (type == "http" || type == "https") {
                for (const auto& domain : custom_domains) {
                    server_.AddVhostRoute(domain, shared_from_this(), name, type);
                    registered_domains_.insert(domain);
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
            common::Logger::Error("Failed to start proxy listener for client " + client_name_ + " (" + client_endpoint_ + "): " + std::string(e.what()));
            protocol::RegisterProxyRespMessage resp;
            resp.status = "error";
            resp.message = e.what();
            SendMessage(protocol::MessageType::RegisterProxyResp, resp.Serialize());
        }
    } else if (msg.type == protocol::MessageType::UnregisterProxy) {
        auto m = protocol::UnregisterProxyMessage::Deserialize(msg.body);
        std::string name = m.name;
        common::Logger::Info("[Server] Unregistering proxy [" + name + "] for client [" + client_name_ + "] (" + client_endpoint_ + ")");
        
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
        common::Logger::Info("[Server] Client authentication failed from " + client_endpoint_ + ": invalid token.");
        resp.status = "error";
        resp.message = "Invalid token";
        SendMessage(protocol::MessageType::LoginResp, resp.Serialize());
        Stop();
        return;
    }

    if (!server_.IsClientAllowed(requested_name)) {
        common::Logger::Info("[Server] Client registration rejected from " + client_endpoint_ + ": name [" + requested_name + "] is not in whitelist.");
        resp.status = "error";
        resp.message = "client name not allowed";
        SendMessage(protocol::MessageType::LoginResp, resp.Serialize());
        Stop();
        return;
    }

    client_name_ = server_.AllocateClientName(requested_name, shared_from_this());
    common::Logger::Info("[Server] Client authenticated successfully (" + stream_->protocol_name() + "): "
                + client_endpoint_ + " as [" + client_name_ + "]");
    common::Logger::Info("[Server] Client [" + client_name_ + "] is READY.");
    authenticated_ = true;
    
    // We don't need a specific 'cancel' here if we check authenticated_ in the timer lambda
    
    resp.status = "ok";
    resp.name = client_name_;
    SendMessage(protocol::MessageType::LoginResp, resp.Serialize());
}

ClientInfo ControlSession::GetClientInfo() const {
    return GetInfo();
}

// --- Server ---
Server::Server(asio::io_context& io_context, const std::string& bind_addr, uint16_t bind_port, const std::string& token, const SslConfig& ssl_config, const std::string& protocol, const std::vector<PortRange>& allowed_ports, const std::vector<std::string>& allowed_clients, std::shared_ptr<common::BufferPool> buffer_pool)
    : io_context_(io_context),
      acceptor_(io_context_, tcp::endpoint(asio::ip::make_address(bind_addr), bind_port)),
      udp_socket_(io_context_, udp::endpoint(asio::ip::make_address(bind_addr), bind_port)),
      token_(token),
      protocol_(protocol),
      ssl_config_(ssl_config),
      buffer_pool_(buffer_pool),
      allowed_ports_(allowed_ports),
    allowed_clients_(allowed_clients.begin(), allowed_clients.end()) {

    if (!buffer_pool_) buffer_pool_ = common::BufferPool::CreateDefault();
    
    if (ssl_config_.enable || protocol_ == "quic" || protocol_ == "auto") {
        if (ssl_config_.auto_generate) {
            common::CertConfig cert_config;
            cert_config.ca_cert_file = ssl_config_.ca_file;
            cert_config.server_cert_file = ssl_config_.cert_file;
            cert_config.server_key_file = ssl_config_.key_file;
            cert_config.domains = ssl_config_.domains;
            // Derive ca_key_file from ca_cert_file by changing extension to .key if not specified differently
            // but for now we'll just use a default or derive it.
            cert_config.ca_key_file = std::string(ssl_config_.ca_file).replace(ssl_config_.ca_file.find(".crt"), 4, ".key");
            
            common::SslUtils::EnsureCertificates(cert_config);
        }

        ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv13);
        ssl_ctx_->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);
        
        quic_ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv13);
        quic_ssl_ctx_->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);

        std::string cert = ssl_config_.cert_file;
        std::string key = ssl_config_.key_file;

        try {
            ssl_ctx_->use_certificate_chain_file(cert);
            ssl_ctx_->use_private_key_file(key, asio::ssl::context::pem);
            
            quic_ssl_ctx_->use_certificate_chain_file(cert);
            quic_ssl_ctx_->use_private_key_file(key, asio::ssl::context::pem);
            
            common::Logger::Info("SSL/QUIC certificate loaded: " + cert);
        } catch (const std::exception& e) {
            if (protocol_ == "quic" || protocol_ == "auto") {
                common::Logger::Error("Warning: Failed to load certificate for QUIC/SSL: " + std::string(e.what()));
                common::Logger::Error("QUIC requires a certificate to function. Please check config_server.toml");
            }
        }
    }
    
    std::string display_proto = protocol_;
    if (protocol_ == "auto") display_proto = "auto (TCP/QUIC)";
    common::Logger::Info("Server initialized on " + bind_addr + ":" + std::to_string(bind_port) + " (" + display_proto + " Mux Enabled)");
}

void Server::Run() {
    common::Logger::Info("Starting cfrp server loop...");
    DoAccept();
    if (protocol_ == "quic" || protocol_ == "auto") {
        DoUdpRead();
    }

    if (vhost_http_port_ > 0) {
        try {
            vhost_http_acceptor_ = std::make_unique<tcp::acceptor>(io_context_, tcp::endpoint(tcp::v4(), vhost_http_port_));
            common::Logger::Info("[Server] Vhost HTTP listener started on port " + std::to_string(vhost_http_port_));
            DoVhostAccept(vhost_http_acceptor_, "http");
        } catch (const std::exception& e) {
            common::Logger::Error("[Server] Failed to start Vhost HTTP listener: " + std::string(e.what()));
        }
    }

    if (vhost_https_port_ > 0) {
        try {
            vhost_https_acceptor_ = std::make_unique<tcp::acceptor>(io_context_, tcp::endpoint(tcp::v4(), vhost_https_port_));
            common::Logger::Info("[Server] Vhost HTTPS listener started on port " + std::to_string(vhost_https_port_));
            DoVhostAccept(vhost_https_acceptor_, "https");
        } catch (const std::exception& e) {
            common::Logger::Error("[Server] Failed to start Vhost HTTPS listener: " + std::string(e.what()));
        }
    }
}

void Server::Stop() {
    common::Logger::Info("Stopping server...");
    std::error_code ec;
    // 1. Stop accepting new connections
    acceptor_.close(ec);
    if (vhost_http_acceptor_) vhost_http_acceptor_->close(ec);
    if (vhost_https_acceptor_) vhost_https_acceptor_->close(ec);

    // 2. Close QUIC sessions BEFORE closing the UDP socket they use
    // We collect them first to avoid iterator invalidation when callbacks are triggered
    std::vector<std::shared_ptr<common::quic::QuicSession>> sessions_to_close;
    {
        std::shared_lock<std::shared_mutex> lock(quic_mutex_);
        for (auto& pair : quic_sessions_) {
            sessions_to_close.push_back(pair.second);
        }
    }

    for (auto& session : sessions_to_close) {
        session->set_on_closed(nullptr); // Prevent callback from modifying quic_sessions_
        session->close_session();
    }

    {
        std::unique_lock<std::shared_mutex> lock(quic_mutex_);
        quic_sessions_.clear();
    }

    // 3. Now it's safe to close the UDP socket
    udp_socket_.close(ec);
}
void Server::DoUdpRead() {
    auto endpoint = std::make_shared<udp::endpoint>();
    auto buffer = buffer_pool_->Get(65535);
    udp_socket_.async_receive_from(asio::buffer(buffer.get(), 65535), *endpoint,
        [this, endpoint, buffer](std::error_code ec, std::size_t length) {
            if (!ec) {
                std::shared_ptr<common::quic::QuicSession> session;
                {
                    std::shared_lock<std::shared_mutex> lock(quic_mutex_);
                    auto it = quic_sessions_.find(*endpoint);
                    if (it != quic_sessions_.end()) {
                        session = it->second;
                    }
                }

                if (!session) {
                    // Parse QUIC header to get CIDs
                    ngtcp2_version_cid vcid;
                    int res = ngtcp2_pkt_decode_version_cid(&vcid, buffer.get(), length, 8);

                    if (res == 0) {
                        common::Logger::Info("[Server] New QUIC connection from " + endpoint->address().to_string() + ":" + std::to_string(endpoint->port()));

                        ngtcp2_cid n_dcid, n_scid;
                        ngtcp2_cid_init(&n_dcid, vcid.dcid, vcid.dcidlen);
                        ngtcp2_cid_init(&n_scid, vcid.scid, vcid.scidlen);

                        auto new_session = std::make_shared<common::quic::QuicSession>(udp_socket_, *endpoint, true, buffer_pool_);

                        if (!quic_ssl_ctx_) {
                            quic_ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv13);
                        }

                        new_session->set_on_new_stream([this](std::shared_ptr<common::quic::QuicStream> quic_stream) {
                            auto mux_session = std::make_shared<common::mux::Session>(quic_stream, true, buffer_pool_);
                            std::weak_ptr<common::mux::Session> weak_mux = mux_session;
                            mux_session->start([this, weak_mux](std::shared_ptr<common::mux::MuxStream> new_stream) {
                                if (auto ms = weak_mux.lock()) {
                                    HandleNewMuxStream(ms, new_stream);
                                }
                            });
                        });

                        new_session->set_on_closed([this, endpoint](std::shared_ptr<common::quic::QuicSession> s) {
                            common::Logger::Info("[Server] QUIC session closed for " + endpoint->address().to_string() + ":" + std::to_string(endpoint->port()));
                            std::unique_lock<std::shared_mutex> lock(quic_mutex_);
                            quic_sessions_.erase(*endpoint);
                        });

                        new_session->init(quic_ssl_ctx_->native_handle(), &n_dcid, &n_scid);
                        {
                            std::unique_lock<std::shared_mutex> lock(quic_mutex_);
                            auto it = quic_sessions_.find(*endpoint);
                            if (it != quic_sessions_.end()) {
                                session = it->second;
                            } else {
                                quic_sessions_[*endpoint] = new_session;
                                session = new_session;
                            }
                        }
                    }
                }

                if (session) {
                    auto s = session;
                    asio::post(s->strand(), [s, buffer, length]() {
                        s->handle_packet(buffer.get(), length);
                    });
                }
            }

            if (ec != asio::error::operation_aborted) {
                if (udp_socket_.is_open()) {
                    if (ec) {
                        common::Logger::Error("UDP read error: " + ec.message());
                        auto timer = std::make_shared<asio::steady_timer>(io_context_);
                        timer->expires_after(std::chrono::milliseconds(100));
                        timer->async_wait([this, timer](std::error_code) {
                            if (udp_socket_.is_open()) DoUdpRead();
                        });
                    } else {
                        DoUdpRead();
                    }
                }
            }
        });
}

void Server::RegisterUserConn(const std::string& ticket, tcp::socket socket, const std::string& proxy_name, const std::vector<uint8_t>& initial_data) {
    std::lock_guard<std::mutex> lock(pending_conn_mutex_);
    DoLazyCleanup();

    if (pending_user_conns_.size() + pending_udp_sessions_.size() >= MAX_PENDING_TICKETS) {
        common::Logger::Error("Max pending connections reached, rejecting new user connection.");
        return;
    }

    auto now = std::chrono::steady_clock::now();
    pending_user_conns_.emplace(ticket, TcpSessionInfo{std::move(socket), initial_data, proxy_name, now});
    ticket_expiration_queue_.push_back({ticket, false, now + std::chrono::seconds(10)});
}

void Server::RegisterUdpSession(const std::string& ticket, std::shared_ptr<UdpProxyListener> listener, udp::endpoint endpoint, const std::string& proxy_name) {
    std::lock_guard<std::mutex> lock(pending_conn_mutex_);
    DoLazyCleanup();

    if (pending_user_conns_.size() + pending_udp_sessions_.size() >= MAX_PENDING_TICKETS) {
        common::Logger::Error("Max pending UDP sessions reached, rejecting new session.");
        return;
    }

    auto now = std::chrono::steady_clock::now();
    pending_udp_sessions_.emplace(ticket, UdpSessionInfo{listener, endpoint, proxy_name, now});
    ticket_expiration_queue_.push_back({ticket, true, now + std::chrono::seconds(10)});
}

std::string Server::AllocateClientName(const std::string& requested_name, std::shared_ptr<ControlSession> session) {
    std::lock_guard<std::mutex> lock(client_name_mutex_);
    std::string name = requested_name;
    if (name.empty()) {
        name = "client";
    }

    std::string final_name = name;
    int suffix = 1;
    while (active_client_names_.find(final_name) != active_client_names_.end()) {
        final_name = name + "_" + std::to_string(suffix++);
    }
    active_client_names_.insert(final_name);
    sessions_[final_name] = session;
    return final_name;
}

void Server::ReleaseClientName(const std::string& name) {
    std::lock_guard<std::mutex> lock(client_name_mutex_);
    active_client_names_.erase(name);
    sessions_.erase(name);
}

void Server::RegisterSession(std::shared_ptr<ControlSession> session) {
    std::unique_lock<std::shared_mutex> lock(session_mutex_);
    // session registration is handled in AllocateClientName currently
}

void Server::UnregisterSession(std::shared_ptr<ControlSession> session) {
    std::unique_lock<std::shared_mutex> lock(session_mutex_);
}

std::vector<ClientInfo> Server::GetClientsInfo() const {
    std::lock_guard<std::mutex> lock(client_name_mutex_);
    std::vector<ClientInfo> infos;
    for (const auto& pair : sessions_) {
        if (pair.second) {
            infos.push_back(pair.second->GetInfo());
        }
    }
    return infos;
}

TrafficStats Server::GetTotalStats() const {
    std::lock_guard<std::mutex> lock(client_name_mutex_);
    TrafficStats total;
    uint64_t sent = 0;
    uint64_t received = 0;
    for (const auto& pair : sessions_) {
        if (pair.second) {
            ClientInfo info = pair.second->GetInfo();
            for (const auto& p : info.proxies) {
                sent += p.bytes_sent;
                received += p.bytes_received;
            }
        }
    }
    total.bytes_sent = sent;
    total.bytes_received = received;
    return total;
}

void Server::RemoveRateLimiter(const std::string& proxy_name) {
    std::unique_lock<std::shared_mutex> lock(rate_limit_mutex_);
    proxy_rate_limiters_.erase(proxy_name);
}

void Server::ClearPendingForProxy(const std::string& proxy_name) {
    std::lock_guard<std::mutex> lock(pending_conn_mutex_);
    for (auto it = pending_user_conns_.begin(); it != pending_user_conns_.end(); ) {
        if (it->second.proxy_name == proxy_name) {
            it = pending_user_conns_.erase(it); // tcp::socket dtor closes the fd
        } else {
            ++it;
        }
    }
    for (auto it = pending_udp_sessions_.begin(); it != pending_udp_sessions_.end(); ) {
        if (it->second.proxy_name == proxy_name) {
            it = pending_udp_sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void Server::DoAccept() {
    acceptor_.async_accept(
        [this](std::error_code ec, tcp::socket socket) {
            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    common::Logger::Error("Accept error: " + ec.message());
                    auto timer = std::make_shared<asio::steady_timer>(io_context_);
                    timer->expires_after(std::chrono::milliseconds(100));
                    timer->async_wait([this, timer](std::error_code) {
                        DoAccept();
                    });
                }
                return;
            }

            common::SetTcpKeepalive(socket);

            std::string peer_addr;
            {
                std::error_code peer_ec;
                auto ep = socket.remote_endpoint(peer_ec);
                if (!peer_ec) peer_addr = ep.address().to_string() + ":" + std::to_string(ep.port());
            }

            // Connection state helper to safely close either the raw socket 
            // or the wrapped stream when a timeout or error occurs.
            struct ConnectionState {
                std::shared_ptr<tcp::socket> socket;
                std::shared_ptr<common::AsyncStream> stream;
                std::mutex mutex;
                bool closed = false;

                void Close() {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (closed) return;
                    closed = true;
                    if (stream) {
                        stream->close();
                    } else if (socket) {
                        std::error_code ec;
                        socket->close(ec);
                    }
                }
            };

            auto state = std::make_shared<ConnectionState>();
            state->socket = std::make_shared<tcp::socket>(std::move(socket));

            auto handshake_timer = std::make_shared<asio::steady_timer>(io_context_);
            handshake_timer->expires_after(std::chrono::seconds(3));
            std::weak_ptr<ConnectionState> weak_state = state;
            handshake_timer->async_wait([weak_state, handshake_timer, peer_addr](std::error_code ec) {
                if (!ec) {
                    if (auto s = weak_state.lock()) {
                        common::Logger::Info("[Server] Handshake/Auth connection timeout from " + peer_addr + ". Closing connection.");
                        s->Close();
                    }
                }
            });

            auto start_handshake = [this, state, handshake_timer, peer_addr](std::shared_ptr<common::AsyncStream> stream) {
                if (protocol_ == "websocket") {
                    stream = std::make_shared<common::WebsocketStream>(stream, false, true, buffer_pool_);
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->stream = stream;
                    }
                }

                auto start_mux = [this, handshake_timer](std::shared_ptr<common::AsyncStream> s) {
                    handshake_timer->cancel();
                    auto mux_session = std::make_shared<common::mux::Session>(s, true, buffer_pool_);
                    std::weak_ptr<common::mux::Session> weak_mux = mux_session;

                    mux_session->start([this, weak_mux](std::shared_ptr<common::mux::MuxStream> new_stream) {
                        if (auto ms = weak_mux.lock()) {
                            HandleNewMuxStream(ms, new_stream);
                        }
                    });
                };

                stream->async_handshake(asio::ssl::stream_base::server, [this, stream, peer_addr, handshake_timer, start_mux](std::error_code ec) {
                    if (!ec) {
                        if (protocol_ == "auto") {
                            auto first_byte = std::make_shared<uint8_t>(0);
                            stream->async_read(asio::buffer(first_byte.get(), 1), [this, stream, first_byte, start_mux, handshake_timer](std::error_code ec, std::size_t) {
                                if (!ec) {
                                    std::shared_ptr<common::AsyncStream> buffered = std::make_shared<common::BufferedStream>(stream, std::vector<uint8_t>{*first_byte});
                                    if (*first_byte == 'G') { // 'G' from GET (WebSocket)
                                        auto ws_stream = std::make_shared<common::WebsocketStream>(buffered, false, false, buffer_pool_);
                                        ws_stream->async_handshake(asio::ssl::stream_base::server, [ws_stream, start_mux, handshake_timer](std::error_code ec) {
                                            if (!ec) {
                                                start_mux(ws_stream);
                                            } else {
                                                handshake_timer->cancel();
                                                ws_stream->close();
                                            }
                                        });
                                    } else {
                                        start_mux(buffered);
                                    }
                                } else {
                                    handshake_timer->cancel();
                                    stream->close();
                                }
                            });
                        } else if (protocol_ == "websocket") {
                            start_mux(stream);
                        } else {
                            start_mux(stream);
                        }
                    } else {
                        handshake_timer->cancel();
                        common::Logger::Error("[Server] TLS handshake failed from " + peer_addr + ": " + ec.message() + " (likely a non-TLS probe)");
                        stream->close();
                    }
                });
            };

            if (ssl_config_.enable) {
                auto first_byte = std::make_shared<uint8_t>(0);
                state->socket->async_receive(
                    asio::buffer(first_byte.get(), 1),
                    asio::socket_base::message_peek,
                    [this, state, first_byte, handshake_timer, start_handshake](std::error_code ec, std::size_t bytes_transferred) {
                        if (ec || bytes_transferred != 1) {
                            state->Close();
                            handshake_timer->cancel();
                            return;
                        }

                        if (*first_byte != 0x16) {
                            // Non-TLS connection: Silently drop connection without response
                            state->Close();
                            handshake_timer->cancel();
                            return;
                        }

                        std::shared_ptr<common::AsyncStream> stream;
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            if (state->closed) return;
                            stream = std::make_shared<common::SslStream>(std::move(*(state->socket)), *ssl_ctx_);
                            state->stream = stream;
                        }
                        start_handshake(stream);
                    }
                );
            } else {
                std::shared_ptr<common::AsyncStream> stream;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    stream = std::make_shared<common::TcpStream>(std::move(*(state->socket)));
                    state->stream = stream;
                }
                start_handshake(stream);
            }

            DoAccept();
        });
}

void Server::HandleNewMuxStream(std::shared_ptr<common::mux::Session> mux_session, std::shared_ptr<common::mux::MuxStream> stream) {
    if (stream->id() == 1) {
        common::Logger::Info("Control stream (ID 1) requested. Starting session...");
        auto control = std::make_shared<ControlSession>(*this, stream, io_context_, mux_session);
        {
            std::lock_guard<std::mutex> lock(client_name_mutex_);
            mux_to_control_[mux_session] = control;
        }

        // Add a 10s authentication timeout: if the client doesn't log in, close it.
        auto auth_timer = std::make_shared<asio::steady_timer>(io_context_);
        auth_timer->expires_after(std::chrono::seconds(10));
        std::weak_ptr<ControlSession> weak_control = control;
        auth_timer->async_wait([weak_control, auth_timer](std::error_code ec) {
            if (!ec) {
                if (auto c = weak_control.lock()) {
                    if (!c->is_authenticated()) {
                        common::Logger::Info("[Server] Authentication timeout for client from " + c->GetInfo().endpoint + ". Closing.");
                        c->Stop();
                    }
                }
            }
        });

        control->Start();
    } else {
        // Work connection
        auto ticket_ptr = std::make_shared<std::vector<uint8_t>>();
        ticket_ptr->resize(65);
        
        stream->async_read(asio::buffer(*ticket_ptr),
            [this, mux_session, stream, ticket_ptr](std::error_code ec, std::size_t) {
                if (!ec) {
                    bool use_compression = ((*ticket_ptr)[0] == 0x01);
                    std::string ticket(reinterpret_cast<char*>(ticket_ptr->data() + 1), 64);
                    ticket.erase(ticket.find_last_not_of(" \n\r\t") + 1);

                    std::shared_ptr<tcp::socket> user_socket;
                    std::shared_ptr<std::vector<uint8_t>> initial_data;
                    std::string proxy_name;
                    bool found_tcp = false;

                    std::shared_ptr<UdpProxyListener> udp_listener;
                    udp::endpoint udp_endpoint;
                    bool found_udp = false;

                    {
                        std::lock_guard<std::mutex> lock(pending_conn_mutex_);
                        auto it_tcp = pending_user_conns_.find(ticket);
                        if (it_tcp != pending_user_conns_.end()) {
                            initial_data = std::make_shared<std::vector<uint8_t>>(std::move(it_tcp->second.initial_data));
                            user_socket = std::make_shared<tcp::socket>(std::move(it_tcp->second.socket));
                            proxy_name = it_tcp->second.proxy_name;
                            pending_user_conns_.erase(it_tcp);
                            found_tcp = true;
                        } else {
                            auto it_udp = pending_udp_sessions_.find(ticket);
                            if (it_udp != pending_udp_sessions_.end()) {
                                udp_listener = it_udp->second.listener;
                                udp_endpoint = it_udp->second.endpoint;
                                proxy_name = it_udp->second.proxy_name;
                                udp_listener->RemoveEndpoint(udp_endpoint);
                                pending_udp_sessions_.erase(it_udp);
                                found_udp = true;
                            }
                        }
                    }

                    if (!found_tcp && !found_udp) {
                        common::Logger::Error("No pending connection/session for ticket: " + ticket);
                        stream->close();
                        return;
                    }

                    std::shared_ptr<ControlSession> control;
                    {
                        std::lock_guard<std::mutex> lock(client_name_mutex_);
                        auto it = mux_to_control_.find(mux_session);
                        if (it != mux_to_control_.end()) control = it->second;
                    }

                    std::shared_ptr<ProxyListener> tcp_pl;
                    std::shared_ptr<UdpProxyListener> udp_pl;
                    if (control) {
                        if (found_tcp) tcp_pl = control->FindProxy(proxy_name);
                        else udp_pl = control->FindUdpProxy(proxy_name);
                    }

                    std::shared_ptr<common::RateLimiter> rl;
                    {
                        std::shared_lock<std::shared_mutex> lock(rate_limit_mutex_);
                        auto it_rl = proxy_rate_limiters_.find(proxy_name);
                        if (it_rl != proxy_rate_limiters_.end()) rl = it_rl->second;
                    }

                    if (found_tcp) {
                        common::Logger::Info("Splicing user TCP connection and mux work stream for ticket: " + ticket + " (Compressed: " + (use_compression ? "true" : "false") + ")");
                        
                        auto user_stream = std::make_shared<common::TcpStream>(std::move(*user_socket));
                        auto bridge = std::make_shared<common::Bridge>(user_stream, stream, use_compression, 1, rl, buffer_pool_);
                        if (tcp_pl) {
                            bridge->SetStatsCounters(&tcp_pl->stats().bytes_sent, &tcp_pl->stats().bytes_received);
                            struct Cleanup {
                                std::weak_ptr<ProxyListener> pl_weak;
                                ~Cleanup() { 
                                    if (auto pl = pl_weak.lock()) pl->dec_active_conns(); 
                                }
                            };
                            auto cleanup = std::make_shared<Cleanup>();
                            cleanup->pl_weak = tcp_pl;
                            bridge->SetOnStop([cleanup]() { cleanup->pl_weak.reset(); });
                        }

                        if (!initial_data->empty()) {
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
                            }

                            auto packet = std::make_shared<std::vector<uint8_t>>();
                            packet->resize(sizeof(final_header) + write_len);
                            std::memcpy(packet->data(), &final_header, sizeof(final_header));
                            std::memcpy(packet->data() + sizeof(final_header), write_buf, write_len);

                            stream->async_write(asio::buffer(*packet), [this, bridge, packet, tcp_pl](std::error_code ec, std::size_t) {
                                if (!ec) {
                                    bridge->Start();
                                } else {
                                    if (tcp_pl) tcp_pl->dec_active_conns();
                                }
                            });
                        } else {
                            bridge->Start();
                        }
                    } else if (found_udp) {
                        common::Logger::Info("Splicing user UDP session and mux work stream for ticket: " + ticket + " (Compressed: " + (use_compression ? "true" : "false") + ")");
                        auto bridge = std::make_shared<UdpBridge>(io_context_, stream, udp_listener->socket(), udp_endpoint, use_compression, rl, buffer_pool_);
                        if (udp_pl) {
                            bridge->SetStatsCounters(&udp_pl->stats().bytes_sent, &udp_pl->stats().bytes_received);
                        }
                        bridge->Start();
                    }
                }
            });
    }
}

void Server::RemoveMuxControl(std::shared_ptr<common::mux::Session> mux_session) {
    std::lock_guard<std::mutex> lock(client_name_mutex_);
    mux_to_control_.erase(mux_session);
}

void Server::AddVhostRoute(const std::string& domain, std::shared_ptr<ControlSession> session, const std::string& proxy_name, const std::string& type) {
    std::unique_lock<std::shared_mutex> lock(vhost_mutex_);
    vhost_routes_[domain] = {session, proxy_name, type};
    common::Logger::Info("[Server] Added vhost route: " + domain + " -> " + proxy_name + " (" + type + ")");
}

void Server::RemoveVhostRoute(const std::string& domain) {
    std::unique_lock<std::shared_mutex> lock(vhost_mutex_);
    vhost_routes_.erase(domain);
}

std::shared_ptr<common::RateLimiter> Server::GetRateLimiter(const std::string& proxy_name) {
    std::shared_lock<std::shared_mutex> lock(rate_limit_mutex_);
    auto it = proxy_rate_limiters_.find(proxy_name);
    if (it != proxy_rate_limiters_.end()) return it->second;
    return nullptr;
}

void Server::CreateRateLimiter(const std::string& proxy_name, int64_t bytes_per_sec) {
    std::unique_lock<std::shared_mutex> lock(rate_limit_mutex_);
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
            
            auto read_timer = std::make_shared<asio::steady_timer>(io_context_);
            read_timer->expires_after(std::chrono::seconds(10));
            std::weak_ptr<tcp::socket> weak_socket = socket;
            read_timer->async_wait([weak_socket, read_timer](std::error_code ec) {
                if (!ec) {
                    if (auto s = weak_socket.lock()) {
                        common::Logger::Info("[Server] Vhost HTTP/HTTPS read timeout. Closing connection.");
                        std::error_code close_ec;
                        s->close(close_ec);
                    }
                }
            });

            socket->async_read_some(asio::buffer(*buffer), [this, socket, buffer, type, read_timer](std::error_code ec, std::size_t length) {
                read_timer->cancel();
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
                        std::shared_lock<std::shared_mutex> lock(vhost_mutex_);
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
            DoVhostAccept(acceptor, type);
        } else {
            if (ec != asio::error::operation_aborted) {
                common::Logger::Error("Vhost accept error (" + type + "): " + ec.message());
                auto timer = std::make_shared<asio::steady_timer>(io_context_);
                timer->expires_after(std::chrono::milliseconds(100));
                timer->async_wait([this, &acceptor, type, timer](std::error_code) {
                    DoVhostAccept(acceptor, type);
                });
            }
        }
    });
}

void Server::DoLazyCleanup() {
    auto now = std::chrono::steady_clock::now();
    while (!ticket_expiration_queue_.empty()) {
        auto& oldest = ticket_expiration_queue_.front();
        if (now >= oldest.expires_at) {
            if (oldest.is_udp) {
                auto it = pending_udp_sessions_.find(oldest.ticket);
                if (it != pending_udp_sessions_.end()) {
                    it->second.listener->RemoveEndpoint(it->second.endpoint);
                    pending_udp_sessions_.erase(it);
                }
            } else {
                pending_user_conns_.erase(oldest.ticket);
            }
            ticket_expiration_queue_.pop_front();
        } else {
            break; // Since queue is chronological, others are not expired
        }
    }
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
    return allowed_clients_.find(name) != allowed_clients_.end();
}

} // namespace server
} // namespace cfrp
