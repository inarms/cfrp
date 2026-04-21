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
    WorkConnAuth = 6
};

struct Header {
    uint32_t body_length;
};

// Simple helper to wrap messages in JSON
struct Message {
    MessageType type;
    json body;

    std::string Encode() const {
        json j;
        j["type"] = static_cast<int>(type);
        j["body"] = body;
        return j.dump();
    }

    static Message Decode(const std::string& data) {
        auto j = json::parse(data);
        return {static_cast<MessageType>(j["type"].get<int>()), j["body"]};
    }
};

} // namespace protocol
} // namespace cfrp
