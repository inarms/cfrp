#include "client.h"
#include <iostream>

namespace cfrp {
namespace client {

// --- Bridge ---
Bridge::Bridge(std::shared_ptr<common::AsyncStream> s1, std::shared_ptr<common::AsyncStream> s2)
    : s1_(std::move(s1)), s2_(std::move(s2)) {}

void Bridge::Start() {
    DoRead(1);
    DoRead(2);
}

void Bridge::DoRead(int direction) {
    auto self(shared_from_this());
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
}

// --- UdpBridge ---
UdpBridge::UdpBridge(asio::io_context& io_context, std::shared_ptr<common::AsyncStream> stream, udp::endpoint local_endpoint)
    : timer_(io_context), stream_(std::move(stream)), socket_(io_context, udp::endpoint(udp::v4(), 0)), local_endpoint_(local_endpoint) {
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
                uint16_t len = asio::detail::socket_ops::network_to_host_short(packet_len_);
                if (len > read_buf_.size()) {
                    stream_->close();
                    return;
                }
                stream_->async_read(asio::buffer(read_buf_.data(), len),
                    [this, self, len](std::error_code ec, std::size_t) {
                        if (!ec) {
                            ResetTimer();
                            socket_.async_send_to(asio::buffer(read_buf_.data(), len), local_endpoint_,
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
                uint16_t len = asio::detail::socket_ops::host_to_network_short(static_cast<uint16_t>(length));
                buf->resize(sizeof(len) + length);
                std::memcpy(buf->data(), &len, sizeof(len));
                std::memcpy(buf->data() + sizeof(len), local_recv_buf_, length);

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
Client::Client(const std::string& server_addr, uint16_t server_port, const std::string& token, const SslConfig& ssl_config)
    : server_addr_(server_addr),
      server_port_(server_port),
      token_(token),
      ssl_config_(ssl_config),
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
    
    std::cout << "Client initialized. Target server: " << server_addr << ":" << server_port << std::endl;
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
                if (ssl_config_.enable) {
                    stream_ = std::make_shared<common::SslStream>(std::move(*socket_ptr), *ssl_ctx_);
                } else {
                    stream_ = std::make_shared<common::TcpStream>(std::move(*socket_ptr));
                }
                
                stream_->async_handshake(asio::ssl::stream_base::client, [this](std::error_code ec) {
                    OnConnect(ec);
                });
            } else {
                HandleDisconnect("Connect failed: " + ec.message());
            }
        });
}

void Client::OnConnect(const std::error_code& ec) {
    if (!ec) {
        std::cout << "Connected to server (Secure)." << std::endl;
        reconnect_delay_sec_ = 0; // Reset delay on success
        DoLogin();
        DoReadHeader();
    } else {
        HandleDisconnect("Handshake/Connect failed: " + ec.message());
    }
}

void Client::HandleDisconnect(const std::string& reason) {
    std::cout << reason << std::endl;
    if (stream_) {
        stream_->close();
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
    uint32_t len = static_cast<uint32_t>(encoded.size());
    
    auto data = std::make_shared<std::vector<uint8_t>>();
    data->resize(sizeof(len) + encoded.size());
    std::memcpy(data->data(), &len, sizeof(len));
    std::memcpy(data->data() + sizeof(len), encoded.data(), encoded.size());
    
    stream_->async_write(asio::buffer(*data), [this, data](std::error_code ec, std::size_t) {
        if (ec) {
            std::cerr << "Failed to send message: " << ec.message() << std::endl;
        }
    });
}

void Client::DoReadHeader() {
    stream_->async_read(asio::buffer(&header_, sizeof(header_)),
        [this](std::error_code ec, std::size_t) {
            if (!ec) {
                DoReadBody(header_.body_length);
            } else {
                HandleDisconnect("Connection to server closed: " + ec.message());
            }
        });
}

void Client::DoReadBody(uint32_t length) {
    body_data_.resize(length);
    stream_->async_read(asio::buffer(body_data_),
        [this](std::error_code ec, std::size_t) {
            if (!ec) {
                try {
                    std::vector<uint8_t> data(body_data_.begin(), body_data_.end());
                    auto msg = protocol::Message::Decode(data);
                    HandleMessage(msg);
                } catch (const std::exception& e) {
                    std::cerr << "Failed to decode message: " << e.what() << std::endl;
                }
                DoReadHeader();
            } else {
                HandleDisconnect("Connection to server closed: " + ec.message());
            }
        });
}

void Client::DoLogin() {
    protocol::json body;
    body["token"] = token_;
    SendMessage(protocol::MessageType::Login, body);
}

void Client::HandleMessage(const protocol::Message& msg) {
    if (msg.type == protocol::MessageType::LoginResp) {
        if (msg.body["status"] == "ok") {
            std::cout << "Authenticated successfully." << std::endl;
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
    auto work_socket_raw = std::make_shared<tcp::socket>(io_context_);

    local_socket->async_connect(tcp::endpoint(asio::ip::make_address(pc.local_ip), pc.local_port),
        [this, local_socket, work_socket_raw, ticket, pc](std::error_code ec) {
            if (!ec) {
                work_socket_raw->async_connect(tcp::endpoint(asio::ip::make_address(server_addr_), server_port_ + 1),
                    [this, local_socket, work_socket_raw, ticket, pc](std::error_code ec) {
                        if (!ec) {
                            std::shared_ptr<common::AsyncStream> work_stream;
                            if (ssl_config_.enable) {
                                work_stream = std::make_shared<common::SslStream>(std::move(*work_socket_raw), *ssl_ctx_);
                            } else {
                                work_stream = std::make_shared<common::TcpStream>(std::move(*work_socket_raw));
                            }
                            
                            work_stream->async_handshake(asio::ssl::stream_base::client, [this, local_socket, work_stream, ticket, pc](std::error_code ec) {
                                if (!ec) {
                                    auto ticket_buf = std::make_shared<std::string>(ticket);
                                    ticket_buf->resize(64, ' ');
                                    
                                    work_stream->async_write(asio::buffer(*ticket_buf),
                                        [local_socket, work_stream, ticket_buf](std::error_code ec, std::size_t) {
                                            if (!ec) {
                                                std::cout << "Bridging local service and server work connection" << std::endl;
                                                auto user_stream = std::make_shared<common::TcpStream>(std::move(*local_socket));
                                                auto bridge = std::make_shared<Bridge>(user_stream, work_stream);
                                                bridge->Start();
                                            }
                                        });
                                } else {
                                    std::cerr << "SSL handshake failed for work connection: " << ec.message() << std::endl;
                                }
                            });
                        } else {
                            std::cerr << "Failed to connect to server work port: " << ec.message() << std::endl;
                        }
                    });
            } else {
                std::cerr << "Failed to connect to local service (" << pc.local_ip << ":" << pc.local_port << "): " << ec.message() << std::endl;
            }
        });
}

void Client::HandleNewUdpUserConn(const ProxyConfig& pc, const std::string& ticket) {
    auto work_socket_raw = std::make_shared<tcp::socket>(io_context_);

    work_socket_raw->async_connect(tcp::endpoint(asio::ip::make_address(server_addr_), server_port_ + 1),
        [this, work_socket_raw, ticket, pc](std::error_code ec) {
            if (!ec) {
                std::shared_ptr<common::AsyncStream> work_stream;
                if (ssl_config_.enable) {
                    work_stream = std::make_shared<common::SslStream>(std::move(*work_socket_raw), *ssl_ctx_);
                } else {
                    work_stream = std::make_shared<common::TcpStream>(std::move(*work_socket_raw));
                }
                
                work_stream->async_handshake(asio::ssl::stream_base::client, [this, work_stream, ticket, pc](std::error_code ec) {
                    if (!ec) {
                        auto ticket_buf = std::make_shared<std::string>(ticket);
                        ticket_buf->resize(64, ' ');
                        
                        work_stream->async_write(asio::buffer(*ticket_buf),
                            [this, work_stream, ticket_buf, pc](std::error_code ec, std::size_t) {
                                if (!ec) {
                                    std::cout << "Bridging local UDP service and server work connection" << std::endl;
                                    auto bridge = std::make_shared<UdpBridge>(io_context_, work_stream, udp::endpoint(asio::ip::make_address(pc.local_ip), pc.local_port));
                                    bridge->Start();
                                }
                            });
                    } else {
                        std::cerr << "SSL handshake failed for UDP work connection: " << ec.message() << std::endl;
                    }
                });
            } else {
                std::cerr << "Failed to connect to server work port for UDP: " << ec.message() << std::endl;
            }
        });
}

} // namespace client
} // namespace cfrp
