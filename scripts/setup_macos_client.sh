#!/bin/bash
# cfrp macOS Client Setup
set -e
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)"
   exit 1
fi

CONFIG="client.toml"
BINARY="./cfrp"
LABEL="com.neesonqk.cfrp-client"
INSTALL_DIR="/usr/local/bin"
CONF_DIR="/usr/local/etc/cfrp"
PLIST_PATH="/Library/LaunchDaemons/${LABEL}.plist"

if [[ ! -f "$CONFIG" ]]; then echo "Error: $CONFIG not found."; exit 1; fi
if [[ ! -f "$BINARY" ]]; then BINARY=$(which cfrp 2>/dev/null || true); fi
if [[ ! -f "$BINARY" ]]; then echo "Error: cfrp binary not found."; exit 1; fi

echo "Installing cfrp Client..."
mkdir -p "$INSTALL_DIR" "$CONF_DIR/config.d"
cp "$BINARY" "$INSTALL_DIR/cfrp"
cp "$CONFIG" "$CONF_DIR/client.toml"

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
        <string>${CONF_DIR}/client.toml</string>
    </array>
    <key>WorkingDirectory</key>
    <string>${CONF_DIR}</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/var/log/cfrp-client.log</string>
    <key>StandardErrorPath</key>
    <string>/var/log/cfrp-client.error.log</string>
</dict>
</plist>
EOF

launchctl unload "$PLIST_PATH" 2>/dev/null || true
launchctl load -w "$PLIST_PATH"
echo "cfrp Client installed and started."
