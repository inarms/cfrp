#include "client.h"
#include <iostream>

namespace cfrp {
namespace client {

// --- Bridge ---
Bridge::Bridge(tcp::socket s1, tcp::socket s2)
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

    from.async_read_some(asio::buffer(buf, sizeof(data1_)),
        [this, self, &from, &to, &buf, direction](std::error_code ec, std::size_t length) {
            if (!ec) {
                asio::async_write(to, asio::buffer(buf, length),
                    [this, self, direction](std::error_code ec, std::size_t) {
                        if (!ec) {
                            DoRead(direction);
                        }
                    });
            }
        });
}

// --- Client ---
Client::Client(const std::string& server_addr, uint16_t server_port, const std::string& token)
    : socket_(io_context_),
      server_addr_(server_addr),
      server_port_(server_port),
      token_(token),
      endpoint_(asio::ip::make_address(server_addr), server_port),
      reconnect_timer_(io_context_) {
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
    socket_.async_connect(endpoint_,
        [this](std::error_code ec) {
            OnConnect(ec);
        });
}

void Client::OnConnect(const std::error_code& ec) {
    if (!ec) {
        std::cout << "Connected to server." << std::endl;
        reconnect_delay_sec_ = 0; // Reset delay on success
        DoLogin();
        DoReadHeader();
    } else {
        HandleDisconnect("Connect failed: " + ec.message());
    }
}

void Client::HandleDisconnect(const std::string& reason) {
    std::cout << reason << std::endl;
    std::error_code ec;
    socket_.close(ec);
    ScheduleReconnect();
}

void Client::ScheduleReconnect() {
    // Increase delay by 10s each time, max 600s (10 mins)
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
    std::string encoded = msg.Encode();
    uint32_t len = static_cast<uint32_t>(encoded.length());
    
    auto data = std::make_shared<std::string>();
    data->append(reinterpret_cast<const char*>(&len), sizeof(len));
    data->append(encoded);
    
    asio::async_write(socket_, asio::buffer(*data), [this, data](std::error_code ec, std::size_t) {
        if (ec) {
            std::cerr << "Failed to send message: " << ec.message() << std::endl;
        }
    });
}

void Client::DoReadHeader() {
    asio::async_read(socket_, asio::buffer(&header_, sizeof(header_)),
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
    asio::async_read(socket_, asio::buffer(body_data_),
        [this](std::error_code ec, std::size_t) {
            if (!ec) {
                std::string data(body_data_.begin(), body_data_.end());
                try {
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

    auto pc = *it;
    auto local_socket = std::make_shared<tcp::socket>(io_context_);
    auto work_socket = std::make_shared<tcp::socket>(io_context_);

    local_socket->async_connect(tcp::endpoint(asio::ip::make_address(pc.local_ip), pc.local_port),
        [this, local_socket, work_socket, ticket, pc](std::error_code ec) {
            if (!ec) {
                work_socket->async_connect(tcp::endpoint(asio::ip::make_address(server_addr_), server_port_ + 1),
                    [this, local_socket, work_socket, ticket, pc](std::error_code ec) {
                        if (!ec) {
                            auto ticket_buf = std::make_shared<std::string>(ticket);
                            ticket_buf->resize(64, ' ');
                            
                            asio::async_write(*work_socket, asio::buffer(*ticket_buf),
                                [local_socket, work_socket, ticket_buf](std::error_code ec, std::size_t) {
                                    if (!ec) {
                                        std::cout << "Bridging local service and server work connection" << std::endl;
                                        auto bridge = std::make_shared<Bridge>(std::move(*local_socket), std::move(*work_socket));
                                        bridge->Start();
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

} // namespace client
} // namespace cfrp
