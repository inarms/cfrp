#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include "common/protocol.h"
#include "common/stream.h"

namespace cfrp {
namespace server {

using asio::ip::tcp;

struct SslConfig {
    bool enable = false;
    std::string cert_file;
    std::string key_file;
};

class Server;
class ControlSession;

class Bridge : public std::enable_shared_from_this<Bridge> {
public:
    Bridge(std::shared_ptr<common::AsyncStream> s1, std::shared_ptr<common::AsyncStream> s2);
    void Start();

private:
    void DoRead(int direction);

    std::shared_ptr<common::AsyncStream> s1_;
    std::shared_ptr<common::AsyncStream> s2_;
    char data1_[8192];
    char data2_[8192];
};

class ProxyListener : public std::enable_shared_from_this<ProxyListener> {
public:
    ProxyListener(Server& server, asio::io_context& io_context, uint16_t port, std::shared_ptr<ControlSession> session, const std::string& proxy_name);
    void Start();
    void Stop();

private:
    void DoAccept();

    Server& server_;
    tcp::acceptor acceptor_;
    std::weak_ptr<ControlSession> session_;
    std::string proxy_name_;
};

class ControlSession : public std::enable_shared_from_this<ControlSession> {
public:
    explicit ControlSession(Server& server, std::shared_ptr<common::AsyncStream> stream, asio::io_context& io_context)
        : server_(server), stream_(std::move(stream)), io_context_(io_context) {}

    void Start();
    void Stop();
    void SendMessage(protocol::MessageType type, const protocol::json& body);

private:
    void DoReadHeader();
    void DoReadBody(uint32_t length);
    void HandleMessage(const protocol::Message& msg);
    void HandleLogin(const protocol::json& body);

    Server& server_;
    std::shared_ptr<common::AsyncStream> stream_;
    asio::io_context& io_context_;
    protocol::Header header_;
    std::vector<char> body_data_;
    std::vector<std::shared_ptr<ProxyListener>> proxies_;
    bool authenticated_ = false;
};

class Server {
public:
    Server(const std::string& bind_addr, uint16_t bind_port, const std::string& token, const SslConfig& ssl_config);
    void Run();

    void RegisterUserConn(const std::string& ticket, tcp::socket socket);
    const std::string& GetToken() const { return token_; }
    const SslConfig& GetSslConfig() const { return ssl_config_; }

private:
    void DoAccept();
    void DoAcceptWork();

    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    tcp::acceptor work_acceptor_;
    std::string token_;
    SslConfig ssl_config_;
    std::unique_ptr<asio::ssl::context> ssl_ctx_;
    
    std::map<std::string, tcp::socket> pending_user_conns_;
    std::mutex map_mutex_;
};

} // namespace server
} // namespace cfrp
