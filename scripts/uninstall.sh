#!/bin/bash

# cfrp Universal Uninstaller (Linux & macOS)
# Usage: curl -sSL https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.sh | sudo bash -s -- [options]
# Options:
#   --mode server    (default) Uninstall server service
#   --mode client    Uninstall client service
#   --mode cli       Uninstall only the system tool

set -e

MODE="server"
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --mode) MODE="$2"; shift ;;
        *) echo "Unknown parameter: $1"; exit 1 ;;
    esac
    shift
done

if [[ $EUID -ne 0 ]]; then
   echo "Error: This script must be run as root (use sudo)"
   exit 1
fi

OS="$(uname -s | tr '[:upper:]' '[:lower:]')"

echo "Uninstalling cfrp $MODE..."

# 1. Stop and Remove Services
if [[ "$MODE" != "cli" ]]; then
    if [[ "$OS" == "darwin" ]]; then
        LABEL="com.inarms.cfrp-${MODE}"
        PLIST_PATH="/Library/LaunchDaemons/${LABEL}.plist"
        launchctl unload "$PLIST_PATH" 2>/dev/null || true
        rm -f "$PLIST_PATH"
        rm -f "/var/log/cfrp-${MODE}.log" "/var/log/cfrp-${MODE}.error.log"
    else
        systemctl stop "cfrp-${MODE}" 2>/dev/null || true
        systemctl disable "cfrp-${MODE}" 2>/dev/null || true
        rm -f "/etc/systemd/system/cfrp-${MODE}.service"
        systemctl daemon-reload
    fi
fi

# 2. Remove Configs
if [[ "$OS" == "darwin" ]]; then
    CONF_DIR="/usr/local/etc/cfrp"
    rm -f "$CONF_DIR/${MODE}.toml"
    if [[ "$MODE" == "client" ]]; then rm -rf "$CONF_DIR/config.d"; fi
    if [ -d "$CONF_DIR" ] && [ -z "$(ls -A $CONF_DIR)" ]; then rm -rf "$CONF_DIR"; fi
else
    CONF_DIR="/etc/cfrp"
    rm -f "$CONF_DIR/${MODE}.toml"
    if [[ "$MODE" == "client" ]]; then rm -rf "$CONF_DIR/config.d"; fi
    if [ -d "$CONF_DIR" ] && [ -z "$(ls -A $CONF_DIR)" ]; then rm -rf "$CONF_DIR"; fi
fi

# 3. Remove Binary (only if both services are gone or if uninstalling CLI)
if [[ "$MODE" == "cli" ]] || { [ ! -f "/etc/systemd/system/cfrp-server.service" ] && [ ! -f "/etc/systemd/system/cfrp-client.service" ] && [ ! -f "/Library/LaunchDaemons/com.inarms.cfrp-server.plist" ] && [ ! -f "/Library/LaunchDaemons/com.inarms.cfrp-client.plist" ]; }; then
    echo "Removing binary /usr/local/bin/cfrp..."
    rm -f /usr/local/bin/cfrp
fi

echo "--------------------------------------------------"
echo "cfrp $MODE has been uninstalled."
echo "--------------------------------------------------"
