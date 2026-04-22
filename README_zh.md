# cfrp - C++ 快速反向代理

[English](./README.md) | 简体中文

基于 C++17 和 Standalone Asio 实现的高性能异步反向代理。`cfrp` 旨在作为一个轻量级且高效的替代方案，用于将 NAT 或防火墙后的本地服务暴露到互联网，其灵感源自流行的 [fatedier/frp](https://github.com/fatedier/frp) 项目。

## 特性

- **高性能**: 基于 Standalone Asio 构建，支持非阻塞异步 I/O。
- **TCP/QUIC 多路复用**: 使用自定义的轻量级多路复用协议将所有流量合并到 **单个连接** 中。支持传统的 TCP 和现代的 **QUIC (基于 ngtcp2)** 协议。
- **自动协议模式**:
  - **服务端**: 在同一个端口上自动处理 TCP 和 QUIC 客户端。
  - **客户端**: 优先尝试 QUIC 连接，并支持在必要时自动降级到 TCP。
- **安全性**: 支持可选的 **SSL/TLS** 加密和 **Token 身份验证**。使用 **wolfSSL** 实现高性能加密和现代 QUIC 支持。
- **带宽效率**: 支持可选的 **Zstd 压缩**（涵盖控制流和数据流），服务端自动检测并处理。
- **弹性客户端**: 如果服务器不可达，客户端支持自动重连，并带有指数退避机制。支持退出时的 **优雅清理**。
- **动态代理**: 支持在单个控制连接上运行多个 **TCP** 和 **UDP** 代理。支持通过 `conf.d` 目录进行 **热重载**。
- **轻量级**: 极简依赖 (`asio`, `tomlplusplus`, `cli11`, `nlohmann-json`, `wolfssl`, `ngtcp2`)。使用紧凑的 **二进制协议** (MessagePack) 以实现极低的开销。
- **清晰配置**: 使用 TOML 格式，易于阅读和配置服务端及客户端设置。

## 架构

1. **多路复用隧道**: 客户端与服务端之间持久的连接（TCP/SSL 或 QUIC）。使用 **自定义多路复用协议** 在此物理连接上复用多个逻辑流。
2. **控制流**: 用于交换指令的虚拟流，使用 **MessagePack** 二进制序列化.
3. **数据流**: 根据需求动态建立的虚拟流，用于桥接流量。支持 **自动压缩检测**。
4. **数据拼接**: 外部用户与本地服务之间双向异步数据转发。

## 快速上手

### 前置条件

- 支持 C++17 的编译器
- CMake 3.10+
- [vcpkg](https://github.com/microsoft/vcpkg) 用于依赖管理
- wolfSSL (通过 vcpkg 安装)

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
protocol = "auto" # 同时支持 TCP 和 QUIC

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
name = "my-client"
protocol = "auto" # 优先尝试 QUIC，失败后降级到 TCP

[client.ssl]
enable = true
verify_peer = false
```
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
- `protocol`: 要使用的协议 (`tcp`, `quic` 或 `auto`)。默认为 `quic`。
- `[server.ssl]`: SSL 设置。
  - `enable`: 为控制连接和工作连接启用 SSL/TLS。
  - `cert_file`: 证书文件路径。
  - `key_file`: 私钥文件路径。

### 客户端配置 ([client])
- `server_addr`: 服务端 IP 或域名。
- `server_port`: 服务端控制端口。
- `token`: 身份验证密钥。
- `protocol`: 要使用的协议 (`tcp`, `quic` 或 `auto`)。默认为 `quic`。在 `auto` 模式下，客户端会尝试 QUIC 并在 5 秒超时后降级到 TCP。
- `name`: 客户端的可选唯一名称。如果省略，服务端将自动分配一个，并通过添加后缀（如 `client_1`）确保唯一性。
- `compression`: 为所有连接启用 Zstd 压缩 (默认 `true`)。
- `conf_d`: 可选，用于动态代理配置的目录路径。
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

## 安全设计：wolfSSL 与 QUIC

本项目已迁移至 **wolfSSL**，以支持现代 **QUIC** 协议，同时为 TCP 连接保持高性能的 TLS 支持：

- **QUIC 支持**: 利用 `ngtcp2` 配合 `wolfSSL` 实现最先进的加密传输。
- **单向 TLS**: 标准 TLS 结合 **Token 身份验证**。
- **服务端**: SSL/TLS 和 QUIC 均需要 `cert_file` 和 `key_file`。
- **客户端**: 仅需验证服务端的身份。

**为什么选择 QUIC？**
QUIC 在丢包严重的网络环境中表现更佳，连接建立速度更快 (0-RTT)，并消除了传统 TCP 多路复用中常见的队头阻塞问题。

## 开源协议

[MIT License](LICENSE)
