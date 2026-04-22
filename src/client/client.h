#pragma once

#ifndef ASIO_USE_WOLFSSL
#define ASIO_USE_WOLFSSL
#endif

#include <asio.hpp>
#include <wolfssl/options.h>
#include <wolfssl/openssl/ssl.h>
#include <asio/ssl.hpp>
#include <string>
#include <vector>
#include "common/protocol.h"
#include "common/stream.h"
#include "common/mux.h"
#include "common/quic_ngtcp2.h"

namespace cfrp {
namespace client {

using asio::ip::tcp;
using asio::ip::udp;

struct SslConfig {
    bool enable = false;
    bool verify_peer = false;
    std::string ca_file;
};

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
    Bridge(std::shared_ptr<common::AsyncStream> s1, std::shared_ptr<common::AsyncStream> s2, bool use_compression);
    void Start();

private:
    void DoRead(int direction);

    std::shared_ptr<common::AsyncStream> s1_;
    std::shared_ptr<common::AsyncStream> s2_;
    bool use_compression_;
    char data1_[8192];
    char data2_[8192];
    uint32_t header1_;
    uint32_t header2_;
};

class UdpBridge : public std::enable_shared_from_this<UdpBridge> {
public:
    UdpBridge(asio::io_context& io_context, std::shared_ptr<common::AsyncStream> stream, udp::endpoint local_endpoint, bool use_compression);
    void Start();

private:
    void DoReadFromStream();
    void DoReadFromLocal();
    void StartTimer();
    void ResetTimer();

    asio::steady_timer timer_;
    std::shared_ptr<common::AsyncStream> stream_;
    udp::socket socket_;
    udp::endpoint local_endpoint_;
    bool use_compression_;
    uint16_t packet_len_;
    std::vector<uint8_t> read_buf_;
    uint8_t local_recv_buf_[65535];
};

class Client : public std::enable_shared_from_this<Client> {
public:
    Client(asio::io_context& io_context, const std::string& server_addr, uint16_t server_port, const std::string& token, const std::string& name, const SslConfig& ssl_config, bool compression, const std::string& conf_d_path, const std::string& protocol = "quic");
    void Run();
    void AddProxy(const ProxyConfig& proxy);

private:
    void DoConnect();
    void DoQuicConnect();
    void DoUdpRead();
    void OnConnect(const std::error_code& ec, std::shared_ptr<common::AsyncStream> underlying_stream);
    void SendMessage(protocol::MessageType type, const protocol::json& body);
    void DoReadHeader();
    void DoReadBody(uint32_t length);
    void HandleMessage(const protocol::Message& msg);
    void DoLogin();
    void RegisterProxies();
    void HandleNewUserConn(const std::string& proxy_name, const std::string& ticket);
    void HandleNewUdpUserConn(const ProxyConfig& pc, const std::string& ticket);
    void HandleDisconnect(const std::string& reason);
    void ScheduleReconnect();

    void StartConfMonitor();
    void PollConfDirectory();
    void RegisterProxy(const ProxyConfig& pc);
    void UnregisterProxy(const std::string& name);

    asio::io_context& io_context_;
    std::shared_ptr<common::mux::Session> mux_session_;
    std::shared_ptr<common::mux::MuxStream> control_stream_;
    
    std::string server_addr_;
    uint16_t server_port_;
    std::string token_;
    std::string name_;
    std::string protocol_;
    SslConfig ssl_config_;
    bool compression_ = false;
    std::unique_ptr<asio::ssl::context> ssl_ctx_;
    
    tcp::endpoint endpoint_;
    udp::endpoint udp_endpoint_;
    udp::socket udp_socket_;
    std::vector<ProxyConfig> proxies_;
    protocol::Header header_;
    std::vector<char> body_data_;
    
    std::string conf_d_path_;
    asio::steady_timer conf_timer_;
    std::map<std::string, ProxyConfig> dynamic_proxies_;
    
    asio::steady_timer reconnect_timer_;
    int reconnect_delay_sec_ = 0;

    // ngtcp2 state
    std::shared_ptr<common::quic::QuicSession> quic_session_;
    uint8_t udp_recv_buf_[65535];
};

} // namespace client
} // namespace cfrp
