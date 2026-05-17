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

#include <asio.hpp>
#include <string>
#include <memory>
#include <vector>
#include "../server.h"

namespace cfrp {
namespace server {

class ManagementServer : public std::enable_shared_from_this<ManagementServer> {
public:
    ManagementServer(asio::io_context& io_context, const std::string& bind_addr, uint16_t bind_port, std::shared_ptr<Server> server);
    void Start();
    void Stop();

    void SetCredentials(const std::string& username, const std::string& password);

private:
    void DoAccept();
    void HandleRequest(std::shared_ptr<asio::ip::tcp::socket> socket);
    
    // Simple JSON helpers to avoid adding heavy dependencies
    std::string GetStatusJson();
    std::string GetClientsJson();

    asio::io_context& io_context_;
    asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<Server> server_;
    std::string bind_addr_;
    uint16_t bind_port_;
    std::string expected_auth_; // Base64(username:password)
};

} // namespace server
} // namespace cfrp
