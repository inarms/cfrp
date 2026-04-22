#include "server.h"
#include "common/quic_ngtcp2.h"
#include <iostream>
#include <chrono>
#include <zstd.h>
#include <sstream>

namespace cfrp {
namespace server {

// --- Bridge ---
Bridge::Bridge(std::shared_ptr<common::AsyncStream> s1, std::shared_ptr<common::AsyncStream> s2, bool use_compression)
    : s1_(std::move(s1)), s2_(std::move(s2)), use_compression_(use_compression) {}

void Bridge::Start() {
    DoRead(1);
    DoRead(2);
}

void Bridge::DoRead(int direction) {
    auto self(shared_from_this());
    if (!use_compression_) {
        auto& from = (direction == 1) ? s1_ : s2_;
        auto& buf = (direction == 1) ? data1_ : data2_;

        from->async_read_some(asio::buffer(buf, sizeof(data1_)),
            [this, self, direction](std::error_code ec, std::size_t length) {
                if (!ec) {
                    auto& to_inner = (direction == 1) ? s2_ : s1_;
                    auto& buf_inner = (direction == 1) ? data1_ : data2_;
                    to_inner->async_write(asio::buffer(buf_inner, length),
                        [this, self, direction](std::error_code ec, std::size_t) {
                            if (!ec) {
                                DoRead(direction);
                            }
                        });
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
                            final_header = static_cast<uint32_t>(cSize) | protocol::COMPRESSION_FLAG;
                            write_buf = compressed.data();
                            write_len = cSize;
                        } else {
                            final_header = static_cast<uint32_t>(length);
                            write_buf = data1_;
                            write_len = length;
                        }

                        auto packet = std::make_shared<std::vector<uint8_t>>();
                        packet->resize(sizeof(final_header) + write_len);
                        std::memcpy(packet->data(), &final_header, sizeof(final_header));
                        std::memcpy(packet->data() + sizeof(final_header), write_buf, write_len);

                        s2_->async_write(asio::buffer(*packet), [this, self, packet](std::error_code ec, std::size_t) {
                            if (!ec) {
                                DoRead(1);
                            }
                        });
                    }
                });
        } else { // Work -> Raw (Unframe & Decompress)
            s2_->async_read(asio::buffer(&header2_, sizeof(header2_)),
                [this, self](std::error_code ec, std::size_t) {
                    if (!ec) {
                        uint32_t len = header2_ & protocol::LENGTH_MASK;
                        bool compressed = (header2_ & protocol::COMPRESSION_FLAG) != 0;
                        auto body = std::make_shared<std::vector<uint8_t>>(len);
                        s2_->async_read(asio::buffer(*body), [this, self, body, compressed](std::error_code ec, std::size_t) {
                            if (!ec) {
                                if (compressed) {
                                    unsigned long long const decodedSize = ZSTD_getFrameContentSize(body->data(), body->size());
                                    std::vector<uint8_t> decompressed(decodedSize);
                                    ZSTD_decompress(decompressed.data(), decodedSize, body->data(), body->size());
                                    auto d_buf = std::make_shared<std::vector<uint8_t>>(std::move(decompressed));
                                    s1_->async_write(asio::buffer(*d_buf), [this, self, d_buf](std::error_code ec, std::size_t) {
                                        if (!ec) DoRead(2);
                                    });
                                } else {
                                    s1_->async_write(asio::buffer(*body), [this, self, body](std::error_code ec, std::size_t) {
                                        if (!ec) DoRead(2);
                                    });
                                }
                            }
                        });
                    }
                });
        }
    }
}

// --- UdpBridge ---
UdpBridge::UdpBridge(asio::io_context& io_context, std::shared_ptr<common::AsyncStream> stream, udp::socket& socket, udp::endpoint remote_endpoint, bool use_compression)
    : timer_(io_context), stream_(std::move(stream)), socket_(socket), remote_endpoint_(remote_endpoint), use_compression_(use_compression) {
    read_buf_.resize(65535);
}

void UdpBridge::Start() {
    StartTimer();
    DoReadFromStream();
}

void UdpBridge::StartTimer() {
    timer_.expires_after(std::chrono::seconds(60));
    auto self(shared_from_this());
    timer_.async_wait([this, self](std::error_code ec) {
        if (!ec) {
            std::cout << "UDP session timed out for " << remote_endpoint_ << std::endl;
            stream_->close();
        }
    });
}

void UdpBridge::ResetTimer() {
    timer_.expires_after(std::chrono::seconds(60));
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
                            ResetTimer();
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

                            socket_.async_send_to(asio::buffer(send_buf, send_len), remote_endpoint_,
                                [this, self](std::error_code ec, std::size_t) {
                                    if (!ec) {
                                        DoReadFromStream();
                                    }
                                });
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

    stream_->async_write(asio::buffer(*buf), [this, self, buf](std::error_code ec, std::size_t) {
        if (!ec) {
            ResetTimer();
        } else {
            stream_->close();
        }
    });
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
    std::cout << "UDP Proxy listener started for [" << proxy_name << "] on port " << port << std::endl;
}

void UdpProxyListener::Start() {
    DoReceive();
}

void UdpProxyListener::Stop() {
    std::error_code ec;
    socket_.close(ec);
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
                    
                    server_.RegisterUdpSession(ticket, shared_from_this(), sender_endpoint_);

                    if (auto session = session_.lock()) {
                        protocol::json body;
                        body["proxy_name"] = proxy_name_;
                        body["ticket"] = ticket;
                        session->SendMessage(protocol::MessageType::NewUserConn, body);
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
    std::cout << "Proxy listener started for [" << proxy_name << "] on port " << port << std::endl;
}

void ProxyListener::Start() {
    DoAccept();
}

void ProxyListener::Stop() {
    std::cout << "Stopping proxy listener for [" << proxy_name_ << "]" << std::endl;
    std::error_code ec;
    acceptor_.close(ec);
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

        std::string ticket = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::cout << "New user connection for proxy [" << proxy_name_ << "], ticket: " << ticket << std::endl;
        
        server_.RegisterUserConn(ticket, std::move(socket));

        if (auto session = session_.lock()) {
            protocol::json body;
            body["proxy_name"] = proxy_name_;
            body["ticket"] = ticket;
            session->SendMessage(protocol::MessageType::NewUserConn, body);
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
    stream_->close();
}

void ControlSession::SendMessage(protocol::MessageType type, const protocol::json& body) {
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
    
    auto data = std::make_shared<std::vector<uint8_t>>();
    data->resize(sizeof(final_len) + to_send_body.size());
    std::memcpy(data->data(), &final_len, sizeof(final_len));
    std::memcpy(data->data() + sizeof(final_len), to_send_body.data(), to_send_body.size());

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
                uint32_t length = header_.body_length & protocol::LENGTH_MASK;
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
        std::string name = msg.body["name"];
        uint16_t remote_port = msg.body["remote_port"];
        std::string type = msg.body.value("type", "tcp");
        
        try {
            if (type == "udp") {
                auto listener = std::make_shared<UdpProxyListener>(server_, io_context_, remote_port, shared_from_this(), name);
                listener->Start();
                udp_proxies_.push_back(listener);
            } else {
                auto listener = std::make_shared<ProxyListener>(server_, io_context_, remote_port, shared_from_this(), name);
                listener->Start();
                proxies_.push_back(listener);
            }
            
            protocol::json resp;
            resp["status"] = "ok";
            resp["name"] = name;
            SendMessage(protocol::MessageType::RegisterProxyResp, resp);
        } catch (const std::exception& e) {
            std::cerr << "Failed to start proxy listener: " << e.what() << std::endl;
            protocol::json resp;
            resp["status"] = "error";
            resp["message"] = e.what();
            SendMessage(protocol::MessageType::RegisterProxyResp, resp);
        }
    } else if (msg.type == protocol::MessageType::UnregisterProxy) {
        std::string name = msg.body["name"];
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

void ControlSession::HandleLogin(const protocol::json& body) {
    std::string token = body.value("token", "");
    std::string requested_name = body.value("name", "");
    protocol::json resp;
    if (token == server_.GetToken()) {
        client_name_ = server_.AllocateClientName(requested_name);
        std::cout << "[Server] Client authenticated successfully (" << stream_->protocol_name() << "): " 
                  << client_endpoint_ << " as [" << client_name_ << "]" << std::endl;
        std::cout << "[Server] Client [" << client_name_ << "] is READY." << std::endl;
        authenticated_ = true;
        resp["status"] = "ok";
        resp["name"] = client_name_;
    } else {
        std::cout << "Client authentication failed." << std::endl;
        resp["status"] = "error";
        resp["message"] = "Invalid token";
        SendMessage(protocol::MessageType::LoginResp, resp);
        Stop();
        return;
    }
    SendMessage(protocol::MessageType::LoginResp, resp);
}

// --- Server ---
Server::Server(asio::io_context& io_context, const std::string& bind_addr, uint16_t bind_port, const std::string& token, const SslConfig& ssl_config, const std::string& protocol)
    : io_context_(io_context),
      acceptor_(io_context_, tcp::endpoint(asio::ip::make_address(bind_addr), bind_port)),
      udp_socket_(io_context_, udp::endpoint(asio::ip::make_address(bind_addr), bind_port)),
      token_(token),
      protocol_(protocol),
      ssl_config_(ssl_config) {
    
    if (ssl_config_.enable || protocol_ == "quic") {
        ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
        ssl_ctx_->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::single_dh_use);
        
        std::string cert = ssl_config_.cert_file.empty() ? "server.crt" : ssl_config_.cert_file;
        std::string key = ssl_config_.key_file.empty() ? "server.key" : ssl_config_.key_file;

        try {
            ssl_ctx_->use_certificate_chain_file(cert);
            ssl_ctx_->use_private_key_file(key, asio::ssl::context::pem);
            std::cout << "SSL/QUIC certificate loaded: " << cert << std::endl;
        } catch (const std::exception& e) {
            if (protocol_ == "quic") {
                std::cerr << "Warning: Failed to load certificate for QUIC: " << e.what() << std::endl;
                std::cerr << "QUIC requires a certificate to function. Please check config_server.toml" << std::endl;
            }
        }
    }
    
    std::cout << "Server initialized on " << bind_addr << ":" << bind_port << " (" << protocol_ << " Mux Enabled)" << std::endl;
}

void Server::Run() {
    std::cout << "Starting cfrp server loop..." << std::endl;
    DoAccept();
    if (protocol_ == "quic") {
        DoUdpRead();
    }
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

                    session->init(ssl_ctx_->native_handle(), &n_dcid, &n_scid);
                    quic_sessions_[*endpoint] = session;
                    it = quic_sessions_.find(*endpoint);
                }
                it->second->handle_packet(udp_recv_buf_, length);
                DoUdpRead();
            }
        });
}

void Server::RegisterUserConn(const std::string& ticket, tcp::socket socket) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    pending_user_conns_.emplace(ticket, std::move(socket));
}

void Server::RegisterUdpSession(const std::string& ticket, std::shared_ptr<UdpProxyListener> listener, udp::endpoint endpoint) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    pending_udp_sessions_.emplace(ticket, UdpSessionInfo{listener, endpoint});
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
            
            std::shared_ptr<common::AsyncStream> stream;
            if (ssl_config_.enable) {
                stream = std::make_shared<common::SslStream>(std::move(socket), *ssl_ctx_);
            } else {
                stream = std::make_shared<common::TcpStream>(std::move(socket));
            }
            
            stream->async_handshake(asio::ssl::stream_base::server, [this, stream](std::error_code ec) {
                if (!ec) {
                    auto mux_session = std::make_shared<common::mux::Session>(stream, true);
                    mux_session->start([this, mux_session](std::shared_ptr<common::mux::MuxStream> new_stream) {
                        HandleNewMuxStream(mux_session, new_stream);
                    });
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
                        auto user_stream = std::make_shared<common::TcpStream>(std::move(it_tcp->second));
                        auto bridge = std::make_shared<Bridge>(user_stream, stream, use_compression);
                        bridge->Start();
                        pending_user_conns_.erase(it_tcp);
                    } else {
                        auto it_udp = pending_udp_sessions_.find(ticket);
                        if (it_udp != pending_udp_sessions_.end()) {
                            std::cout << "Splicing user UDP session and mux work stream for ticket: " << ticket << " (Compressed: " << use_compression << ")" << std::endl;
                            auto bridge = std::make_shared<UdpBridge>(io_context_, stream, it_udp->second.listener->socket(), it_udp->second.endpoint, use_compression);
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

} // namespace server
} // namespace cfrp
