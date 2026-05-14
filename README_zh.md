# cfrp - C++ 快速反向代理

[English](./README.md) | 简体中文 | [日本語](./README_ja.md) | [한국어](./README_ko.md)

`cfrp` 是一个基于 C++17 和 Standalone Asio 构建的高性能异步反向代理，深受 [fatedier/frp](https://github.com/fatedier/frp) 的启发。它可以安全、高效地将处于 NAT 或防火墙后的本地服务暴露到互联网。

## 特性
- **高性能：** 基于 Standalone Asio 的非阻塞异步 I/O。
- **多协议支持：** 支持 TCP, UDP, HTTP, HTTPS (SNI), QUIC 和 WebSockets。
- **内置安全：** 自动生成 SSL/QUIC 证书并支持 Token 身份验证。
- **高效率：** 多路复用隧道和可选的 Zstd 压缩。
- **动态配置：** 通过 `conf.d` 目录热重载代理配置。
- **跨平台：** 支持 Windows, Linux 和 macOS。

## 快速上手 (Docker)
使用 Docker 运行 `cfrp` 是最简单的方式：
```bash
# 在本地启动服务端和客户端
docker compose up -d
```

## 安装

### 脚本安装 (Linux/macOS)
```bash
# 服务端
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode server

# 客户端
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode client
```
*(Windows PowerShell 脚本请参见 [Releases](https://github.com/inarms/cfrp/releases))*

### 手动配置
基础 `server.toml`:
```toml
[server]
bind_addr = "0.0.0.0"
bind_port = 7001
token = "your_secret_token"
protocol = "auto"

[server.ssl]
enable = true
auto_generate = true
```

基础 `client.toml`:
```toml
[client]
server_addr = "your_server_ip"
server_port = 7001
token = "your_secret_token"

[[client.proxies]]
name = "ssh"
type = "tcp"
local_ip = "127.0.0.1"
local_port = 22
remote_port = 6000
```

使用 `cfrp server.toml` 或 `cfrp client.toml` 运行。

## 从源码构建
```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg 路径]/scripts/buildsystems/vcpkg.cmake
cmake --build .
```

## 开源协议
Apache License 2.0。深受 [fatedier/frp](https://github.com/fatedier/frp) 启发。
