#!/bin/bash
# cfrp Linux Server Setup
set -e
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)"
   exit 1
fi

MODE="server"
CONFIG="server.toml"
BINARY="./cfrp"

if [[ ! -f "$CONFIG" ]]; then echo "Error: $CONFIG not found."; exit 1; fi
if [[ ! -f "$BINARY" ]]; then BINARY=$(which cfrp 2>/dev/null || true); fi
if [[ ! -f "$BINARY" ]]; then echo "Error: cfrp binary not found."; exit 1; fi

echo "Installing cfrp Server..."
cp "$BINARY" /usr/local/bin/cfrp
chmod +x /usr/local/bin/cfrp
mkdir -p /etc/cfrp
cp "$CONFIG" "/etc/cfrp/server.toml"

cat <<EOF > "/etc/systemd/system/cfrp-server.service"
[Unit]
Description=cfrp Server Service
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/cfrp /etc/cfrp/server.toml
WorkingDirectory=/etc/cfrp
Restart=always
RestartSec=5
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable cfrp-server
systemctl restart cfrp-server
echo "cfrp Server installed and started."
