# cfrp - C++ Fast Reverse Proxy

A high-performance, asynchronous reverse proxy implemented in C++17 using Standalone Asio. `cfrp` is designed to be a lightweight and efficient alternative for exposing local services behind a NAT or firewall to the internet, similar to the `frp` project.

## Features

- **High Performance**: Built on Standalone Asio for non-blocking, asynchronous I/O.
- **Resilient Client**: Automatic reconnection with exponential backoff (up to 10 minutes) if the server becomes unreachable.
- **Dynamic Proxying**: Supports multiple TCP proxies over a single control connection.
- **Lightweight**: Minimal dependencies (`asio`, `tomlplusplus`, `cli11`, `nlohmann-json`).
- **Clean Configuration**: Uses TOML for easy-to-read server and client settings.

## Architecture

1. **Control Channel**: A persistent TCP connection between the client and server for command exchange.
2. **Work Connections**: Dynamic TCP connections established on-demand to bridge traffic.
3. **Data Splicing**: Bi-directional asynchronous data forwarding between external users and local services.

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

### Usage

#### 1. Start the Server
Configure `config_server.toml`:
```toml
[server]
bind_addr = "0.0.0.0"
bind_port = 7001
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

[[client.proxies]]
name = "ssh"
type = "tcp"
local_ip = "127.0.0.1"
local_port = 22
remote_port = 6000
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

## Configuration

| Option | Description |
|--------|-------------|
| `-c, --config` | Path to the TOML configuration file (defaults to `config_server.toml`). |

## License

[MIT License](LICENSE)
