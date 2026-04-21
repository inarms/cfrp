# cfrp - C++ Fast Reverse Proxy

English | [简体中文](./README_zh.md)

A high-performance, asynchronous reverse proxy implemented in C++17 using Standalone Asio.
 `cfrp` is designed to be a lightweight and efficient alternative for exposing local services behind a NAT or firewall to the internet, inspired by the popular [fatedier/frp](https://github.com/fatedier/frp) project.

## Features

- **High Performance**: Built on Standalone Asio for non-blocking, asynchronous I/O.
- **TCP Multiplexing**: Consolidates all traffic into a **single TCP connection** using the **Yamux** protocol. No separate work port required.
- **Security**: Optional **SSL/TLS** encryption and **Token-based authentication**.
- **Bandwidth Efficiency**: Optional **Zstd compression** for both control and data channels, with automatic server-side detection.
- **Resilient Client**: Automatic reconnection with exponential backoff (up to 10 minutes) if the server becomes unreachable.
- **Dynamic Proxying**: Supports multiple **TCP** and **UDP** proxies over a single control connection.
- **Lightweight**: Minimal dependencies (`asio`, `tomlplusplus`, `cli11`, `nlohmann-json`, `openssl`). Uses a compact **binary protocol** (MessagePack) for minimal overhead.
- **Clean Configuration**: Uses TOML for easy-to-read server and client settings.

## Architecture

1. **Multiplexed Tunnel**: A single persistent TCP connection (optionally SSL) between the client and server. Uses **Yamux** to multiplex multiple logical streams over this single physical connection.
2. **Control Stream**: A virtual stream used for command exchange using **MessagePack** binary serialization.
3. **Data Streams**: Dynamic virtual streams established on-demand to bridge traffic. Supports **automatic compression detection**.
4. **Data Splicing**: Bi-directional asynchronous data forwarding between external users and local services.

## Getting Started

### Prerequisites

- C++17 compatible compiler
- CMake 3.10+
- [vcpkg](https://github.com/microsoft/vcpkg) for dependency management
- OpenSSL

### Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[path/to/vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build .
```

### Usage

#### 1. Start the Server
Configure `config_server.toml`:
```toml
[server]
bind_addr = "0.0.0.0"
bind_port = 7001
token = "your_secret_token"

[server.ssl]
enable = true
cert_file = "server.crt"
key_file = "server.key"
```
Run the server:
```bash
./cfrp -c config_server.toml
```

#### 2. Start the Client
Configure `config_client.toml`:
```toml
[client]
server_addr = "your_server_ip"
server_port = 7001
token = "your_secret_token"

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
Run the client:
```bash
./cfrp -c config_client.toml
```

#### 3. Access your service
You can now access your local service via the server's public IP:
```bash
ssh -p 6000 user@your_server_ip
```

## Configuration Options

### Server Section
- `bind_addr`: Address to listen on (default `0.0.0.0`).
- `bind_port`: Control port (default `7000`).
- `token`: Authentication token shared with the client.
- `[server.ssl]`: SSL settings.
  - `enable`: Enable SSL/TLS for control and work connections.
  - `cert_file`: Path to the certificate file.
  - `key_file`: Path to the private key file.

### Client Section
- `server_addr`: Server IP or hostname.
- `server_port`: Server control port.
- `token`: Authentication token.
- `compression`: Enable Zstd compression for all connections (default `true`).
- `[client.ssl]`: SSL settings.
  - `enable`: Enable SSL/TLS.
  - `verify_peer`: Verify server certificate.
  - `ca_file`: Path to CA certificate for verification.

### Proxy Section (`[[client.proxies]]`)
- `name`: Unique name for the proxy.
- `type`: Protocol type (`tcp` or `udp`).
- `local_ip`: Local service IP.
- `local_port`: Local service port.
- `remote_port`: Port on the server to expose the service.

## Security Design: One-Way TLS vs. mTLS

This project uses **One-Way TLS** (Standard) combined with **Token Authentication**:

- **Server-Side**: Requires `cert_file` and `key_file` to prove its identity to clients.
- **Client-Side**: Only needs to verify the server's identity (using system CA roots or a provided `ca_file`). 
- **Token-based Auth**: Once the secure TLS tunnel is established, a shared `token` is used to authenticate the client.

**Why not Mutual TLS (mTLS)?**
mTLS requires certificates for every client, which adds significant management overhead (generating, distributing, and rotating client certificates). By combining One-Way TLS (to secure the transport) with Token Authentication (to verify the client), we achieve a high level of security with much better usability.

## License

[MIT License](LICENSE)
