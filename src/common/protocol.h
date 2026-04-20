#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace cfrp {
namespace protocol {

using json = nlohmann::json;

enum class MessageType {
    RegisterProxy = 1,
    RegisterProxyResp = 2,
    NewUserConn = 3,
    StartWorkConn = 4,
    WorkConnAuth = 5
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
