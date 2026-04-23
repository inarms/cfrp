/*
 * Copyright 2026 neesonqk
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

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace cfrp {
namespace protocol {

using json = nlohmann::json;

enum class MessageType {
    Login = 0,
    LoginResp = 1,
    RegisterProxy = 2,
    RegisterProxyResp = 3,
    NewUserConn = 4,
    StartWorkConn = 5,
    WorkConnAuth = 6,
    UnregisterProxy = 7
};

struct Header {
    uint32_t body_length;
};

const uint32_t COMPRESSION_FLAG = 0x80000000;
const uint32_t LENGTH_MASK = 0x7FFFFFFF;

// Simple helper to wrap messages in MessagePack
struct Message {
    MessageType type;
    json body;

    std::vector<uint8_t> Encode() const {
        json j;
        j["type"] = static_cast<int>(type);
        j["body"] = body;
        return json::to_msgpack(j);
    }

    static Message Decode(const std::vector<uint8_t>& data) {
        auto j = json::from_msgpack(data);
        return {static_cast<MessageType>(j["type"].get<int>()), j["body"]};
    }
};

} // namespace protocol
} // namespace cfrp
