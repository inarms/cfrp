# cfrp - C++ 高性能リバースプロキシ

English | [简体中文](./README_zh.md) | 日本語 | [한국어](./README_ko.md)

Standalone Asio を使用して C++17 で実装された高性能な非同期リバースプロキシです。
`cfrp` は、NAT やファイアウォールの背後にあるローカルサービスをインターネットに公開するための、軽量で効率的な代替手段として設計されており、人気のプロジェクト [fatedier/frp](https://github.com/fatedier/frp) からインスピレーションを得ています。

## 特徴

- **高性能**: 非ブロッキング非同期 I/O のための Standalone Asio をベースに構築。
- **ゼロ設定 (即時利用可能なセキュリティ)**: 証明書が不足している場合、SSL/QUIC 証明書と CA チェーンを自動的に生成します。手動での OpenSSL コマンドは不要です。
- **TCP/QUIC 上のマルチプレクシング**: 独自に構築された軽量なマルチプレクシングプロトコルを使用して、すべてのトラフィックを **単一の接続** に統合します。従来の TCP と最新の **QUIC (ngtcp2 経由)** プロトコルの両方をサポート。
- **自動プロトコルモード**:
  - **サーバー**: 同じポートで TCP と QUIC の両方のクライアントを自動的に処理。
  - **クライアント**: 最初に QUIC 接続を試行し、必要に応じて自動的に TCP にフェイルオーバー。
- **セキュリティ**: オプションの **SSL/TLS** 暗号化と **トークンベースの認証**。高性能な暗号化と最新の QUIC サポートのために **wolfSSL** を使用。
- **帯域幅効率**: 制御チャネルとデータチャネルの両方でオプションの **Zstd 圧縮** をサポートし、サーバー側で自動的に検出。
- **耐障害性の高いクライアント**: サーバーが到達不能になった場合、指数バックオフを伴う自動再接続を実行。終了時の **クリーンアップ** をサポート。
- **動的なプロキシ**: 単一の制御接続上で複数の **TCP**、**UDP**、**HTTP**、および **HTTPS (SNI)** プロキシをサポート。`conf.d` ディレクトリを介した **ホットリロード** に対応。
- **プロトコルの柔軟性**: トンネルの基盤として **TCP**、**QUIC**、および **WebSocket** をサポート。WebSocket サポートにより、ファイアウォールの通過や CDN (Cloudflare など) の統合が可能。
- **VHost サポート**: ドメインベースのルーティングを使用して、複数の Web サービスが同じ HTTP (80) または HTTPS (443) ポートを共有可能。
- **DNS 解決**: `local_ip` でホスト名 (例: `localhost` や Docker サービス名) をサポート。
- **トラフィック制御**: ネットワークの飽和を防ぐためのプロキシごとの帯域幅制限。
- **軽量**: 最小限の依存関係 (`asio`, `tomlplusplus`, `wolfssl`, `ngtcp2`)。最小限のオーバーヘッドのためのコンパクトな **独自のバイナリプロトコル** を使用。
- **クリーンな設定**: 読みやすいサーバーおよびクライアント設定のための TOML を採用。

## ゼロ設定セキュリティ

`cfrp` はトンネルのセキュリティ保護を容易にします。QUIC または TLS を有効にすると：
1. **自動 PKI**: 証明書がない、または期限切れの場合、サーバーはルート CA とサーバー証明書を自動的に生成します。
2. **自動クリーンアップ**: 証明書は `certs/` ディレクトリに保存され、有効期限が近づくと自動的に更新されます。
3. **簡単な配布**: 生成された `certs/ca.crt` をクライアントデバイスにコピーするだけで、完全なピア検証が有効になります。

## インストール

### 1. 手動インストール

#### ダウンロード手順
1. **[GitHub リリース](https://github.com/inarms/cfrp/releases)** ページにアクセスします。
2. プラットフォームとアーキテクチャに対応する圧縮ファイルをダウンロードします。
3. 内容を解凍します：
   - **サーバーパッケージ**: `cfrp` バイナリ、`server.toml`、および `setup.sh`/`uninstall.sh` (または `.ps1`) が含まれます。
   - **クライアントパッケージ**: `cfrp` バイナリ、`client.toml`、`config.d/`、および `setup.sh`/`uninstall.sh` (または `.ps1`) が含まれます。

#### 設定ファイルの自動選択
**注意: `server.toml` または `client.toml` が現在のディレクトリに存在する場合、それらが絶対的に優先されます。すべてのコマンドラインの位置引数やフラグ ( `-c` や `-t` など) は無視されます。**

設定ファイルが存在しない場合、アプリケーションは以下のロジックに従います：
1. **位置引数**: パスが提供されている場合 (例: `./cfrp my.toml`)、そのファイルを使用します。
2. **フラグ**: `-c` と `-t` が提供されている場合、それらの値を使用して `client.toml` を生成します。
3. **デフォルト**: それ以外の場合、デフォルトの `server.toml` を生成し、サーバーとして起動します。

既存ファイルの検索順序は以下の通りです：
1. **`server.toml`**: 見つかった場合、**サーバー**として起動。
2. **`client.toml`**: 見つかった場合、**クライアント**として起動。

#### 手動使用例

##### 1. サーバーとして起動
デフォルトの `server.toml` を使用してサーバーを実行する場合：
```bash
./cfrp
```
または、カスタム設定を指定：
```bash
./cfrp my_server.toml
```

**`server.toml`** の例：
```toml
[server]
bind_addr = "0.0.0.0"
bind_port = 7001
token = "your_secret_token"
protocol = "auto" # TCP と QUIC を同時にサポート

[server.ssl]
enable = true
auto_generate = true # CA と証明書を自動生成
```

##### 2. クライアントとして起動
デフォルトの `client.toml` を使用してクライアントを実行する場合：
```bash
./cfrp
```

**`client.toml`** の例：
```toml
[client]
server_addr = "your_server_ip"
server_port = 7001
token = "your_secret_token"
name = "my-client"
protocol = "auto" # 最初に QUIC を試し、TCP にフェイルオーバー

[[client.proxies]]
name = "ssh"
type = "tcp"
local_ip = "127.0.0.1"
local_port = 22
remote_port = 6000
```

##### 3. クイッククライアントセットアップ (自動生成)
サーバーの CA 証明書 (`ca.crt`) とトークンがある場合、クライアントを素早く開始できます。`client.toml` が存在しない場合、`cfrp` は自動的に生成します：
```bash
./cfrp -c certs/ca.crt -t your_secret_token
```
- `server.toml` が存在する場合でも、**クライアントモードを強制**します。
- `client.toml` が **不足** している場合、提供された検証用 CA とトークンを使用してデフォルトのファイルを生成します。
- `client.toml` が **存在** する場合、既存の設定を使用します (`-c` と `-t` パラメータは無視されます)。

##### 4. サービスへのアクセス
トンネルが確立されたら、サーバーのパブリック IP を介してローカルサービスにアクセスします：
```bash
ssh -p 6000 user@your_server_ip
```

---

### 2. スクリプトインストール

#### サーバーインストール
バイナリをインストールし、バックグラウンドサービス (systemd/launchd/Windows サービス) をセットアップします。

**Linux & macOS:**
```bash
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode server
```
- **設定パス (Linux):** `/etc/cfrp/server.toml`
- **設定パス (macOS):** `/usr/local/etc/cfrp/server.toml`

**Windows (PowerShell 管理者):**
```powershell
iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.ps1).Content -Args "-Mode server"
```
- **設定パス:** `C:\Program Files\cfrp\server.toml`

**サーバーのアンインストール:**
- **Linux/macOS:** `curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.sh | sudo bash -s -- --mode server`
- **Windows:** `iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.ps1).Content -Args "-Mode server"`

#### サーバー管理
| アクション | Linux (systemd) | macOS (launchd) | Windows (PowerShell) |
| :--- | :--- | :--- | :--- |
| **起動** | `sudo systemctl start cfrp-server` | `sudo launchctl load -w /Library/LaunchDaemons/com.inarms.cfrp-server.plist` | `Start-Service cfrp-server` |
| **停止** | `sudo systemctl stop cfrp-server` | `sudo launchctl unload /Library/LaunchDaemons/com.inarms.cfrp-server.plist` | `Stop-Service cfrp-server` |
| **ステータス** | `systemctl status cfrp-server` | `sudo launchctl list \| grep cfrp-server` | `Get-Service cfrp-server` |
| **ログ** | `journalctl -u cfrp-server -f` | `tail -f /var/log/cfrp-server.log` | `C:\Program Files\cfrp\cfrp.log` を確認 |

---

#### クライアントインストール
バイナリをインストールし、バックグラウンドサービスをセットアップします。

**Linux & macOS:**
```bash
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode client
```
- **設定パス (Linux):** `/etc/cfrp/client.toml`
- **設定パス (macOS):** `/usr/local/etc/cfrp/client.toml`

**Windows (PowerShell 管理者):**
```powershell
iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.ps1).Content -Args "-Mode client"
```
- **設定パス:** `C:\Program Files\cfrp\client.toml`

**クライアントのアンインストール:**
- **Linux/macOS:** `curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.sh | sudo bash -s -- --mode client`
- **Windows:** `iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.ps1).Content -Args "-Mode client"`

#### クライアント管理
| アクション | Linux (systemd) | macOS (launchd) | Windows (PowerShell) |
| :--- | :--- | :--- | :--- |
| **起動** | `sudo systemctl start cfrp-client` | `sudo launchctl load -w /Library/LaunchDaemons/com.inarms.cfrp-client.plist` | `Start-Service cfrp-client` |
| **停止** | `sudo systemctl stop cfrp-client` | `sudo launchctl unload /Library/LaunchDaemons/com.inarms.cfrp-client.plist` | `Stop-Service cfrp-client` |
| **再読み込み** | `sudo systemctl restart cfrp-client` | `sudo launchctl unload ... && sudo launchctl load ...` | `Restart-Service cfrp-client` |

> **プロのヒント:** `config.d/` ディレクトリ (設定パス内) を使用して、サービスを再起動せずに新しいプロキシを追加できます。

---

#### CLI ツールのみ
サービスを作成せずにバイナリをシステム PATH にインストールします。

**Linux & macOS:**
```bash
curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.sh | sudo bash -s -- --mode cli
```

**Windows (PowerShell 管理者):**
```powershell
iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/install.ps1).Content -Args "-Mode cli"
```

**CLI のアンインストール:**
- **Linux/macOS:** `curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.sh | sudo bash -s -- --mode cli`
- **Windows:** `iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.ps1).Content -Args "-Mode cli"`

#### CLI 使用例

`cfrp` CLI の動作は、`~/.cfrp/config` に保存されているグローバル設定によって制御されます。

##### グローバル設定
`config` コマンドを使用してツール全体の設定を管理します：
```bash
# すべてのグローバル設定を表示
cfrp config ls

# 動作モードを設定 (foreground または background)
cfrp config set working_mode background

# 特定の設定を取得
cfrp config get working_mode
```

##### プロセス管理
| コマンド | 説明 |
| :--- | :--- |
| `cfrp status` | バックグラウンドプロセスが実行中かどうか、PID、および現在のプロキシアクティビティを表示します。 |
| `cfrp stop` | バックグラウンドデーモンを正常に終了し、PID ファイルをクリーンアップします。 |

##### `working_mode` による実行動作
`working_mode` 設定 (デフォルト: `foreground`) は、起動時のバイナリの動作を決定します：

1. **`foreground` モード**:
   - プロセスはターミナルに接続されたままになります。
   - ログは `stdout` に出力されます。
   - デバッグ時や管理サービス (systemd/docker) として実行する場合に使用します。

2. **`background` モード**:
   - プロセスは即座に自身を「デーモン化」します (Linux/macOS ではフォーク、Windows では非表示で生成)。
   - ログは実行可能ディレクトリの `cfrp.log` にリダイレクトされます。
   - プロセスを追跡するために `cfrp.pid` ファイルが作成されます。
   - プロセスを停止するには `cfrp stop` を使用します。

##### クイックスタートフラグ
実行時に設定を上書きまたは動的に生成します：
```bash
# 一時的な CA とトークンを使用してクライアントを起動 (client.toml がない場合は生成)
cfrp -c certs/ca.crt -t my_secret_token

# 特定の設定ファイルを強制的に使用
cfrp /path/to/my_config.toml
```

---

### 3. Docker インストール

`cfrp` を実行する最も簡単な方法は Docker を使用することです。イメージは `amd64` と `arm64` アーキテクチャの両方で利用可能です。

#### サーバー展開
1. 現在のディレクトリに `server.toml` を作成します。
2. 提供されている `docker-compose.server.yml` を使用します：
```bash
# compose ファイルをダウンロード
curl -O https://raw.githubusercontent.com/inarms/cfrp/main/docker-compose.server.yml

# サーバーを起動
docker compose -f docker-compose.server.yml up -d
```
サーバーは、ポート `7001` (制御)、`8080` (HTTP)、および `8443` (HTTPS) で利用可能になります。

#### クライアント展開
1. 現在のディレクトリに `client.toml` と `conf.d` ディレクトリを作成します。
2. 提供されている `docker-compose.client.yml` を使用します：
```bash
# compose ファイルをダウンロード
curl -O https://raw.githubusercontent.com/inarms/cfrp/main/docker-compose.client.yml

# クライアントを起動
docker compose -f docker-compose.client.yml up -d
```
クライアントは `network_mode: host` を使用して、ホストマシン上で実行されているサービスに簡単にアクセスできます。

#### ローカルテスト (サーバー + クライアント)
単一のコマンドで `cfrp` をローカルでテストする場合：
```bash
docker compose up -d
```

---

## アーキテクチャ

1. **マルチプレクス・トンネル**: クライアントとサーバー間の単一の永続的な接続 (TCP/SSL または QUIC)。カスタムマルチプレクシングプロトコルを使用して、この単一の物理接続上で複数の論理ストリームを処理します。
2. **制御ストリーム**: **独自のバイナリ** シリアル化プロトコルを使用したコマンド交換用の仮想ストリーム。
3. **データストリーム**: トラフィックをブリッジするためにオンデマンドで確立される動的な仮想ストリーム。**自動圧縮検出** をサポート。
4. **データスプライシング**: 外部ユーザーとローカルサービス間の双方向非同期データ転送。

## はじめに

### 前提条件

- C++17 互換のコンパイラ
- CMake 3.10+
- 依存関係管理のための [vcpkg](https://github.com/microsoft/vcpkg)

### ビルド

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkgへのパス]/scripts/buildsystems/vcpkg.cmake
cmake --build .
```

## プロキシ・ホットリロード

`cfrp` はクライアントを再起動せずに動的なプロキシ管理をサポートします。ディレクトリ (デフォルトは `./conf.d`) を監視することで、プロキシを即座に追加、更新、または削除できます。

### ホットリロードの使用手順:

1. **クライアント設定で有効にする**:
   `client.toml` で `conf_d` が設定されていることを確認します：
   ```toml
   [client]
   conf_d = "./conf.d"
   ```

2. **ディレクトリを作成する**:
   ```bash
   mkdir -p ./conf.d
   ```

3. **プロキシを追加する**:
   `./conf.d/` 内に新しい `.toml` ファイルを作成します (例: `web.toml`)：
   ```toml
   name = "my-web-service"
   type = "tcp"
   local_ip = "127.0.0.1"
   local_port = 8080
   remote_port = 8081
   ```
   クライアントは新しいファイルを検出し、即座にサーバーにプロキシを登録します。

4. **更新または削除**:
   - **更新**: `web.toml` のフィールドを変更します。クライアントは古い設定を解除し、新しい設定を登録します。
   - **削除**: `web.toml` を削除します。クライアントはプロキシを停止し、サーバーに通知します。

## 設定オプション

### Server セクション
- `bind_addr`: リッスンするアドレス (デフォルト `0.0.0.0`)。
- `bind_port`: 制御ポート (デフォルト `7000`)。
- `vhost_http_port`: HTTP vhost ルーティング用のポート (例: `80`)。
- `vhost_https_port`: HTTPS SNI ルーティング用のポート (例: `443`)。
- `token`: クライアントと共有される認証トークン。
- `allowed_ports`: オプション。許可されるポートまたはポート範囲のリスト (例: `[6000, "8000-9000"]`)。省略した場合、すべてのポートが許可されます。
- `allowed_clients`: オプション。許可されるクライアント名のホワイトリスト (例: `["my-client", "office-pc"]`)。省略した場合、すべてのクライアント名が許可されます。
- `protocol`: 使用するプロトコル (`tcp`, `quic`, `websocket`, または `auto`)。デフォルトは `auto`。
- `[server.ssl]`: SSL 設定。
  - `enable`: 制御接続およびワーク接続で SSL/TLS を有効にする (TCP のみ)。
  - `auto_generate`: 証明書がない、または期限切れの場合、CA とサーバー証明書を自動生成する (デフォルト `true`)。
  - `cert_file`: 証明書ファイルのパス (デフォルト `certs/server.crt`)。
  - `key_file`: 秘密鍵ファイルのパス (デフォルト `certs/server.key`)。
  - `ca_file`: CA 証明書ファイルのパス (デフォルト `certs/ca.crt`)。

### Client セクション
- `server_addr`: サーバーの IP またはホスト名。
- `server_port`: サーバーの制御ポート。
- `token`: 認証トークン。
- `protocol`: 使用するプロトコル (`tcp`, `quic`, `websocket`, または `auto`)。デフォルトは `auto`。`auto` モードでは、クライアントは **QUIC -> TCP -> WebSocket** の順にプロトコルを試行し、前のプロトコルが失敗またはタイムアウトした場合は次のプロトコルにフェイルオーバーします。
- `name`: オプション。このクライアントのユニークな名前。省略した場合、サーバーは自動的に名前を割り当て、サフィックス (例: `client_1`) を追加して一意性を確保します。
- `compression`: すべての接続で Zstd 圧縮を有効にする (デフォルト `true`)。
- `conf_d`: オプション。動的なプロキシ設定のためのディレクトリパス。
- `[client.ssl]`: SSL 設定。
  - `enable`: SSL/TLS を有効にする。
  - `verify_peer`: サーバー証明書を検証する。
  - `ca_file`: 検証用の CA 証明書のパス。

### Proxy セクション (`[[client.proxies]]`)
- `name`: プロキシのユニークな名前。
- `type`: プロトコルタイプ (`tcp`, `udp`, `http`, または `https`)。
- `local_ip`: ローカルサービスの IP またはホスト名 (例: `127.0.0.1` または `localhost`)。
- `local_port`: ローカルサービスのポート。
- `remote_port`: サービスを公開するサーバー上のポート (`tcp`/`udp` で必要)。
- `custom_domains`: `http`/`https` タイプのドメイン名 (例: `["a.com", "b.com"]`)。
- `bandwidth_limit`: プロキシの帯域幅制限 (例: `"1M"`, `"512K"`, または整数としてのバイト)。

## セキュリティ設計: wolfSSL & QUIC

このプロジェクトは、TCP 接続に高性能な TLS を維持しながら、最新の **QUIC** プロトコルをサポートするために **wolfSSL** に移行しました：

- **QUIC サポート**: 最先端の暗号化転送のために `ngtcp2` と `wolfSSL` を活用。
- **一方向 TLS**: 標準 TLS と **トークン認証** を組み合わせ。
- **サーバー側**: SSL/TLS と QUIC の両方で `cert_file` と `key_file` が必要。
- **クライアント側**: サーバーの身元を確認するだけで済みます。

**なぜ QUIC なのか？**
QUIC は、パケット損失が多いネットワーク環境でより優れたパフォーマンスを提供し、接続確立が速く (0-RTT)、従来の TCP マルチプレクシングで共通の課題であるヘッドオブラインブロッキングを排除します。

## ライセンス

このプロジェクトは Apache License 2.0 の下でライセンスされています。詳細は [LICENSE](LICENSE) ファイルを参照してください。

このプロジェクトは、同じく Apache License 2.0 の下でライセンスされている fatedier によるオリジナルの [frp](https://github.com/fatedier/frp) プロジェクトからインスピレーションを得ています。
