#!/bin/bash

# cfrp Universal Installer (Linux & macOS)
# This script auto-downloads and installs the latest cfrp.
# Usage: curl -sSL https://raw.githubusercontent.com/neesonqk/cfrp/main/scripts/install.sh | sudo bash -s -- [options]
# Options:
#   --mode server    (default) Install as server service
#   --mode client    Install as client service
#   --mode cli       Install only as system tool

set -e

# --- Configuration ---
REPO="inarms/cfrp"
MODE="server"
VERSION="latest"

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --mode) MODE="$2"; shift ;;
        --version) VERSION="$2"; shift ;;
        *) echo "Unknown parameter: $1"; exit 1 ;;
    esac
    shift
done

# --- Check Environment ---
if [[ $EUID -ne 0 ]]; then
   echo "Error: This script must be run as root (use sudo)"
   exit 1
fi

OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"
case $ARCH in
    x86_64) ARCH="amd64" ;;
    aarch64|arm64) ARCH="arm64" ;;
    *) echo "Unsupported architecture: $ARCH"; exit 1 ;;
esac

if [[ "$OS" == "darwin" ]]; then
    PLATFORM="macos"
    SERVICE_TYPE="launchd"
elif [[ "$OS" == "linux" ]]; then
    PLATFORM="linux"
    SERVICE_TYPE="systemd"
else
    echo "Unsupported OS: $OS"
    exit 1
fi

# --- Fetch Latest Release ---
echo "Fetching $VERSION version info for $PLATFORM-$ARCH..."
if [[ "$VERSION" == "latest" ]]; then
    URL="https://api.github.com/repos/$REPO/releases/latest"
else
    URL="https://api.github.com/repos/$REPO/releases/tags/$VERSION"
fi

# Detect download URL
# Example name: cfrp-server-linux-amd64.tar.gz
PACKAGE_MODE=$MODE
if [[ "$MODE" == "cli" ]]; then PACKAGE_MODE="server"; fi # CLI comes in either, we use server pack

ASSET_NAME="cfrp-${PACKAGE_MODE}-${PLATFORM}-${ARCH}.tar.gz"
DOWNLOAD_URL=$(curl -s $URL | grep "browser_download_url" | grep "$ASSET_NAME" | cut -d '"' -f 4)

if [[ -z "$DOWNLOAD_URL" ]]; then
    echo "Error: Could not find download URL for $ASSET_NAME"
    exit 1
fi

# --- Download and Extract ---
TMP_DIR=$(mktemp -d)
echo "Downloading from $DOWNLOAD_URL..."
curl -L "$DOWNLOAD_URL" -o "$TMP_DIR/cfrp.tar.gz"
tar -xzf "$TMP_DIR/cfrp.tar.gz" -C "$TMP_DIR"

# --- Install Binary ---
echo "Installing binary to /usr/local/bin..."
cp "$TMP_DIR/cfrp" /usr/local/bin/cfrp
chmod +x /usr/local/bin/cfrp

# --- Setup Service or Tool ---
if [[ "$MODE" == "cli" ]]; then
    echo "CLI tool installed. Initializing config directories..."
    mkdir -p /etc/cfrp
    echo "Installation complete. Run 'cfrp --help' to get started."
    exit 0
fi

# If mode is server/client, we run the internal setup logic
cd "$TMP_DIR"
if [[ "$OS" == "darwin" ]]; then
    # Use the setup_service_macos.sh logic directly
    LABEL="com.neesonqk.cfrp-${MODE}"
    CONF_DIR="/usr/local/etc/cfrp"
    mkdir -p "$CONF_DIR"
    cp "${MODE}.toml" "$CONF_DIR/"
    if [[ "$MODE" == "client" ]]; then mkdir -p "$CONF_DIR/config.d"; fi
    
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
        <string>/usr/local/bin/cfrp</string>
        <string>${CONF_DIR}/${MODE}.toml</string>
    </array>
    <key>WorkingDirectory</key>
    <string>${CONF_DIR}</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
</dict>
</plist>
EOF
    launchctl unload "$PLIST_PATH" 2>/dev/null || true
    launchctl load -w "$PLIST_PATH"
else
    # Linux systemd logic
    mkdir -p /etc/cfrp
    cp "${MODE}.toml" "/etc/cfrp/"
    if [[ "$MODE" == "client" ]]; then mkdir -p /etc/cfrp/config.d; fi
    
    cat <<EOF > "/etc/systemd/system/cfrp-${MODE}.service"
[Unit]
Description=cfrp ${MODE} service
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
    systemctl daemon-reload
    systemctl enable "cfrp-${MODE}"
    systemctl restart "cfrp-${MODE}"
fi

CONF_FILE="$(if [[ "$OS" == "darwin" ]]; then echo "/usr/local/etc/cfrp/${MODE}.toml"; else echo "/etc/cfrp/${MODE}.toml"; fi)"

rm -rf "$TMP_DIR"
echo -e "${GREEN}--------------------------------------------------${NC}"
echo -e "${GREEN}cfrp $MODE service installed and started successfully!${NC}"
echo -e "Binary:      ${CYAN}/usr/local/bin/cfrp${NC}"
echo -e "Config File: ${CYAN}$CONF_FILE${NC}"
echo -e "Status:      ${CYAN}$(if [[ "$OS" == "darwin" ]]; then echo "sudo launchctl list | grep com.neesonqk.cfrp-${MODE}"; else echo "systemctl status cfrp-${MODE}"; fi)${NC}"
echo -e "${GREEN}--------------------------------------------------${NC}"
