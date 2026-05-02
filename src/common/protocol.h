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

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <asio.hpp>

namespace cfrp {
namespace protocol {

enum class MessageType : uint8_t {
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
static constexpr uint32_t MAX_MESSAGE_SIZE = 1024 * 1024;
static constexpr uint32_t MAX_CONTROL_MESSAGE_SIZE = 4 * 1024 * 1024;
static constexpr unsigned long long MAX_DECOMPRESSED_SIZE = 16ULL * 1024 * 1024;

class BinaryWriter {
public:
    explicit BinaryWriter(size_t reserve_bytes = 0) {
        if (reserve_bytes > 0) {
            data_.reserve(reserve_bytes);
        }
    }

    void WriteUint8(uint8_t v) { data_.push_back(v); }
    void WriteUint16(uint16_t v) {
        uint16_t net = asio::detail::socket_ops::host_to_network_short(v);
        AppendBytes(&net, 2);
    }
    void WriteUint32(uint32_t v) {
        uint32_t net = asio::detail::socket_ops::host_to_network_long(v);
        AppendBytes(&net, 4);
    }
    void WriteInt64(int64_t v) {
        uint32_t net_high = asio::detail::socket_ops::host_to_network_long(static_cast<uint32_t>((static_cast<uint64_t>(v) >> 32) & 0xFFFFFFFFULL));
        uint32_t net_low = asio::detail::socket_ops::host_to_network_long(static_cast<uint32_t>(v & 0xFFFFFFFF));
        AppendBytes(&net_high, 4);
        AppendBytes(&net_low, 4);
    }
    void WriteString(const std::string& s) {
        WriteUint16(static_cast<uint16_t>(s.size()));
        AppendBytes(s.data(), s.size());
    }
    void WriteStringVector(const std::vector<std::string>& v) {
        WriteUint16(static_cast<uint16_t>(v.size()));
        for (const auto& s : v) WriteString(s);
    }
    const std::vector<uint8_t>& GetData() const { return data_; }
    std::vector<uint8_t> MoveData() { return std::move(data_); }
private:
    void AppendBytes(const void* src, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(src);
        if (len == 0) {
            return;
        }
        size_t old_size = data_.size();
        data_.resize(old_size + len);
        std::memcpy(data_.data() + old_size, p, len);
    }
    std::vector<uint8_t> data_;
};

class BinaryReader {
public:
    BinaryReader(const std::vector<uint8_t>& data) : data_(data), pos_(0) {}
    uint8_t ReadUint8() { return data_.at(pos_++); }
    uint16_t ReadUint16() {
        uint16_t v;
        ReadBytes(&v, 2);
        return asio::detail::socket_ops::network_to_host_short(v);
    }
    uint32_t ReadUint32() {
        uint32_t v;
        ReadBytes(&v, 4);
        return asio::detail::socket_ops::network_to_host_long(v);
    }
    int64_t ReadInt64() {
        uint32_t high = ReadUint32();
        uint32_t low = ReadUint32();
        return (static_cast<int64_t>(high) << 32) | low;
    }
    std::string ReadString() {
        uint16_t len = ReadUint16();
        if (pos_ + len > data_.size()) throw std::runtime_error("Buffer overflow");
        std::string s(reinterpret_cast<const char*>(&data_[pos_]), len);
        pos_ += len;
        return s;
    }
    std::vector<std::string> ReadStringVector() {
        uint16_t count = ReadUint16();
        std::vector<std::string> v;
        for (uint16_t i = 0; i < count; ++i) v.push_back(ReadString());
        return v;
    }
private:
    void ReadBytes(void* dest, size_t len) {
        if (pos_ + len > data_.size()) throw std::runtime_error("Buffer overflow");
        std::memcpy(dest, &data_[pos_], len);
        pos_ += len;
    }
    const std::vector<uint8_t>& data_;
    size_t pos_;
};

struct Message {
    MessageType type;
    std::vector<uint8_t> body;

    std::vector<uint8_t> Encode() const {
        std::vector<uint8_t> data;
        data.push_back(static_cast<uint8_t>(type));
        data.insert(data.end(), body.begin(), body.end());
        return data;
    }

    static Message Decode(const std::vector<uint8_t>& data) {
        if (data.empty()) throw std::runtime_error("Empty message");
        MessageType type = static_cast<MessageType>(data[0]);
        std::vector<uint8_t> body(data.begin() + 1, data.end());
        return {type, std::move(body)};
    }
};

struct LoginMessage {
    std::string token;
    std::string name;
    std::vector<uint8_t> Serialize() const {
        BinaryWriter w;
        w.WriteString(token);
        w.WriteString(name);
        return w.MoveData();
    }
    static LoginMessage Deserialize(const std::vector<uint8_t>& data) {
        BinaryReader r(data);
        return {r.ReadString(), r.ReadString()};
    }
};

struct LoginRespMessage {
    std::string status; // "ok" or "error"
    std::string name;
    std::string message;
    std::vector<uint8_t> Serialize() const {
        BinaryWriter w;
        w.WriteString(status);
        w.WriteString(name);
        w.WriteString(message);
        return w.MoveData();
    }
    static LoginRespMessage Deserialize(const std::vector<uint8_t>& data) {
        BinaryReader r(data);
        return {r.ReadString(), r.ReadString(), r.ReadString()};
    }
};

struct RegisterProxyMessage {
    std::string name;
    std::string type;
    uint16_t remote_port;
    std::vector<std::string> custom_domains;
    int64_t bandwidth_limit;
    std::vector<uint8_t> Serialize() const {
        BinaryWriter w;
        w.WriteString(name);
        w.WriteString(type);
        w.WriteUint16(remote_port);
        w.WriteStringVector(custom_domains);
        w.WriteInt64(bandwidth_limit);
        return w.MoveData();
    }
    static RegisterProxyMessage Deserialize(const std::vector<uint8_t>& data) {
        BinaryReader r(data);
        return {r.ReadString(), r.ReadString(), r.ReadUint16(), r.ReadStringVector(), r.ReadInt64()};
    }
};

struct RegisterProxyRespMessage {
    std::string status;
    std::string name;
    std::string message;
    std::vector<uint8_t> Serialize() const {
        BinaryWriter w;
        w.WriteString(status);
        w.WriteString(name);
        w.WriteString(message);
        return w.MoveData();
    }
    static RegisterProxyRespMessage Deserialize(const std::vector<uint8_t>& data) {
        BinaryReader r(data);
        return {r.ReadString(), r.ReadString(), r.ReadString()};
    }
};

struct NewUserConnMessage {
    std::string proxy_name;
    std::string ticket;
    std::vector<uint8_t> Serialize() const {
        BinaryWriter w;
        w.WriteString(proxy_name);
        w.WriteString(ticket);
        return w.MoveData();
    }
    static NewUserConnMessage Deserialize(const std::vector<uint8_t>& data) {
        BinaryReader r(data);
        return {r.ReadString(), r.ReadString()};
    }
};

struct UnregisterProxyMessage {
    std::string name;
    std::vector<uint8_t> Serialize() const {
        BinaryWriter w;
        w.WriteString(name);
        return w.MoveData();
    }
    static UnregisterProxyMessage Deserialize(const std::vector<uint8_t>& data) {
        BinaryReader r(data);
        return {r.ReadString()};
    }
};

} // namespace protocol
} // namespace cfrp
