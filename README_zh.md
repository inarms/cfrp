# cfrp - C++ 快速反向代理

[English](./README.md) | 简体中文

基于 C++17 和 Standalone Asio 实现的高性能异步反向代理。`cfrp` 旨在作为一个轻量级且高效的替代方案，用于将 NAT 或防火墙后的本地服务暴露到互联网，其灵感源自流行的 [fatedier/frp](https://github.com/fatedier/frp) 项目。

## 特性

- **高性能**: 基于 Standalone Asio 构建，支持非阻塞异步 I/O。
- **零配置 (开箱即用安全性)**: 如果缺失，自动生成 SSL/QUIC 证书和 CA 链。无需手动运行 OpenSSL 命令。
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

## 零配置安全性

`cfrp` 让加密隧道的建立变得毫不费力。当您启用 QUIC 或 TLS 时：
1. **自动 PKI**: 如果证书缺失或过期，服务端会自动生成根 CA 和服务端证书。
2. **自动管理**: 证书存储在 `certs/` 目录中，并在临近过期时自动更新。
3. **轻松分发**: 只需将生成的 `certs/ca.crt` 复制到您的客户端设备，即可开启完整的对端验证 (verify_peer)。

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

### 构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg 路径]/scripts/buildsystems/vcpkg.cmake
cmake --build .
```

## 使用方法

`cfrp` 可以作为服务端节点或客户端节点启动。

### 配置文件自动选择
如果未通过 `-c` 指定配置文件，程序将按以下顺序在当前目录中查找：
1. **`server.toml`**: 如果存在，以**服务端**模式启动。
2. **`client.toml`**: 如果存在，以**客户端**模式启动。
3. **无配置文件**: 自动生成默认的 `server.toml` 并以**服务端**模式启动。

*注意：如果同时存在 `server.toml` 和 `client.toml`，程序将优先使用 `server.toml`（除非使用了 `--ca` 参数）。*

### 示例

#### 1. 以服务端模式启动
使用默认的 `server.toml`:
```bash
./cfrp
```
或者明确指定配置文件：
```bash
./cfrp -c my_server.toml
```

示例 **`server.toml`**:
```toml
[server]
bind_addr = "0.0.0.0"
bind_port = 7001
token = "your_secret_token"
protocol = "auto" # 同时支持 TCP 和 QUIC

[server.ssl]
enable = true
auto_generate = true # 自动生成 CA 和证书
```

#### 2. 以客户端模式启动
使用默认的 `client.toml`:
```bash
./cfrp
```

示例 **`client.toml`**:
```toml
[client]
server_addr = "your_server_ip"
server_port = 7001
token = "your_secret_token"
name = "my-client"
protocol = "auto" # 优先尝试 QUIC，失败后降级到 TCP

[[client.proxies]]
name = "ssh"
type = "tcp"
local_ip = "127.0.0.1"
local_port = 22
remote_port = 6000
```

#### 3. 快速客户端设置 (自动生成)
如果你有服务端的 CA 证书 (`ca.crt`)，可以快速启动客户端。如果配置文件不存在，`cfrp` 会自动生成 `client.toml`:
```bash
./cfrp --ca certs/ca.crt
```
- **强制进入客户端模式**，即使存在 `server.toml`。
- 如果 `client.toml` **不存在**，将自动生成一个并配置为使用提供的 CA 进行验证。
- 如果 `client.toml` **已存在**，将直接使用现有配置（忽略 `--ca` 的 SSL 覆盖设置）。

#### 4. 访问服务
隧道建立后，通过服务端的公网 IP 访问本地服务：
```bash
ssh -p 6000 用户名@你的服务器IP
```

## 代理热重载

`cfrp` 支持无需重启客户端的动态代理管理。通过监控一个目录（默认为 `./conf.d`），您可以即时添加、更新或删除代理。

### 使用热重载的步骤：

1. **在客户端配置中启用**:
   确保您的 `client.toml` 中设置了 `conf_d`:
   ```toml
   [client]
   conf_d = "./conf.d"
   ```

2. **创建目录**:
   ```bash
   mkdir -p ./conf.d
   ```

3. **添加代理**:
   在 `./conf.d/` 目录下创建一个新的 `.toml` 文件（例如 `web.toml`）：
   ```toml
   name = "my-web-service"
   type = "tcp"
   local_ip = "127.0.0.1"
   local_port = 8080
   remote_port = 8081
   ```
   客户端将立即检测到新文件并向服务端注册该代理。

4. **更新或删除**:
   - **更新**: 修改 `web.toml` 中的字段。客户端将注销旧配置并注册新配置。
   - **删除**: 删除 `web.toml`。客户端将停止该代理并通知服务端。

## 配置选项

### 服务端配置 ([server])
- `bind_addr`: 监听地址 (默认 `0.0.0.0`)。
- `bind_port`: 控制端口 (默认 `7000`)。
- `token`: 与客户端共享的身份验证密钥。
- `allowed_ports`: 可选，允许代理使用的端口或端口范围列表 (例如 `[6000, "8000-9000"]`)。如果省略，则允许所有端口。
- `allowed_clients`: 可选，允许连接的客户端名称白名单 (例如 `["my-client", "office-pc"]`)。如果省略，则允许任何客户端名称。
- `protocol`: 要使用的协议 (`tcp`, `quic` 或 `auto`)。默认为 `auto`。
- `[server.ssl]`: SSL 设置。
  - `enable`: 为控制连接和工作连接启用 SSL/TLS (仅限 TCP)。
  - `auto_generate`: 如果证书缺失或过期，自动生成 CA 和服务端证书 (默认为 `true`)。
  - `cert_file`: 证书文件路径 (默认 `certs/server.crt`)。
  - `key_file`: 私钥文件路径 (默认 `certs/server.key`)。
  - `ca_file`: CA 证书文件路径 (默认 `certs/ca.crt`)。

### 客户端配置 ([client])
- `server_addr`: 服务端 IP 或域名。
- `server_port`: 服务端控制端口。
- `token`: 身份验证密钥。
- `protocol`: 要使用的协议 (`tcp`, `quic`, `websocket` 或 `auto`)。默认为 `auto`。在 `auto` 模式下，客户端会按以下顺序尝试协议：**QUIC -> TCP -> WebSocket**，如果前一个协议失败或超时，则自动降级到下一个。
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

本项目基于 Apache License 2.0 协议。详情请参阅 [LICENSE](LICENSE) 文件。

本项目深受 fatedier 开发的 [frp](https://github.com/fatedier/frp) 项目启发，该项目同样基于 Apache License 2.0 协议。
��于 Apache License 2.0 协议。详情请参阅 [LICENSE](LICENSE) 文件。

本项目深受 fatedier 开发的 [frp](https://github.com/fatedier/frp) 项目启发，该项目同样基于 Apache License 2.0 协议。
com/fatedier/frp) 项目启发，该项目同样基于 Apache License 2.0 协议。
