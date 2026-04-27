# cfrp - C++ Fast Reverse Proxy

English | [简体中文](./README_zh.md)

A high-performance, asynchronous reverse proxy implemented in C++17 using Standalone Asio.
 `cfrp` is designed to be a lightweight and efficient alternative for exposing local services behind a NAT or firewall to the internet, inspired by the popular [fatedier/frp](https://github.com/fatedier/frp) project.

## Features

- **High Performance**: Built on Standalone Asio for non-blocking, asynchronous I/O.
- **Zero-Config (Out-of-the-Box Security)**: Automatically generates SSL/QUIC certificates and CA chains if missing. No manual OpenSSL commands required.
- **Multiplexing over TCP/QUIC**: Consolidates all traffic into a **single connection** using a custom-built, lightweight multiplexing protocol. Supports both traditional TCP and the modern **QUIC (via ngtcp2)** protocol.
- **Auto Protocol Mode**:
  - **Server**: Automatically handles both TCP and QUIC clients on the same port.
  - **Client**: Attempts a QUIC connection first and automatically fails over to TCP if needed.
- **Security**: Optional **SSL/TLS** encryption and **Token-based authentication**. Uses **wolfSSL** for high-performance cryptography and modern QUIC support.
- **Bandwidth Efficiency**: Optional **Zstd compression** for both control and data channels, with automatic server-side detection.
- **Resilient Client**: Automatic reconnection with exponential backoff if the server becomes unreachable. Supports **graceful cleanup** on exit.
- **Dynamic Proxying**: Supports multiple **TCP**, **UDP**, **HTTP**, and **HTTPS (SNI)** proxies over a single control connection. Supports **hot-reloading** via a `conf.d` directory.
- **Protocol Flexibility**: Supports **TCP**, **QUIC**, and **WebSocket** for the underlying tunnel. WebSocket support allows for firewall traversal and CDN (e.g. Cloudflare) integration.
- **VHost Support**: Multiple web services can share the same HTTP (80) or HTTPS (443) port using domain-based routing.
- **DNS Resolution**: `local_ip` now supports hostnames (e.g., `localhost` or Docker service names).
- **Traffic Control**: Per-proxy bandwidth limiting to prevent network saturation.
- **Lightweight**: Minimal dependencies (`asio`, `tomlplusplus`, `wolfssl`, `ngtcp2`). Uses a compact **custom binary protocol** for minimal overhead.
- **Clean Configuration**: Uses TOML for easy-to-read server and client settings.

## Zero-Config Security

`cfrp` makes it effortless to secure your tunnel. When you enable QUIC or TLS:
1. **Automated PKI**: If certificates are missing or expired, the server automatically generates a Root CA and a Server Certificate.
2. **Auto-Cleanup**: Certificates are stored in the `certs/` directory and renewed automatically when they near expiration.
3. **Easy Distribution**: Simply copy the generated `certs/ca.crt` to your client devices to enable full peer verification.

## Installation

### 1. Server Installation
Installs the binary and sets up a background service (systemd/launchd/Windows Service).

**Linux & macOS:**
```bash
curl -sSL https://raw.githubusercontent.com/neesonqk/cfrp/main/scripts/install.sh | sudo bash -s -- --mode server
```
- **Config Path (Linux):** `/etc/cfrp/server.toml`
- **Config Path (macOS):** `/usr/local/etc/cfrp/server.toml`

**Windows (PowerShell Admin):**
```powershell
iex (iwr https://raw.githubusercontent.com/neesonqk/cfrp/main/scripts/install.ps1).Content -Args "-Mode server"
```
- **Config Path:** `C:\Program Files\cfrp\server.toml`

---

### 2. Client Installation
Installs the binary and sets up a background service.

**Linux & macOS:**
```bash
curl -sSL https://raw.githubusercontent.com/neesonqk/cfrp/main/scripts/install.sh | sudo bash -s -- --mode client
```
- **Config Path (Linux):** `/etc/cfrp/client.toml`
- **Config Path (macOS):** `/usr/local/etc/cfrp/client.toml`

**Windows (PowerShell Admin):**
```powershell
iex (iwr https://raw.githubusercontent.com/neesonqk/cfrp/main/scripts/install.ps1).Content -Args "-Mode client"
```
- **Config Path:** `C:\Program Files\cfrp\client.toml`

---

### 3. CLI Tool Only
Installs the binary to system PATH without creating a service.

**Linux & macOS:**
```bash
curl -sSL https://raw.githubusercontent.com/neesonqk/cfrp/main/scripts/install.sh | sudo bash -s -- --mode cli
```

**Windows (PowerShell Admin):**
```powershell
iex (iwr https://raw.githubusercontent.com/neesonqk/cfrp/main/scripts/install.ps1).Content -Args "-Mode cli"
```

## Architecture

1. **Multiplexed Tunnel**: A single persistent connection (TCP/SSL or QUIC) between the client and server. Uses a custom multiplexing protocol to handle multiple logical streams over this single physical connection.
2. **Control Stream**: A virtual stream used for command exchange using a **custom binary** serialization protocol.
3. **Data Streams**: Dynamic virtual streams established on-demand to bridge traffic. Supports **automatic compression detection**.
4. **Data Splicing**: Bi-directional asynchronous data forwarding between external users and local services.

## Getting Started

### Prerequisites

- C++17 compatible compiler
- CMake 3.10+
- [vcpkg](https://github.com/microsoft/vcpkg) for dependency management

### Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[path/to/vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build .
```

## Usage

`cfrp` can be started as either a server or a client node.

### Automatic Configuration Selection
**Note: If `server.toml` or `client.toml` exists in the current directory, they take absolute precedence. All command-line positional arguments and flags (like `-c` or `-t`) will be ignored.**

If no configuration file exists, the application follows this logic:
1. **Positional Argument**: If a path is provided (e.g., `./cfrp my.toml`), it uses that file.
2. **Flags**: If `-c` and `-t` are provided, it generates a `client.toml` using those values.
3. **Default**: Otherwise, it generates a default `server.toml` and starts as a Server.

The search order for existing files is:
1. **`server.toml`**: If found, starts as a **Server**.
2. **`client.toml`**: If found, starts as a **Client**.

### Examples

#### 1. Start as a Server
To run a server using the default `server.toml`:
```bash
./cfrp
```
Or specify a custom configuration:
```bash
./cfrp my_server.toml
```

Example **`server.toml`**:
```toml
[server]
bind_addr = "0.0.0.0"
bind_port = 7001
token = "your_secret_token"
protocol = "auto" # Supports TCP and QUIC simultaneously

[server.ssl]
enable = true
auto_generate = true # Auto-generate CA and certificates
```

#### 2. Start as a Client
To run a client using the default `client.toml`:
```bash
./cfrp
```

Example **`client.toml`**:
```toml
[client]
server_addr = "your_server_ip"
server_port = 7001
token = "your_secret_token"
name = "my-client"
protocol = "auto" # Try QUIC first, failover to TCP

[[client.proxies]]
name = "ssh"
type = "tcp"
local_ip = "127.0.0.1"
local_port = 22
remote_port = 6000
```

#### 3. Quick Client Setup (Automatic Generation)
If you have the server's CA certificate (`ca.crt`) and the token, you can quickly start a client. `cfrp` will automatically generate a `client.toml` if it doesn't exist:
```bash
./cfrp -c certs/ca.crt -t your_secret_token
```
- **Forces Client Mode** even if `server.toml` is present.
- If `client.toml` is **missing**, it generates a default one using the provided CA for verification and the provided token.
- If `client.toml` **exists**, it uses the existing configuration (the `-c` and `-t` parameters are ignored).

#### 4. Access your service
Once the tunnel is established, access your local service via the server's public IP:
```bash
ssh -p 6000 user@your_server_ip
```

## Proxy Hot-Reloading

`cfrp` supports dynamic proxy management without restarting the client. By monitoring a directory (default `./conf.d`), you can add, update, or remove proxies on the fly.

### Steps to use Hot-Reloading:

1. **Enable in Client Config**:
   Ensure `conf_d` is set in your `client.toml`:
   ```toml
   [client]
   conf_d = "./conf.d"
   ```

2. **Create the Directory**:
   ```bash
   mkdir -p ./conf.d
   ```

3. **Add a Proxy**:
   Create a new `.toml` file inside `./conf.d/` (e.g., `web.toml`):
   ```toml
   name = "my-web-service"
   type = "tcp"
   local_ip = "127.0.0.1"
   local_port = 8080
   remote_port = 8081
   ```
   The client will detect the new file and register the proxy with the server immediately.

4. **Update or Remove**:
   - **Update**: Modify the fields in `web.toml`. The client will unregister the old config and register the new one.
   - **Remove**: Delete `web.toml`. The client will stop the proxy and inform the server.

## Configuration Options

### Server Section
- `bind_addr`: Address to listen on (default `0.0.0.0`).
- `bind_port`: Control port (default `7000`).
- `vhost_http_port`: Port for HTTP vhost routing (e.g., `80`).
- `vhost_https_port`: Port for HTTPS SNI routing (e.g., `443`).
- `token`: Authentication token shared with the client.
- `allowed_ports`: Optional list of allowed ports or port ranges (e.g., `[6000, "8000-9000"]`). If omitted, all ports are allowed.
- `allowed_clients`: Optional whitelist of allowed client names (e.g., `["my-client", "office-pc"]`). If omitted, any client name is allowed.
- `protocol`: Protocol to use (`tcp`, `quic`, `websocket`, or `auto`). Default is `auto`.
- `[server.ssl]`: SSL settings.
  - `enable`: Enable SSL/TLS for control and work connections (TCP only).
  - `auto_generate`: Automatically generate CA and Server certificates if missing or expired (default `true`).
  - `cert_file`: Path to the certificate file (default `certs/server.crt`).
  - `key_file`: Path to the private key file (default `certs/server.key`).
  - `ca_file`: Path to the CA certificate file (default `certs/ca.crt`).

### Client Section
- `server_addr`: Server IP or hostname.
- `server_port`: Server control port.
- `token`: Authentication token.
- `protocol`: Protocol to use (`tcp`, `quic`, `websocket`, or `auto`). Default is `auto`. In `auto` mode, the client tries protocols in the following order: **QUIC -> TCP -> WebSocket**, failing over to the next if the previous one fails or times out.
- `name`: Optional unique name for this client. If omitted, the server automatically assigns one and ensures uniqueness by adding suffixes (e.g. `client_1`).
- `compression`: Enable Zstd compression for all connections (default `true`).
- `conf_d`: Optional path to a directory for dynamic proxy configurations.
- `[client.ssl]`: SSL settings.
  - `enable`: Enable SSL/TLS.
  - `verify_peer`: Verify server certificate.
  - `ca_file`: Path to CA certificate for verification.

### Proxy Section (`[[client.proxies]]`)
- `name`: Unique name for the proxy.
- `type`: Protocol type (`tcp`, `udp`, `http`, or `https`).
- `local_ip`: Local service IP or hostname (e.g., `127.0.0.1` or `localhost`).
- `local_port`: Local service port.
- `remote_port`: Port on the server to expose the service (required for `tcp`/`udp`).
- `custom_domains`: Domain name(s) for `http`/`https` types (e.g., `["a.com", "b.com"]`).
- `bandwidth_limit`: Bandwidth limit for the proxy (e.g., `"1M"`, `"512K"`, or bytes as integer).

## Security Design: wolfSSL & QUIC

This project has migrated to **wolfSSL** to support the modern **QUIC** protocol while maintaining high-performance TLS for TCP connections:

- **QUIC Support**: Leverages `ngtcp2` with `wolfSSL` for state-of-the-art encrypted transport.
- **One-Way TLS**: Standard TLS combined with **Token Authentication**.
- **Server-Side**: Requires `cert_file` and `key_file` for both SSL/TLS and QUIC.
- **Client-Side**: Only needs to verify the server's identity. 

**Why QUIC?**
QUIC provides better performance in lossy network environments, faster connection establishment (0-RTT), and eliminates head-of-line blocking which is a common issue with traditional TCP multiplexing.

## License

This project is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for details.

This project is inspired by the original [frp](https://github.com/fatedier/frp) project by fatedier, which is also licensed under the Apache License 2.0.
