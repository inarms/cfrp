# cfrp - C++ 고성능 리버스 프록시

English | [简体中文](./README_zh.md) | [日本語](./README_ja.md) | 한국어

Standalone Asio를 사용하여 C++17로 구현된 고성능 비동기 리버스 프록시입니다.
`cfrp`는 NAT나 방화벽 뒤에 있는 로컬 서비스를 인터넷에 노출하기 위한 가볍고 효율적인 대안으로 설계되었으며, 인기 있는 프로젝트인 [fatedier/frp](https://github.com/fatedier/frp)에서 영감을 얻었습니다.

## 주요 기능

- **고성능**: 논블로킹 비동기 I/O를 위해 Standalone Asio를 기반으로 구축되었습니다.
- **제로 설정 (즉시 사용 가능한 보안)**: 인증서가 없는 경우 SSL/QUIC 인증서와 CA 체인을 자동으로 생성합니다. 별도의 OpenSSL 명령어가 필요하지 않습니다.
- **TCP/QUIC 멀티플렉싱**: 자체 구축된 가벼운 멀티플렉싱 프로토콜을 사용하여 모든 트래픽을 **단일 연결**로 통합합니다. 전통적인 TCP와 최신 **QUIC (ngtcp2 경유)** 프로토콜을 모두 지원합니다.
- **자동 프로토콜 모드**:
  - **서버**: 동일한 포트에서 TCP와 QUIC 클라이언트를 모두 자동으로 처리합니다.
  - **클라이언트**: 먼저 QUIC 연결을 시도하고, 필요에 따라 자동으로 TCP로 페일오버합니다.
- **보안**: 선택적인 **SSL/TLS** 암호화 및 **토큰 기반 인증**을 지원합니다. 고성능 암호화와 최신 QUIC 지원을 위해 **wolfSSL**을 사용합니다.
- **대역폭 효율성**: 제어 채널과 데이터 채널 모두에서 선택적인 **Zstd 압축**을 지원하며, 서버 측에서 자동으로 감지합니다.
- **복원력 있는 클라이언트**: 서버에 연결할 수 없는 경우 지수 백오프를 사용한 자동 재연결을 수행합니다. 종료 시 **정상적인 정리(graceful cleanup)**를 지원합니다.
- **동적 프록시**: 단일 제어 연결을 통해 여러 **TCP**, **UDP**, **HTTP**, **HTTPS (SNI)** 프록시를 지원합니다. `conf.d` 디렉토리를 통한 **핫 리로드**를 지원합니다.
- **프로토콜 유연성**: 터널의 기반으로 **TCP**, **QUIC**, **WebSocket**을 지원합니다. WebSocket 지원을 통해 방화벽 통과 및 CDN(예: Cloudflare) 통합이 가능합니다.
- **VHost 지원**: 도메인 기반 라우팅을 사용하여 여러 웹 서비스가 동일한 HTTP (80) 또는 HTTPS (443) 포트를 공유할 수 있습니다.
- **DNS 확인**: `local_ip`에서 호스트 이름(예: `localhost` 또는 Docker 서비스 이름)을 지원합니다.
- **트래픽 제어**: 네트워크 포화를 방지하기 위한 프록시별 대역폭 제한 기능을 제공합니다.
- **경량화**: 최소한의 의존성(`asio`, `tomlplusplus`, `wolfssl`, `ngtcp2`)을 가지며, 오버헤드를 최소화하기 위해 컴팩트한 **자체 바이너리 프로토콜**을 사용합니다.
- **깔끔한 설정**: 읽기 쉬운 서버 및 클라이언트 설정을 위해 TOML 형식을 채택했습니다.

## 제로 설정 보안

`cfrp`는 터널의 보안 유지를 간편하게 해줍니다. QUIC 또는 TLS를 활성화하면:
1. **자동 PKI**: 인증서가 없거나 만료된 경우, 서버가 루트 CA와 서버 인증서를 자동으로 생성합니다.
2. **자동 정리**: 인증서는 `certs/` 디렉토리에 저장되며 만료가 가까워지면 자동으로 갱신됩니다.
3. **간편한 배포**: 생성된 `certs/ca.crt`를 클라이언트 장치에 복사하는 것만으로 전체 피어 검증(peer verification)을 활성화할 수 있습니다.

## 설치 방법

### 1. 수동 설치

#### 다운로드 안내
1. **[GitHub 리리스](https://github.com/inarms/cfrp/releases)** 페이지로 이동합니다.
2. 사용 중인 플랫폼과 아키텍처에 맞는 압축 파일을 다운로드합니다.
3. 압축을 해제합니다:
   - **서버 패키지**: `cfrp` 바이너리, `server.toml`, `setup.sh`/`uninstall.sh` (또는 `.ps1`)가 포함되어 있습니다.
   - **클라이언트 패키지**: `cfrp` 바이너, `client.toml`, `config.d/`, `setup.sh`/`uninstall.sh` (또는 `.ps1`)가 포함되어 있습니다.

#### 설정 파일 자동 선택
**주의: 현재 디렉토리에 `server.toml` 또는 `client.toml`이 존재하는 경우 해당 파일이 절대적으로 우선됩니다. 모든 명령줄 인자 및 플래그(예: `-c`, `-t`)는 무시됩니다.**

설정 파일이 없는 경우 애플리케이션은 다음 로직을 따릅니다:
1. **위치 인자**: 경로가 제공된 경우(예: `./cfrp my.toml`) 해당 파일을 사용합니다.
2. **플래그**: `-c`와 `-t`가 제공된 경우 해당 값을 사용하여 `client.toml`을 생성합니다.
3. **기본값**: 그 외의 경우 기본 `server.toml`을 생성하고 서버로 시작합니다.

기존 파일 검색 순서는 다음과 같습니다:
1. **`server.toml`**: 발견 시 **서버**로 시작합니다.
2. **`client.toml`**: 발견 시 **클라이언트**로 시작합니다.

#### 수동 사용 예시

##### 1. 서버로 시작
기본 `server.toml`을 사용하여 서버를 실행하려면:
```bash
./cfrp
```
또는 사용자 정의 설정을 지정하려면:
```bash
./cfrp my_server.toml
```

**`server.toml`** 예시:
```toml
[server]
bind_addr = "0.0.0.0"
bind_port = 7001
token = "your_secret_token"
protocol = "auto" # TCP와 QUIC을 동시에 지원

[server.ssl]
enable = true
auto_generate = true # CA 및 인증서 자동 생성
```

##### 2. 클라이언트로 시작
기본 `client.toml`을 사용하여 클라이언트를 실행하려면:
```bash
./cfrp
```

**`client.toml`** 예시:
```toml
[client]
server_addr = "your_server_ip"
server_port = 7001
token = "your_secret_token"
name = "my-client"
protocol = "auto" # QUIC을 먼저 시도하고 TCP로 페일오버

[[client.proxies]]
name = "ssh"
type = "tcp"
local_ip = "127.0.0.1"
local_port = 22
remote_port = 6000
```

##### 3. 빠른 클라이언트 설정 (자동 생성)
서버의 CA 인증서(`ca.crt`)와 토큰이 있는 경우 클라이언트를 빠르게 시작할 수 있습니다. `client.toml`이 없는 경우 `cfrp`가 자동으로 생성합니다:
```bash
./cfrp -c certs/ca.crt -t your_secret_token
```
- `server.toml`이 있더라도 **클라이언트 모드를 강제**합니다.
- `client.toml`이 **없는** 경우 제공된 CA와 토큰을 사용하여 기본 파일을 생성합니다.
- `client.toml`이 **이미 있는** 경우 기존 설정을 사용합니다 (`-c` 및 `-t` 파라미터 무시).

##### 4. 서비스 접속
터널이 구축되면 서버의 공인 IP를 통해 로컬 서비스에 접속합니다:
```bash
ssh -p 6000 user@your_server_ip
```

---

### 2. 스크립트 설치

#### 서버 설치
바이너리를 설치하고 백그라운드 서비스(systemd/launchd/Windows 서비스)를 설정합니다.

**Linux & macOS:**
```bash
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode server
```
- **설정 경로 (Linux):** `/etc/cfrp/server.toml`
- **설정 경로 (macOS):** `/usr/local/etc/cfrp/server.toml`

**Windows (PowerShell 관리자 권한):**
```powershell
iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.ps1).Content -Args "-Mode server"
```
- **설정 경로:** `C:\Program Files\cfrp\server.toml`

**서버 제거:**
- **Linux/macOS:** `curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.sh | sudo bash -s -- --mode server`
- **Windows:** `iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.ps1).Content -Args "-Mode server"`

#### 서버 관리
| 작업 | Linux (systemd) | macOS (launchd) | Windows (PowerShell) |
| :--- | :--- | :--- | :--- |
| **시작** | `sudo systemctl start cfrp-server` | `sudo launchctl load -w /Library/LaunchDaemons/com.inarms.cfrp-server.plist` | `Start-Service cfrp-server` |
| **중지** | `sudo systemctl stop cfrp-server` | `sudo launchctl unload /Library/LaunchDaemons/com.inarms.cfrp-server.plist` | `Stop-Service cfrp-server` |
| **상태** | `systemctl status cfrp-server` | `sudo launchctl list \| grep cfrp-server` | `Get-Service cfrp-server` |
| **로그** | `journalctl -u cfrp-server -f` | `tail -f /var/log/cfrp-server.log` | `C:\Program Files\cfrp\cfrp.log` 확인 |

---

#### 클라이언트 설치
바이너리를 설치하고 백그라운드 서비스를 설정합니다.

**Linux & macOS:**
```bash
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode client
```
- **설정 경로 (Linux):** `/etc/cfrp/client.toml`
- **설정 경로 (macOS):** `/usr/local/etc/cfrp/client.toml`

**Windows (PowerShell 관리자 권한):**
```powershell
iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.ps1).Content -Args "-Mode client"
```
- **설정 경로:** `C:\Program Files\cfrp\client.toml`

**클라이언트 제거:**
- **Linux/macOS:** `curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.sh | sudo bash -s -- --mode client`
- **Windows:** `iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.ps1).Content -Args "-Mode client"`

#### 클라이언트 관리
| 작업 | Linux (systemd) | macOS (launchd) | Windows (PowerShell) |
| :--- | :--- | :--- | :--- |
| **시작** | `sudo systemctl start cfrp-client` | `sudo launchctl load -w /Library/LaunchDaemons/com.inarms.cfrp-client.plist` | `Start-Service cfrp-client` |
| **중지** | `sudo systemctl stop cfrp-client` | `sudo launchctl unload /Library/LaunchDaemons/com.inarms.cfrp-client.plist` | `Stop-Service cfrp-client` |
| **재시작** | `sudo systemctl restart cfrp-client` | `sudo launchctl unload ... && sudo launchctl load ...` | `Restart-Service cfrp-client` |

> **꿀팁:** 서비스 재시작 없이 새 프록시를 추가하려면 설정 경로 내의 `config.d/` 디렉토리를 사용하세요!

---

#### CLI 도구 전용
서비스를 생성하지 않고 시스템 PATH에 바이너리만 설치합니다.

**Linux & macOS:**
```bash
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode cli
```

**Windows (PowerShell 관리자 권한):**
```powershell
iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.ps1).Content -Args "-Mode cli"
```

**CLI 제거:**
- **Linux/macOS:** `curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.sh | sudo bash -s -- --mode cli`
- **Windows:** `iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.ps1).Content -Args "-Mode cli"`

#### CLI 사용 예시

`cfrp` CLI 동작은 `~/.cfrp/config`에 저장된 전역 설정에 의해 제어됩니다.

##### 전역 설정
`config` 명령어를 사용하여 도구 전체 설정을 관리합니다:
```bash
# 모든 전역 설정 나열
cfrp config ls

# 작업 모드 설정 (foreground 또는 background)
cfrp config set working_mode background

# 특정 설정 가져오기
cfrp config get working_mode
```

##### 프로세스 관리
| 명령어 | 설명 |
| :--- | :--- |
| `cfrp status` | 백그라운드 프로세스 실행 여부, PID 및 현재 프록시 활동을 표시합니다. |
| `cfrp stop` | 백그라운드 데몬을 정상적으로 종료하고 PID 파일을 정리합니다. |

##### `working_mode`에 따른 실행 동작
`working_mode` 설정(기본값: `foreground`)에 따라 바이너리 실행 방식이 결정됩니다:

1. **`foreground` 모드**:
   - 프로세스가 터미널에 연결된 상태를 유지합니다.
   - 로그가 `stdout`으로 출력됩니다.
   - 디버깅 시 또는 관리형 서비스(systemd/docker)로 실행할 때 사용합니다.

2. **`background` 모드**:
   - 프로세스가 즉시 데몬화됩니다(Linux/macOS에서는 fork, Windows에서는 숨김 실행).
   - 로그가 실행 파일 디렉토리의 `cfrp.log`로 리다이렉트됩니다.
   - 프로세스 추적을 위해 `cfrp.pid` 파일이 생성됩니다.
   - 프로세스를 중지하려면 `cfrp stop`을 사용합니다.

##### 빠른 시작 플래그
실행 시 설정을 덮어쓰거나 즉석에서 생성합니다:
```bash
# 임시 CA와 토큰을 사용하여 클라이언트 시작 (client.toml이 없으면 생성)
cfrp -c certs/ca.crt -t my_secret_token

# 특정 설정 파일 강제 사용
cfrp /path/to/my_config.toml
```

---

### 3. Docker 설치

`cfrp`를 실행하는 가장 쉬운 방법은 Docker를 사용하는 것입니다. `amd64`와 `arm64` 아키텍처용 이미지가 모두 제공됩니다.

#### 서버 배포
1. 현재 디렉토리에 `server.toml`을 작성합니다.
2. 제공된 `docker-compose.server.yml`을 사용합니다:
```bash
# compose 파일 다운로드
curl -O https://raw.githubusercontent.com/inarms/cfrp/main/docker-compose.server.yml

# 서버 시작
docker compose -f docker-compose.server.yml up -d
```
서버는 `7001`(제어), `8080`(HTTP), `8443`(HTTPS) 포트를 사용합니다.

#### 클라이언트 배포
1. 현재 디렉토리에 `client.toml`과 `conf.d` 디렉토리를 생성합니다.
2. 제공된 `docker-compose.client.yml`을 사용합니다:
```bash
# compose 파일 다운로드
curl -O https://raw.githubusercontent.com/inarms/cfrp/main/docker-compose.client.yml

# 클라이언트 시작
docker compose -f docker-compose.client.yml up -d
```
클라이언트는 호스트 머신에서 실행 중인 서비스에 쉽게 접근하기 위해 `network_mode: host`를 사용합니다.

#### 로컬 테스트 (서버 + 클라이언트)
명령어 하나로 로컬에서 `cfrp`를 테스트하려면:
```bash
docker compose up -d
```

---

## 아키텍처

1. **멀티플렉싱 터널**: 클라이언트와 서버 간의 단일 영속 연결(TCP/SSL 또는 QUIC). 커스텀 멀티플렉싱 프로토콜을 사용하여 단일 물리 연결에서 여러 논리 스트림을 처리합니다.
2. **제어 스트림**: **자체 바이너리** 직렬화 프로토콜을 사용한 명령 교환용 가상 스트림입니다.
3. **데이터 스트림**: 트래픽 브리징을 위해 온디맨드로 생성되는 동적 가상 스트림입니다. **자동 압축 감지**를 지원합니다.
4. **데이터 스플라이싱**: 외부 사용자와 로컬 서비스 간의 양방향 비동기 데이터 전달을 처리합니다.

## 시작하기

### 사전 요구 사항

- C++17 호환 컴파일러
- CMake 3.10 이상
- 의존성 관리를 위한 [vcpkg](https://github.com/microsoft/vcpkg)

### 빌드 방법

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg 설치 경로]/scripts/buildsystems/vcpkg.cmake
cmake --build .
```

## 프록시 핫 리로드

`cfrp`는 클라이언트 재시작 없이 동적인 프록시 관리를 지원합니다. 특정 디렉토리(기본값 `./conf.d`)를 모니터링하여 프록시를 즉석에서 추가, 업데이트 또는 제거할 수 있습니다.

### 핫 리로드 사용 단계:

1. **클라이언트 설정 활성화**:
   `client.toml`에 `conf_d`가 설정되어 있는지 확인합니다:
   ```toml
   [client]
   conf_d = "./conf.d"
   ```

2. **디렉토리 생성**:
   ```bash
   mkdir -p ./conf.d
   ```

3. **프록시 추가**:
   `./conf.d/` 내부에 새 `.toml` 파일을 생성합니다(예: `web.toml`):
   ```toml
   name = "my-web-service"
   type = "tcp"
   local_ip = "127.0.0.1"
   local_port = 8080
   remote_port = 8081
   ```
   클라이언트는 새 파일을 감지하고 즉시 서버에 프록시를 등록합니다.

4. **업데이트 또는 제거**:
   - **업데이트**: `web.toml` 내용을 수정하면 클라이언트가 이전 설정을 해제하고 새 설정을 등록합니다.
   - **제거**: `web.toml` 파일을 삭제하면 클라이언트가 프록시를 중지하고 서버에 알립니다.

## 설정 옵션

### Server 섹션
- `bind_addr`: 수신 대기 주소 (기본값 `0.0.0.0`).
- `bind_port`: 제어 포트 (기본값 `7000`).
- `vhost_http_port`: HTTP vhost 라우팅용 포트 (예: `80`).
- `vhost_https_port`: HTTPS SNI 라우팅용 포트 (예: `443`).
- `token`: 클라이언트와 공유되는 인증 토큰입니다.
- `allowed_ports`: 선택 사항. 허용되는 포트 또는 포트 범위 목록 (예: `[6000, "8000-9000"]`). 생략 시 모든 포트가 허용됩니다.
- `allowed_clients`: 선택 사항. 허용되는 클라이언트 이름 화이트리스트 (예: `["my-client", "office-pc"]`). 생략 시 모든 클라이언트가 허용됩니다.
- `protocol`: 사용할 프로토콜 (`tcp`, `quic`, `websocket`, `auto`). 기본값은 `auto`입니다.
- `[server.ssl]`: SSL 설정입니다.
  - `enable`: 제어 및 작업 연결에 SSL/TLS를 사용합니다 (TCP 전용).
  - `auto_generate`: 인증서가 없거나 만료된 경우 CA 및 서버 인증서를 자동 생성합니다 (기본값 `true`).
  - `cert_file`: 인증서 파일 경로 (기본값 `certs/server.crt`).
  - `key_file`: 개인키 파일 경로 (기본값 `certs/server.key`).
  - `ca_file`: CA 인증서 파일 경로 (기본값 `certs/ca.crt`).

### Client 섹션
- `server_addr`: 서버 IP 또는 호스트 이름입니다.
- `server_port`: 서버 제어 포트입니다.
- `token`: 인증 토큰입니다.
- `protocol`: 사용할 프로토콜 (`tcp`, `quic`, `websocket`, `auto`). 기본값은 `auto`입니다. `auto` 모드에서는 클라이언트가 **QUIC -> TCP -> WebSocket** 순서로 시도하며, 이전 단계가 실패하거나 타임아웃되면 다음 단계로 페일오버합니다.
- `name`: 선택 사항. 이 클라이언트의 고유 이름입니다. 생략 시 서버가 자동으로 이름을 할당하고 접미사(예: `client_1`)를 붙여 고유성을 보장합니다.
- `compression`: 모든 연결에 Zstd 압축을 사용합니다 (기본값 `true`).
- `conf_d`: 선택 사항. 동적 프록시 설정을 위한 디렉토리 경로입니다.
- `[client.ssl]`: SSL 설정입니다.
  - `enable`: SSL/TLS를 활성화합니다.
  - `verify_peer`: 서버 인증서를 검증합니다.
  - `ca_file`: 검증에 사용할 CA 인증서 경로입니다.

### Proxy 섹션 (`[[client.proxies]]`)
- `name`: 프록시의 고유 이름입니다.
- `type`: 프로토콜 유형 (`tcp`, `udp`, `http`, `https`).
- `local_ip`: 로컬 서비스 IP 또는 호스트 이름 (예: `127.0.0.1` 또는 `localhost`).
- `local_port`: 로컬 서비스 포트입니다.
- `remote_port`: 서비스를 노출할 서버 측 포트입니다 (`tcp`/`udp` 유형 시 필수).
- `custom_domains`: `http`/`https` 유형을 위한 도메인 이름 (예: `["a.com", "b.com"]`).
- `bandwidth_limit`: 프록시별 대역폭 제한 (예: `"1M"`, `"512K"`, 또는 바이트 단위 정수).

## 보안 설계: wolfSSL & QUIC

이 프로젝트는 TCP 연결에 대해 고성능 TLS를 유지하면서 최신 **QUIC** 프로토콜을 지원하기 위해 **wolfSSL**로 마이그레이션되었습니다:

- **QUIC 지원**: 최신 암호화 전송을 위해 `ngtcp2`와 `wolfSSL`을 활용합니다.
- **단방향 TLS**: 표준 TLS와 **토큰 인증**을 결합합니다.
- **서버 측**: SSL/TLS와 QUIC 모두 `cert_file` 및 `key_file`이 필요합니다.
- **클라이언트 측**: 서버의 신원만 확인하면 됩니다.

**왜 QUIC인가요?**
QUIC은 패킷 손실이 잦은 네트워크 환경에서 더 나은 성능을 제공하고, 더 빠른 연결 수립(0-RTT)이 가능하며, 기존 TCP 멀티플렉싱의 고질적인 문제인 HOL(Head-of-Line) 블로킹을 해결합니다.

## 라이선스

이 프로젝트는 Apache License 2.0 라이선스에 따라 배포됩니다. 자세한 내용은 [LICENSE](LICENSE) 파일을 참조하십시오.

이 프로젝트는 fatedier의 [frp](https://github.com/fatedier/frp) 프로젝트에서 영감을 얻었으며, 해당 프로젝트 역시 Apache License 2.0 라이선스를 따릅니다.
