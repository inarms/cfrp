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
