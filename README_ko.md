# cfrp - C++ 고성능 리버스 프록시

English | [简体中文](./README_zh.md) | [日本語](./README_ja.md) | 한국어

`cfrp`는 C++17과 Standalone Asio를 사용하여 구축된 고성능 비동기 리버스 프록시입니다. [fatedier/frp](https://github.com/fatedier/frp)에서 영감을 얻었으며, NAT나 방화벽 뒤에 있는 로컬 서비스를 안전하고 효율적으로 인터넷에 노출합니다.

## 주요 기능
- **고성능:** Standalone Asio를 이용한 논블로킹 비동기 I/O.
- **다중 프로토콜:** TCP, UDP, HTTP, HTTPS (SNI), QUIC, WebSocket 지원.
- **보안 내장:** SSL/QUIC 인증서 자동 생성 및 토큰 인증 지원.
- **효율성:** 멀티플렉싱 터널 및 선택적 Zstd 압축 지원.
- **동적 설정:** `conf.d` 디렉토리를 통한 프록시 핫 리로드 지원.
- **크로스 플랫폼:** Windows, Linux, macOS 지원.

## 빠른 시작 (Docker)
Docker를 사용하는 것이 가장 쉬운 방법입니다:
```bash
# 서버와 클라이언트를 로컬에서 실행
docker compose up -d
```

## 설치 방법

### 스크립트 설치 (Linux/macOS)
```bash
# 서버
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode server

# 클라이언트
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode client
```
*(Windows PowerShell 스크립트는 [Releases](https://github.com/inarms/cfrp/releases)를 참조하세요)*

### 수동 설정
기본 `server.toml`:
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

기본 `client.toml`:
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

`cfrp server.toml` 또는 `cfrp client.toml` 명령어로 실행합니다.

## 소스 빌드
```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg 설치 경로]/scripts/buildsystems/vcpkg.cmake
cmake --build .
```

## 라이선스
Apache License 2.0. [fatedier/frp](https://github.com/fatedier/frp)에서 영감을 얻었습니다.
