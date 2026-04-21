#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <deque>
#include <optional>
#include <msquic.h>
#include "common/protocol.h"
#include "common/stream.h"
#include "common/mux.h"

namespace cfrp {
namespace server {

using asio::ip::tcp;
using asio::ip::udp;

struct SslConfig {
    bool enable = false;
    std::string cert_file;
    std::string key_file;
};

class Server;
class ControlSession;

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

// --- UDP Support ---

class UdpBridge : public std::enable_shared_from_this<UdpBridge> {
public:
    UdpBridge(asio::io_context& io_context, std::shared_ptr<common::AsyncStream> stream, udp::socket& socket, udp::endpoint remote_endpoint, bool use_compression);
    void Start();

private:
    void DoReadFromStream();
    void DoWriteToStream(const std::vector<uint8_t>& data);
    void HandleUdpPacket(const std::vector<uint8_t>& data);
    void StartTimer();
    void ResetTimer();

    asio::steady_timer timer_;
    std::shared_ptr<common::AsyncStream> stream_;
    udp::socket& socket_;
    udp::endpoint remote_endpoint_;
    bool use_compression_;
    uint16_t packet_len_;
    std::vector<uint8_t> read_buf_;
};

class UdpProxyListener : public std::enable_shared_from_this<UdpProxyListener> {
public:
    UdpProxyListener(Server& server, asio::io_context& io_context, uint16_t port, std::shared_ptr<ControlSession> session, const std::string& proxy_name);
    void Start();
    void Stop();
    const std::string& name() const { return proxy_name_; }
    void SendTo(const std::vector<uint8_t>& data, const udp::endpoint& endpoint);
    udp::socket& socket() { return socket_; }

private:
    void DoReceive();

    Server& server_;
    udp::socket socket_;
    udp::endpoint sender_endpoint_;
    std::weak_ptr<ControlSession> session_;
    std::string proxy_name_;
    std::map<udp::endpoint, std::string> endpoint_to_ticket_;
    uint8_t recv_buf_[65535];
};

// --- Listeners & Sessions ---

class ProxyListener : public std::enable_shared_from_this<ProxyListener> {
public:
    ProxyListener(Server& server, asio::io_context& io_context, uint16_t port, std::shared_ptr<ControlSession> session, const std::string& proxy_name);
    void Start();
    void Stop();
    const std::string& name() const { return proxy_name_; }

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
    std::string client_endpoint_;
    std::string client_name_;
    protocol::Header header_;
    std::vector<char> body_data_;
    std::vector<std::shared_ptr<ProxyListener>> proxies_;
    std::vector<std::shared_ptr<UdpProxyListener>> udp_proxies_;
    bool authenticated_ = false;
    bool compression_enabled_ = false;
};

class Server {
public:
    Server(asio::io_context& io_context, const std::string& bind_addr, uint16_t bind_port, const std::string& token, const SslConfig& ssl_config, const std::string& protocol = "quic");
    void Run();
    void Stop();

    void RegisterUserConn(const std::string& ticket, tcp::socket socket);
    void RegisterUdpSession(const std::string& ticket, std::shared_ptr<UdpProxyListener> listener, udp::endpoint endpoint);
    
    std::string AllocateClientName(const std::string& requested_name);
    void ReleaseClientName(const std::string& name);

    const std::string& GetToken() const { return token_; }
    const SslConfig& GetSslConfig() const { return ssl_config_; }

private:
    void DoAccept();
    void HandleNewMuxStream(std::shared_ptr<common::mux::Session> mux_session, std::shared_ptr<common::mux::MuxStream> stream);

    // QUIC Support
    struct ConnectionContext {
        Server* server;
        std::vector<std::shared_ptr<common::mux::Session>> sessions;
        std::mutex mutex;
    };

    void StartQuicListener();
    static QUIC_STATUS QUIC_API QuicListenerCallback(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event);
    static QUIC_STATUS QUIC_API QuicConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);

    asio::io_context& io_context_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    tcp::acceptor acceptor_;
    std::string token_;
    std::string protocol_;
    SslConfig ssl_config_;
    std::unique_ptr<asio::ssl::context> ssl_ctx_;
    
    HQUIC quic_listener_ = nullptr;
    std::vector<ConnectionContext*> active_quic_conns_;
    
    struct UdpSessionInfo {
        std::shared_ptr<UdpProxyListener> listener;
        udp::endpoint endpoint;
    };

    std::map<std::string, tcp::socket> pending_user_conns_;
    std::map<std::string, UdpSessionInfo> pending_udp_sessions_;
    std::vector<std::string> active_client_names_;
    std::mutex map_mutex_;
};

} // namespace server
} // namespace cfrp
