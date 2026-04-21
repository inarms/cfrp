#include "client.h"
#include <iostream>
#include <zstd.h>

namespace cfrp {
namespace client {

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
        auto& to = (direction == 1) ? s2_ : s1_;
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
UdpBridge::UdpBridge(asio::io_context& io_context, std::shared_ptr<common::AsyncStream> stream, udp::endpoint local_endpoint, bool use_compression)
    : timer_(io_context), stream_(std::move(stream)), socket_(io_context, udp::endpoint(udp::v4(), 0)), local_endpoint_(local_endpoint), use_compression_(use_compression) {
    read_buf_.resize(65535);
}

void UdpBridge::Start() {
    StartTimer();
    DoReadFromStream();
    DoReadFromLocal();
}

void UdpBridge::StartTimer() {
    timer_.expires_after(std::chrono::seconds(60));
    auto self(shared_from_this());
    timer_.async_wait([this, self](std::error_code ec) {
        if (!ec) {
            std::cout << "UDP bridge timed out" << std::endl;
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

                            socket_.async_send_to(asio::buffer(send_buf, send_len), local_endpoint_,
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

void UdpBridge::DoReadFromLocal() {
    auto self(shared_from_this());
    socket_.async_receive_from(asio::buffer(local_recv_buf_, sizeof(local_recv_buf_)), local_endpoint_,
        [this, self](std::error_code ec, std::size_t length) {
            if (!ec) {
                ResetTimer();
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

                stream_->async_write(asio::buffer(*buf), [this, self, buf](std::error_code ec, std::size_t) {
                    if (!ec) {
                        DoReadFromLocal();
                    } else {
                        stream_->close();
                    }
                });
            } else {
                stream_->close();
            }
        });
}

// --- Client ---
Client::Client(const std::string& server_addr, uint16_t server_port, const std::string& token, const std::string& name, const SslConfig& ssl_config, bool compression)
    : server_addr_(server_addr),
      server_port_(server_port),
      token_(token),
      name_(name),
      ssl_config_(ssl_config),
      compression_(compression),
      endpoint_(asio::ip::make_address(server_addr), server_port),
      reconnect_timer_(io_context_) {
    
    if (ssl_config_.enable) {
        ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
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
        std::cout << "SSL enabled on client." << std::endl;
    }
    
    std::cout << "Client initialized (TCP Mux Enabled). Target: " << server_addr << ":" << server_port << std::endl;
}

void Client::Run() {
    DoConnect();
    io_context_.run();
}

void Client::AddProxy(const ProxyConfig& proxy) {
    proxies_.push_back(proxy);
}

void Client::DoConnect() {
    std::cout << "Connecting to server " << server_addr_ << ":" << server_port_ << "..." << std::endl;
    
    tcp::socket socket(io_context_);
    auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
    
    socket_ptr->async_connect(endpoint_,
        [this, socket_ptr](std::error_code ec) {
            if (!ec) {
                std::shared_ptr<common::AsyncStream> stream;
                if (ssl_config_.enable) {
                    stream = std::make_shared<common::SslStream>(std::move(*socket_ptr), *ssl_ctx_);
                } else {
                    stream = std::make_shared<common::TcpStream>(std::move(*socket_ptr));
                }
                
                stream->async_handshake(asio::ssl::stream_base::client, [this, stream](std::error_code ec) {
                    OnConnect(ec, stream);
                });
            } else {
                HandleDisconnect("Connect failed: " + ec.message());
            }
        });
}

void Client::OnConnect(const std::error_code& ec, std::shared_ptr<common::AsyncStream> underlying_stream) {
    if (!ec) {
        std::cout << "Connected to server. Initializing MuxSession..." << std::endl;
        reconnect_delay_sec_ = 0;
        
        mux_session_ = std::make_shared<common::mux::Session>(underlying_stream, false);
        mux_session_->start([](std::shared_ptr<common::mux::MuxStream>) {
            // Client doesn't expect server to open streams in this model
        });
        
        std::cout << "MuxSession initialized. Opening control stream..." << std::endl;
        control_stream_ = mux_session_->open_stream();
        std::cout << "Control stream opened. Sending login request..." << std::endl;
        DoLogin();
        DoReadHeader();
    } else {
        HandleDisconnect("Handshake/Connect failed: " + ec.message());
    }
}

void Client::HandleDisconnect(const std::string& reason) {
    std::cout << reason << std::endl;
    if (mux_session_) {
        mux_session_->stop();
        mux_session_.reset();
    }
    ScheduleReconnect();
}

void Client::ScheduleReconnect() {
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

void Client::SendMessage(protocol::MessageType type, const protocol::json& body) {
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
    
    auto data = std::make_shared<std::vector<uint8_t>>();
    data->resize(sizeof(final_len) + to_send_body.size());
    std::memcpy(data->data(), &final_len, sizeof(final_len));
    std::memcpy(data->data() + sizeof(final_len), to_send_body.data(), to_send_body.size());
    
    control_stream_->async_write(asio::buffer(*data), [this, data](std::error_code ec, std::size_t) {
        if (ec) {
            std::cerr << "Failed to send message: " << ec.message() << std::endl;
        }
    });
}

void Client::DoReadHeader() {
    control_stream_->async_read(asio::buffer(&header_, sizeof(header_)),
        [this](std::error_code ec, std::size_t) {
            if (!ec) {
                uint32_t length = header_.body_length & protocol::LENGTH_MASK;
                DoReadBody(length);
            } else {
                HandleDisconnect("Control stream closed: " + ec.message());
            }
        });
}

void Client::DoReadBody(uint32_t length) {
    bool is_compressed = (header_.body_length & protocol::COMPRESSION_FLAG) != 0;
    body_data_.resize(length);
    control_stream_->async_read(asio::buffer(body_data_),
        [this, is_compressed](std::error_code ec, std::size_t) {
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
                HandleDisconnect("Control stream closed: " + ec.message());
            }
        });
}

void Client::DoLogin() {
    protocol::json body;
    body["token"] = token_;
    body["name"] = name_;
    SendMessage(protocol::MessageType::Login, body);
}

void Client::HandleMessage(const protocol::Message& msg) {
    if (msg.type == protocol::MessageType::LoginResp) {
        if (msg.body["status"] == "ok") {
            std::string assigned_name = msg.body.value("name", "");
            if (!assigned_name.empty()) {
                name_ = assigned_name;
            }
            std::cout << "Authenticated successfully as [" << name_ << "]" << std::endl;
            RegisterProxies();
        } else {
            std::cerr << "Authentication failed: " << msg.body.value("message", "unknown error") << std::endl;
        }
    } else if (msg.type == protocol::MessageType::RegisterProxyResp) {
        std::cout << "Proxy registration response: " << msg.body["status"] << " for " << msg.body["name"] << std::endl;
    } else if (msg.type == protocol::MessageType::NewUserConn) {
        std::string proxy_name = msg.body["proxy_name"];
        std::string ticket = msg.body["ticket"];
        HandleNewUserConn(proxy_name, ticket);
    }
}

void Client::RegisterProxies() {
    for (const auto& proxy : proxies_) {
        protocol::json body;
        body["name"] = proxy.name;
        body["type"] = proxy.type;
        body["remote_port"] = proxy.remote_port;
        SendMessage(protocol::MessageType::RegisterProxy, body);
    }
}

void Client::HandleNewUserConn(const std::string& proxy_name, const std::string& ticket) {
    auto it = std::find_if(proxies_.begin(), proxies_.end(), [&](const ProxyConfig& pc) {
        return pc.name == proxy_name;
    });

    if (it == proxies_.end()) {
        std::cerr << "Unknown proxy name: " << proxy_name << std::endl;
        return;
    }

    if (it->type == "udp") {
        HandleNewUdpUserConn(*it, ticket);
        return;
    }

    auto pc = *it;
    auto local_socket = std::make_shared<tcp::socket>(io_context_);
    
    local_socket->async_connect(tcp::endpoint(asio::ip::make_address(pc.local_ip), pc.local_port),
        [this, local_socket, ticket, pc](std::error_code ec) {
            if (!ec) {
                auto work_stream = mux_session_->open_stream();
                
                auto ticket_buf = std::make_shared<std::vector<uint8_t>>();
                ticket_buf->push_back(compression_ ? 0x01 : 0x00);
                ticket_buf->insert(ticket_buf->end(), ticket.begin(), ticket.end());
                ticket_buf->resize(65, ' ');
                
                work_stream->async_write(asio::buffer(*ticket_buf),
                    [this, local_socket, work_stream, ticket_buf, pc](std::error_code ec, std::size_t) {
                        if (!ec) {
                            std::cout << "Bridging local service and mux work stream (Compressed: " << compression_ << ")" << std::endl;
                            auto user_stream = std::make_shared<common::TcpStream>(std::move(*local_socket));
                            auto bridge = std::make_shared<Bridge>(user_stream, work_stream, compression_);
                            bridge->Start();
                        } else {
                            std::cerr << "Failed to send ticket over mux stream" << std::endl;
                        }
                    });
            } else {
                std::cerr << "Failed to connect to local service (" << pc.local_ip << ":" << pc.local_port << "): " << ec.message() << std::endl;
            }
        });
}

void Client::HandleNewUdpUserConn(const ProxyConfig& pc, const std::string& ticket) {
    auto work_stream = mux_session_->open_stream();
    
    auto ticket_buf = std::make_shared<std::vector<uint8_t>>();
    ticket_buf->push_back(compression_ ? 0x01 : 0x00);
    ticket_buf->insert(ticket_buf->end(), ticket.begin(), ticket.end());
    ticket_buf->resize(65, ' ');
    
    work_stream->async_write(asio::buffer(*ticket_buf),
        [this, work_stream, ticket_buf, pc](std::error_code ec, std::size_t) {
            if (!ec) {
                std::cout << "Bridging local UDP service and mux work stream (Compressed: " << compression_ << ")" << std::endl;
                auto bridge = std::make_shared<UdpBridge>(io_context_, work_stream, udp::endpoint(asio::ip::make_address(pc.local_ip), pc.local_port), compression_);
                bridge->Start();
            } else {
                std::cerr << "Failed to send ticket over mux stream for UDP" << std::endl;
            }
        });
}

} // namespace client
} // namespace cfrp
