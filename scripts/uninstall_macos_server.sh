#!/bin/bash
# cfrp macOS Server Uninstall
set -e
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)"
   exit 1
fi

LABEL="com.inarms.cfrp-server"
PLIST_PATH="/Library/LaunchDaemons/${LABEL}.plist"

echo "Uninstalling cfrp Server..."
launchctl unload "$PLIST_PATH" 2>/dev/null || true
rm -f "$PLIST_PATH"

rm -f /usr/local/bin/cfrp
rm -f /usr/local/etc/cfrp/server.toml
if [ -d "/usr/local/etc/cfrp" ] && [ -z "$(ls -A /usr/local/etc/cfrp)" ]; then
    rm -rf /usr/local/etc/cfrp
fi

rm -f /var/log/cfrp-server.log
rm -f /var/log/cfrp-server.error.log

echo "cfrp Server uninstalled."
