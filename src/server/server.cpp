#include "server.h"
#include <iostream>
#include <chrono>

namespace cfrp {
namespace server {

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
    std::cout << "New client connected to control port." << std::endl;
    DoReadHeader();
}

void ControlSession::Stop() {
    for (auto& proxy : proxies_) {
        proxy->Stop();
    }
    proxies_.clear();
    std::error_code ec;
    socket_.close(ec);
}

void ControlSession::SendMessage(protocol::MessageType type, const protocol::json& body) {
    protocol::Message msg{type, body};
    std::string encoded = msg.Encode();
    uint32_t len = static_cast<uint32_t>(encoded.length());
    
    auto data = std::make_shared<std::string>();
    data->append(reinterpret_cast<const char*>(&len), sizeof(len));
    data->append(encoded);

    auto self(shared_from_this());
    asio::async_write(socket_, asio::buffer(*data), [this, self, data](std::error_code ec, std::size_t) {
        if (ec) {
            std::cerr << "Failed to send message: " << ec.message() << std::endl;
        }
    });
}

void ControlSession::DoReadHeader() {
    auto self(shared_from_this());
    asio::async_read(socket_, asio::buffer(&header_, sizeof(header_)),
        [this, self](std::error_code ec, std::size_t) {
            if (!ec) {
                DoReadBody(header_.body_length);
            } else {
                std::cout << "Control session closed: " << ec.message() << std::endl;
                Stop();
            }
        });
}

void ControlSession::DoReadBody(uint32_t length) {
    body_data_.resize(length);
    auto self(shared_from_this());
    asio::async_read(socket_, asio::buffer(body_data_),
        [this, self](std::error_code ec, std::size_t) {
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
        
        try {
            auto listener = std::make_shared<ProxyListener>(server_, io_context_, remote_port, shared_from_this(), name);
            listener->Start();
            proxies_.push_back(listener);
            
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
    }
}

void ControlSession::HandleLogin(const protocol::json& body) {
    std::string token = body.value("token", "");
    protocol::json resp;
    if (token == server_.GetToken()) {
        std::cout << "Client authenticated successfully." << std::endl;
        authenticated_ = true;
        resp["status"] = "ok";
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
Server::Server(const std::string& bind_addr, uint16_t bind_port, const std::string& token)
    : acceptor_(io_context_, tcp::endpoint(asio::ip::make_address(bind_addr), bind_port)),
      work_acceptor_(io_context_, tcp::endpoint(asio::ip::make_address(bind_addr), bind_port + 1)),
      token_(token) {
    std::cout << "Server initialized on " << bind_addr << ":" << bind_port << " (Work port: " << bind_port + 1 << ")" << std::endl;
}

void Server::Run() {
    std::cout << "Starting cfrp server loop..." << std::endl;
    DoAccept();
    DoAcceptWork();
    io_context_.run();
}

void Server::RegisterUserConn(const std::string& ticket, tcp::socket socket) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    pending_user_conns_.emplace(ticket, std::move(socket));
}

void Server::DoAccept() {
    acceptor_.async_accept(
        [this](std::error_code ec, tcp::socket socket) {
            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    std::cerr << "Control accept error: " << ec.message() << std::endl;
                    DoAccept();
                }
                return;
            }
            std::make_shared<ControlSession>(*this, std::move(socket), io_context_)->Start();
            DoAccept();
        });
}

void Server::DoAcceptWork() {
    work_acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                std::cerr << "Work accept error: " << ec.message() << std::endl;
                DoAcceptWork();
            }
            return;
        }

        auto ticket_ptr = std::make_shared<std::string>();
        ticket_ptr->resize(64);
        
        auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
        
        asio::async_read(*socket_ptr, asio::buffer(&((*ticket_ptr)[0]), 64),
            [this, socket_ptr, ticket_ptr](std::error_code ec, std::size_t) {
                if (!ec) {
                    std::string ticket = ticket_ptr->c_str();
                    ticket.erase(ticket.find_last_not_of(" \n\r\t") + 1);

                    std::lock_guard<std::mutex> lock(map_mutex_);
                    auto it = pending_user_conns_.find(ticket);
                    if (it != pending_user_conns_.end()) {
                        std::cout << "Splicing user connection and work connection for ticket: " << ticket << std::endl;
                        auto bridge = std::make_shared<Bridge>(std::move(it->second), std::move(*socket_ptr));
                        bridge->Start();
                        pending_user_conns_.erase(it);
                    } else {
                        std::cerr << "No pending user connection for ticket: " << ticket << std::endl;
                    }
                }
            });

        DoAcceptWork();
    });
}

} // namespace server
} // namespace cfrp
