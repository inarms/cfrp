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

#pragma once

#ifndef ASIO_USE_WOLFSSL
#define ASIO_USE_WOLFSSL
#endif

#include <asio.hpp>
#include <wolfssl/options.h>
#include <wolfssl/openssl/ssl.h>
#include <asio/ssl.hpp>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <deque>
#include "common/protocol.h"
#include "common/stream.h"
#include "common/mux.h"
#include "common/quic_ngtcp2.h"
#include "common/rate_limiter.h"

namespace cfrp {
namespace server {

using asio::ip::tcp;
using asio::ip::udp;

struct SslConfig {
    bool enable = true;
    bool auto_generate = true;
    std::string cert_file = "certs/server.crt";
    std::string key_file = "certs/server.key";
    std::string ca_file = "certs/ca.crt";
};

struct PortRange {
    uint16_t start;
    uint16_t end;
};

class Server;
class ControlSession;

class Bridge : public std::enable_shared_from_this<Bridge> {
public:
    Bridge(std::shared_ptr<common::AsyncStream> s1, std::shared_ptr<common::AsyncStream> s2, bool use_compression, std::shared_ptr<common::RateLimiter> rate_limiter = nullptr);
    void Start();

private:
    void DoRead(int direction);

    std::shared_ptr<common::AsyncStream> s1_;
    std::shared_ptr<common::AsyncStream> s2_;
    std::shared_ptr<common::RateLimiter> rate_limiter_;
    bool use_compression_;
    char data1_[32768];
    char data2_[32768];
    uint32_t header1_;
    uint32_t header2_;
};

// --- UDP Support ---

class UdpBridge : public std::enable_shared_from_this<UdpBridge> {
public:
    UdpBridge(asio::io_context& io_context, std::shared_ptr<common::AsyncStream> stream, udp::socket& socket, udp::endpoint remote_endpoint, bool use_compression, std::shared_ptr<common::RateLimiter> rate_limiter = nullptr);
    void Start();

private:
    void DoReadFromStream();
    void DoWriteToStream(const std::vector<uint8_t>& data);
    void HandleUdpPacket(const std::vector<uint8_t>& data);

    std::shared_ptr<common::AsyncStream> stream_;
    std::shared_ptr<common::RateLimiter> rate_limiter_;
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
    void SendMessage(protocol::MessageType type, const std::vector<uint8_t>& body);

private:
    void DoReadHeader();
    void DoReadBody(uint32_t length);
    void HandleMessage(const protocol::Message& msg);
    void HandleLogin(const std::vector<uint8_t>& body);

    Server& server_;
    std::shared_ptr<common::AsyncStream> stream_;
    asio::io_context& io_context_;
    std::string client_endpoint_;
    std::string client_name_;
    protocol::Header header_;
    std::vector<char> body_data_;
    std::vector<std::shared_ptr<ProxyListener>> proxies_;
    std::vector<std::shared_ptr<UdpProxyListener>> udp_proxies_;
    std::vector<std::string> registered_domains_;
    bool authenticated_ = false;
    bool compression_enabled_ = false;
};

class Server {
public:
    Server(asio::io_context& io_context, const std::string& bind_addr, uint16_t bind_port, const std::string& token, const SslConfig& ssl_config, const std::string& protocol = "auto", const std::vector<PortRange>& allowed_ports = {}, const std::vector<std::string>& allowed_clients = {});
    void Run();
    void Stop();

    void SetVhostPorts(uint16_t http_port, uint16_t https_port) {
        vhost_http_port_ = http_port;
        vhost_https_port_ = https_port;
    }

    void RegisterUserConn(const std::string& ticket, tcp::socket socket, const std::string& proxy_name, const std::vector<uint8_t>& initial_data = {});
    void RegisterUdpSession(const std::string& ticket, std::shared_ptr<UdpProxyListener> listener, udp::endpoint endpoint, const std::string& proxy_name);
    
    void AddVhostRoute(const std::string& domain, std::shared_ptr<ControlSession> session, const std::string& proxy_name, const std::string& type);
    void RemoveVhostRoute(const std::string& domain);

    std::shared_ptr<common::RateLimiter> GetRateLimiter(const std::string& proxy_name);
    void CreateRateLimiter(const std::string& proxy_name, int64_t bytes_per_sec);

    std::string AllocateClientName(const std::string& requested_name);
    void ReleaseClientName(const std::string& name);

    const std::string& GetToken() const { return token_; }
    const SslConfig& GetSslConfig() const { return ssl_config_; }
    bool IsPortAllowed(uint16_t port) const;
    bool IsClientAllowed(const std::string& name) const;

private:
    void DoAccept();
    void DoVhostAccept(std::unique_ptr<tcp::acceptor>& acceptor, const std::string& type);
    void HandleNewMuxStream(std::shared_ptr<common::mux::Session> mux_session, std::shared_ptr<common::mux::MuxStream> stream);

    void DoUdpRead();

    asio::io_context& io_context_;
    tcp::acceptor acceptor_;
    udp::socket udp_socket_;
    std::string token_;
    std::string protocol_;
    SslConfig ssl_config_;
    std::vector<PortRange> allowed_ports_;
    std::vector<std::string> allowed_clients_;
    std::unique_ptr<asio::ssl::context> ssl_ctx_;

    uint16_t vhost_http_port_ = 0;
    uint16_t vhost_https_port_ = 0;
    std::unique_ptr<tcp::acceptor> vhost_http_acceptor_;
    std::unique_ptr<tcp::acceptor> vhost_https_acceptor_;

    struct VhostRoute {
        std::weak_ptr<ControlSession> session;
        std::string proxy_name;
        std::string type; // "http" or "https"
    };
    std::map<std::string, VhostRoute> vhost_routes_;
    std::map<std::string, std::shared_ptr<common::RateLimiter>> proxy_rate_limiters_;
    
    struct TcpSessionInfo {
        tcp::socket socket;
        std::vector<uint8_t> initial_data;
        std::string proxy_name;
    };

    struct UdpSessionInfo {
        std::shared_ptr<UdpProxyListener> listener;
        udp::endpoint endpoint;
        std::string proxy_name;
    };

    std::map<std::string, TcpSessionInfo> pending_user_conns_;
    std::map<std::string, UdpSessionInfo> pending_udp_sessions_;
    std::vector<std::string> active_client_names_;
    std::mutex map_mutex_;

    // ngtcp2 state
    std::map<asio::ip::udp::endpoint, std::shared_ptr<common::quic::QuicSession>> quic_sessions_;
    uint8_t udp_recv_buf_[65535];
};

} // namespace server
} // namespace cfrp
