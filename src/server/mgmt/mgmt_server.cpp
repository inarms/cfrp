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

#include "mgmt_server.h"
#include "common/utils.h"
#include <sstream>
#include <iomanip>

namespace cfrp {
namespace server {

ManagementServer::ManagementServer(asio::io_context& io_context, const std::string& bind_addr, uint16_t bind_port, std::shared_ptr<Server> server)
    : io_context_(io_context),
      acceptor_(io_context, asio::ip::tcp::endpoint(asio::ip::make_address(bind_addr), bind_port)),
      server_(server),
      bind_addr_(bind_addr),
      bind_port_(bind_port) {
}

void ManagementServer::Start() {
    common::Logger::Info("[Mgmt] Management server listening on " + bind_addr_ + ":" + std::to_string(bind_port_));
    DoAccept();
}

void ManagementServer::Stop() {
    acceptor_.close();
}

void ManagementServer::SetCredentials(const std::string& username, const std::string& password) {
    if (!username.empty() || !password.empty()) {
        expected_auth_ = common::Base64Encode(username + ":" + password);
    } else {
        expected_auth_.clear();
    }
}

void ManagementServer::DoAccept() {
    auto self(shared_from_this());
    auto socket = std::make_shared<asio::ip::tcp::socket>(io_context_);
    acceptor_.async_accept(*socket, [this, self, socket](std::error_code ec) {
        if (!ec) {
            HandleRequest(socket);
            if (acceptor_.is_open()) {
                DoAccept();
            }
        } else {
            if (ec != asio::error::operation_aborted && acceptor_.is_open()) {
                common::Logger::Error("[Mgmt] Accept error: " + ec.message());
                auto timer = std::make_shared<asio::steady_timer>(io_context_);
                timer->expires_after(std::chrono::milliseconds(100));
                timer->async_wait([this, self, timer](std::error_code) {
                    if (acceptor_.is_open()) DoAccept();
                });
            }
        }
    });
}

void ManagementServer::HandleRequest(std::shared_ptr<asio::ip::tcp::socket> socket) {
    auto self(shared_from_this());
    auto buffer = std::make_shared<asio::streambuf>();
    asio::async_read_until(*socket, *buffer, "\r\n\r\n", [this, self, socket, buffer](std::error_code ec, std::size_t length) {
        if (!ec) {
            std::istream is(buffer.get());
            std::string line;
            std::string method, path, version;
            
            if (!std::getline(is, line)) return;
            std::stringstream ss(line);
            ss >> method >> path >> version;

            std::string auth_header;
            while (std::getline(is, line) && line != "\r") {
                if (line.compare(0, 15, "Authorization: ") == 0) {
                    auth_header = line.substr(15);
                    if (!auth_header.empty() && auth_header.back() == '\r') auth_header.pop_back();
                }
            }

            bool authorized = true;
            if (!expected_auth_.empty()) {
                std::string required = "Basic " + expected_auth_;
                if (auth_header != required) {
                    authorized = false;
                }
            }

            std::string body;
            std::string content_type = "application/json";
            int status_code = 200;
            std::string status_text = "OK";
            std::string extra_headers;

            if (!authorized) {
                status_code = 401;
                status_text = "Unauthorized";
                extra_headers = "WWW-Authenticate: Basic realm=\"cfrp management\"\r\n";
                body = "{\"error\": \"Unauthorized\"}";
            } else if (path == "/api/v1/status") {
                body = GetStatusJson();
            } else if (path == "/api/v1/clients") {
                body = GetClientsJson();
            } else {
                status_code = 404;
                status_text = "Not Found";
                body = "{\"error\": \"Not Found\"}";
            }

            std::stringstream response;
            response << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
            response << "Content-Type: " << content_type << "\r\n";
            response << "Content-Length: " << body.length() << "\r\n";
            response << "Access-Control-Allow-Origin: *\r\n";
            response << extra_headers;
            response << "Connection: close\r\n";
            response << "\r\n";
            response << body;

            auto response_s = std::make_shared<std::string>(response.str());
            asio::async_write(*socket, asio::buffer(*response_s), [self, socket, response_s](std::error_code, std::size_t) {
                std::error_code ec;
                socket->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            });
        }
    });
}

std::string ManagementServer::GetStatusJson() {
    auto total_stats = server_->GetTotalStats();
    std::stringstream ss;
    ss << "{"
       << "\"version\": \"" << CFRP_VERSION << "\","
       << "\"status\": \"running\","
       << "\"total_bytes_sent\": " << total_stats.bytes_sent.load() << ","
       << "\"total_bytes_received\": " << total_stats.bytes_received.load()
       << "}";
    return ss.str();
}

std::string ManagementServer::GetClientsJson() {
    auto clients = server_->GetClientsInfo();
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < clients.size(); ++i) {
        const auto& c = clients[i];
        ss << "{"
           << "\"name\": \"" << c.name << "\","
           << "\"endpoint\": \"" << c.endpoint << "\","
           << "\"protocol\": \"" << c.protocol << "\","
           << "\"proxies\": [";
        for (size_t j = 0; j < c.proxies.size(); ++j) {
            const auto& p = c.proxies[j];
            ss << "{"
               << "\"name\": \"" << p.name << "\","
               << "\"type\": \"" << p.type << "\","
               << "\"port\": " << p.port << ","
               << "\"active_conns\": " << p.active_conns << ","
               << "\"total_conns\": " << p.total_conns << ","
               << "\"bytes_sent\": " << p.bytes_sent << ","
               << "\"bytes_received\": " << p.bytes_received
               << "}";
            if (j < c.proxies.size() - 1) ss << ",";
        }
        ss << "]}";
        if (i < clients.size() - 1) ss << ",";
    }
    ss << "]";
    return ss.str();
}

} // namespace server
} // namespace cfrp
