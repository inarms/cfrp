#!/bin/bash
# cfrp macOS Client Uninstall
set -e
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)"
   exit 1
fi

LABEL="com.neesonqk.cfrp-client"
PLIST_PATH="/Library/LaunchDaemons/${LABEL}.plist"

echo "Uninstalling cfrp Client..."
launchctl unload "$PLIST_PATH" 2>/dev/null || true
rm -f "$PLIST_PATH"

rm -f /usr/local/bin/cfrp
rm -f /usr/local/etc/cfrp/client.toml
rm -rf /usr/local/etc/cfrp/config.d
if [ -d "/usr/local/etc/cfrp" ] && [ -z "$(ls -A /usr/local/etc/cfrp)" ]; then
    rm -rf /usr/local/etc/cfrp
fi

rm -f /var/log/cfrp-client.log
rm -f /var/log/cfrp-client.error.log

echo "cfrp Client uninstalled."
