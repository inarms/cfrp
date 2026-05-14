# cfrp - C++ Fast Reverse Proxy

English | [简体中文](./README_zh.md) | [日本語](./README_ja.md) | [한국어](./README_ko.md)

`cfrp` is a high-performance, asynchronous reverse proxy built with C++17 and Standalone Asio, inspired by [fatedier/frp](https://github.com/fatedier/frp). It exposes local services behind NATs or firewalls securely and efficiently.

## Features
- **High Performance:** Non-blocking async I/O with Standalone Asio.
- **Multiprotocol:** Supports TCP, UDP, HTTP, HTTPS (SNI), QUIC, and WebSockets.
- **Security Built-in:** Automatic SSL/QUIC certificate generation and token authentication.
- **Efficiency:** Multiplexed tunnels and optional Zstd compression.
- **Dynamic Configuration:** Hot-reload proxies via `conf.d` directory.
- **Cross-Platform:** Works on Windows, Linux, and macOS.

## Quick Start (Docker)
The easiest way to run `cfrp` is via Docker:
```bash
# Start server and client locally
docker compose up -d
```

## Installation

### Script Installation (Linux/macOS)
```bash
# Server
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode server

# Client
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode client
```
*(For Windows PowerShell scripts, see [Releases](https://github.com/inarms/cfrp/releases))*

### Manual Configuration
Basic `server.toml`:
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

Basic `client.toml`:
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

Run with `cfrp server.toml` or `cfrp client.toml`.

## Building from Source
```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[path/to/vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build .
```

## License
Apache License 2.0. Inspired by [fatedier/frp](https://github.com/fatedier/frp).
