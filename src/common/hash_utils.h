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
#include <functional>

namespace cfrp {
namespace common {

struct UdpEndpointHash {
    std::size_t operator()(const asio::ip::udp::endpoint& ep) const {
        std::size_t h1 = std::hash<std::string>{}(ep.address().to_string());
        std::size_t h2 = std::hash<unsigned short>{}(ep.port());
        return h1 ^ (h2 << 1);
    }
};

struct UdpEndpointEqual {
    bool operator()(const asio::ip::udp::endpoint& lhs, const asio::ip::udp::endpoint& rhs) const {
        return lhs == rhs;
    }
};

} // namespace common
} // namespace cfrp
