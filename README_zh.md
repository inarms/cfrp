# cfrp - C++ 快速反向代理

[English](./README.md) | 简体中文

基于 C++17 和 Standalone Asio 实现的高性能异步反向代理。`cfrp` 旨在作为一个轻量级且高效的替代方案，用于将 NAT 或防火墙后的本地服务暴露到互联网，其灵感源自流行的 [fatedier/frp](https://github.com/fatedier/frp) 项目。

## 特性

- **高性能**: 基于 Standalone Asio 构建，支持非阻塞异步 I/O。
- **安全性**: 支持可选的 **SSL/TLS** 加密和 **Token 身份验证**。
- **弹性客户端**: 如果服务器不可达，客户端支持自动重连，并带有指数退避机制（最高 10 分钟）。
- **动态代理**: 支持在单个控制连接上运行多个 **TCP** 和 **UDP** 代理。
- **轻量级**: 极简依赖 (`asio`, `tomlplusplus`, `cli11`, `nlohmann-json`, `openssl`)。使用紧凑的 **二进制协议** (MessagePack) 以实现极低的开销。
- **清晰配置**: 使用 TOML 格式，易于阅读和配置服务端及客户端设置。

## 架构

1. **控制通道**: 客户端与服务端之间持久的 TCP 连接（可选 SSL），用于交换指令。
2. **工作连接**: 根据需求动态建立的 TCP 连接，用于桥接流量。
3. **数据拼接**: 外部用户与本地服务之间双向异步数据转发。

## 快速上手

### 前置条件

- 支持 C++17 的编译器
- CMake 3.10+
- [vcpkg](https://github.com/microsoft/vcpkg) 用于依赖管理
- OpenSSL

### 构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg 路径]/scripts/buildsystems/vcpkg.cmake
cmake --build .
```

### 使用方法

#### 1. 启动服务端
配置 `config_server.toml`:
```toml
[server]
bind_addr = "0.0.0.0"
bind_port = 7001
token = "你的密钥"

[server.ssl]
enable = true
cert_file = "server.crt"
key_file = "server.key"
```
运行服务端:
```bash
./cfrp -c config_server.toml
```

#### 2. 启动客户端
配置 `config_client.toml`:
```toml
[client]
server_addr = "你的服务器IP"
server_port = 7001
token = "你的密钥"

[client.ssl]
enable = true
verify_peer = false

[[client.proxies]]
name = "ssh"
type = "tcp"
local_ip = "127.0.0.1"
local_port = 22
remote_port = 6000

[[client.proxies]]
name = "dns"
type = "udp"
local_ip = "8.8.8.8"
local_port = 53
remote_port = 5300
```
运行客户端:
```bash
./cfrp -c config_client.toml
```

#### 3. 访问你的服务
你现在可以通过服务器的公网 IP 访问本地服务:
```bash
ssh -p 6000 用户名@你的服务器IP
```

## 配置选项

### 服务端配置 ([server])
- `bind_addr`: 监听地址 (默认 `0.0.0.0`)。
- `bind_port`: 控制端口 (默认 `7000`)。
- `token`: 与客户端共享的身份验证密钥。
- `[server.ssl]`: SSL 设置。
  - `enable`: 为控制连接和工作连接启用 SSL/TLS。
  - `cert_file`: 证书文件路径。
  - `key_file`: 私钥文件路径。

### 客户端配置 ([client])
- `server_addr`: 服务端 IP 或域名。
- `server_port`: 服务端控制端口。
- `token`: 身份验证密钥。
- `[client.ssl]`: SSL 设置。
  - `enable`: 启用 SSL/TLS。
  - `verify_peer`: 验证服务端证书。
  - `ca_file`: 用于验证的 CA 证书路径。

### 代理配置 ([[client.proxies]])
- `name`: 代理的唯一名称。
- `type`: 协议类型 (`tcp` 或 `udp`)。
- `local_ip`: 本地服务 IP。
- `local_port`: 本地服务端口。
- `remote_port`: 在服务端暴露服务的端口。

## 安全设计：单向 TLS 与 mTLS

本项目采用了 **单向 TLS** (标准 TLS) 结合 **Token 身份验证** 的设计：

- **服务端**: 需要 `cert_file` 和 `key_file` 来向客户端证明自己的身份。
- **客户端**: 仅需验证服务端的身份（使用系统内置 CA 或提供的 `ca_file`）。
- **Token 验证**: 在 TLS 加密隧道建立后，通过共享的 `token` 来验证客户端权限。

**为什么不使用双向 TLS (mTLS)？**
双向 TLS 要求为每个客户端都生成并分发证书，这会带来巨大的运维负担（如证书的生成、分发和定期轮换）。通过将单向 TLS（负责加密传输）与 Token 验证（负责客户端授权）相结合，我们在保证高安全性的同时，极大地提升了易用性。

## 开源协议

[MIT License](LICENSE)
