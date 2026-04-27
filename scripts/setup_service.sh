#!/bin/bash

# cfrp systemd service setup script
# Usage: sudo ./setup_service.sh [server|client] [config_path]

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

echo "Setting up cfrp as a $MODE service..."

# 1. Install binary
cp "$BINARY" /usr/local/bin/cfrp
chmod +x /usr/local/bin/cfrp

# 2. Setup config
mkdir -p /etc/cfrp
cp "$CONFIG" "/etc/cfrp/${MODE}.toml"
if [[ "$MODE" == "client" ]]; then
    mkdir -p /etc/cfrp/config.d
fi

# 3. Create systemd unit
SERVICE_NAME="cfrp-${MODE}"
CAT_CMD="cat"
$CAT_CMD <<EOF > "/etc/systemd/system/${SERVICE_NAME}.service"
[Unit]
Description=cfrp $MODE service
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/cfrp /etc/cfrp/${MODE}.toml
WorkingDirectory=/etc/cfrp
Restart=always
RestartSec=5
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
EOF

# 4. Start service
systemctl daemon-reload
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"

echo "-----------------------------------------------"
echo "cfrp $MODE service has been installed and started."
echo "Service name: $SERVICE_NAME"
echo "Config file:  /etc/cfrp/${MODE}.toml"
echo "Status check: systemctl status $SERVICE_NAME"
echo "Logs check:   journalctl -u $SERVICE_NAME -f"
echo "-----------------------------------------------"
