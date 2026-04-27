#!/bin/bash

# cfrp macOS launchd service setup script
# Usage: sudo ./setup_service_macos.sh [server|client] [config_path]

set -e

if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)"
   exit 1
fi

MODE=$1
CONFIG=$2

if [[ -z "$MODE" ]]; then
    if [[ -f "server.toml" ]]; then
        MODE="server"
    elif [[ -f "client.toml" ]]; then
        MODE="client"
    else
        echo "Usage: $0 [server|client] [config_path]"
        exit 1
    fi
fi

if [[ -z "$CONFIG" ]]; then
    CONFIG="${MODE}.toml"
fi

if [[ ! -f "$CONFIG" ]]; then
    echo "Error: Configuration file $CONFIG not found."
    exit 1
fi

BINARY="./cfrp"
if [[ ! -f "$BINARY" ]]; then
    BINARY=$(which cfrp 2>/dev/null || true)
fi

if [[ ! -f "$BINARY" ]]; then
    echo "Error: cfrp binary not found in current directory or PATH."
    exit 1
fi

echo "Setting up cfrp as a macOS $MODE service (launchd)..."

# 1. Install binary
INSTALL_DIR="/usr/local/bin"
mkdir -p "$INSTALL_DIR"
cp "$BINARY" "$INSTALL_DIR/cfrp"
chmod +x "$INSTALL_DIR/cfrp"

# 2. Setup config
CONF_DIR="/usr/local/etc/cfrp"
mkdir -p "$CONF_DIR"
cp "$CONFIG" "$CONF_DIR/${MODE}.toml"
if [[ "$MODE" == "client" ]]; then
    mkdir -p "$CONF_DIR/config.d"
fi

# 3. Create launchd plist
LABEL="com.neesonqk.cfrp-${MODE}"
PLIST_PATH="/Library/LaunchDaemons/${LABEL}.plist"

cat <<EOF > "$PLIST_PATH"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>${LABEL}</string>
    <key>ProgramArguments</key>
    <array>
        <string>${INSTALL_DIR}/cfrp</string>
        <string>${CONF_DIR}/${MODE}.toml</string>
    </array>
    <key>WorkingDirectory</key>
    <string>${CONF_DIR}</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/var/log/cfrp-${MODE}.log</string>
    <key>StandardErrorPath</key>
    <string>/var/log/cfrp-${MODE}.error.log</string>
</dict>
</plist>
EOF

# 4. Load service
launchctl unload "$PLIST_PATH" 2>/dev/null || true
launchctl load -w "$PLIST_PATH"

echo "-----------------------------------------------"
echo "cfrp $MODE service has been installed and started."
echo "Service label: $LABEL"
echo "Config file:   $CONF_DIR/${MODE}.toml"
echo "Plist path:    $PLIST_PATH"
echo "Status check:  sudo launchctl list | grep $LABEL"
echo "Logs:          tail -f /var/log/cfrp-${MODE}.log"
echo "-----------------------------------------------"
