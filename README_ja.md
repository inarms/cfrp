# cfrp - C++ 高性能リバースプロキシ

English | [简体中文](./README_zh.md) | 日本語 | [한국어](./README_ko.md)

`cfrp` は、Standalone Asio を使用して C++17 で構築された高性能な非同期リバースプロキシです。[fatedier/frp](https://github.com/fatedier/frp) からインスピレーションを得ており、NAT やファイアウォールの背後にあるローカルサービスを安全かつ効率的にインターネットへ公開します。

## 特徴
- **高性能:** Standalone Asio による非ブロッキング非同期 I/O。
- **マルチプロトコル:** TCP, UDP, HTTP, HTTPS (SNI), QUIC, WebSocket をサポート。
- **セキュリティ内蔵:** SSL/QUIC 証明書の自動生成とトークン認証をサポート。
- **高効率:** マルチプレクス・トンネルとオプションの Zstd 圧縮。
- **動的設定:** `conf.d` ディレクトリを介したプロキシのホットリロード。
- **クロスプラットフォーム:** Windows, Linux, macOS で動作。

## クイックスタート (Docker)
Docker を使用するのが最も簡単な方法です：
```bash
# サーバーとクライアントをローカルで起動
docker compose up -d
```

## インストール

### スクリプトインストール (Linux/macOS)
```bash
# サーバー
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode server

# クライアント
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode client
```
*(Windows PowerShell スクリプトについては [Releases](https://github.com/inarms/cfrp/releases) を参照)*

### 手動設定
基本的な `server.toml`:
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

基本的な `client.toml`:
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

`cfrp server.toml` または `cfrp client.toml` で実行します。

## ソースからビルド
```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkgへのパス]/scripts/buildsystems/vcpkg.cmake
cmake --build .
```

## ライセンス
Apache License 2.0。[fatedier/frp](https://github.com/fatedier/frp) からインスピレーションを得ています。
