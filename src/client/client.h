#pragma once

#include <asio.hpp>
#include <string>
#include <vector>
#include "common/protocol.h"

namespace cfrp {
namespace client {

using asio::ip::tcp;

struct ProxyConfig {
    std::string name;
    std::string type;
    std::string local_ip;
    uint16_t local_port;
    uint16_t remote_port;
};

class Client;

class Bridge : public std::enable_shared_from_this<Bridge> {
public:
    Bridge(tcp::socket s1, tcp::socket s2);
    void Start();

private:
    void DoRead(int direction);

    tcp::socket s1_;
    tcp::socket s2_;
    char data1_[8192];
    char data2_[8192];
};

class Client {
public:
    Client(const std::string& server_addr, uint16_t server_port);
    void Run();
    void AddProxy(const ProxyConfig& proxy);

private:
    void DoConnect();
    void OnConnect(const std::error_code& ec);
    void SendMessage(protocol::MessageType type, const protocol::json& body);
    void DoReadHeader();
    void DoReadBody(uint32_t length);
    void HandleMessage(const protocol::Message& msg);
    void RegisterProxies();
    void HandleNewUserConn(const std::string& proxy_name, const std::string& ticket);
    void HandleDisconnect(const std::string& reason);
    void ScheduleReconnect();

    asio::io_context io_context_;
    tcp::socket socket_;
    std::string server_addr_;
    uint16_t server_port_;
    tcp::endpoint endpoint_;
    std::vector<ProxyConfig> proxies_;
    protocol::Header header_;
    std::vector<char> body_data_;
    
    asio::steady_timer reconnect_timer_;
    int reconnect_delay_sec_ = 0;
};

} // namespace client
} // namespace cfrp
