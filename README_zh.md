# cfrp - C++ 快速反向代理

[English](./README.md) | 简体中文

基于 C++17 和 Standalone Asio 实现的高性能异步反向代理。`cfrp` 旨在作为一个轻量级且高效的替代方案，用于将 NAT 或防火墙后的本地服务暴露到互联网，其灵感源自流行的 [fatedier/frp](https://github.com/fatedier/frp) 项目。

## 特性

- **高性能**: 基于 Standalone Asio 构建，支持非阻塞异步 I/O。
- **弹性客户端**: 如果服务器不可达，客户端支持自动重连，并带有指数退避机制（最高 10 分钟）。
- **动态代理**: 支持在单个控制连接上运行多个 TCP 代理。
- **轻量级**: 极简依赖 (`asio`, `tomlplusplus`, `cli11`, `nlohmann-json`)。
- **清晰配置**: 使用 TOML 格式，易于阅读和配置服务端及客户端设置。

## 架构

1. **控制通道**: 客户端与服务端之间持久的 TCP 连接，用于交换指令。
2. **工作连接**: 根据需求动态建立的 TCP 连接，用于桥接流量。
3. **数据拼接**: 外部用户与本地服务之间双向异步数据转发。

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

### 使用方法

#### 1. 启动服务端
配置 `config_server.toml`:
```toml
[server]
bind_addr = "0.0.0.0"
bind_port = 7001
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

[[client.proxies]]
name = "ssh"
type = "tcp"
local_ip = "127.0.0.1"
local_port = 22
remote_port = 6000
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

| 选项 | 描述 |
|--------|-------------|
| `-c, --config` | TOML 配置文件路径 (默认为 `config_server.toml`)。 |

## 开源协议

[MIT License](LICENSE)
