#!/bin/bash
# cfrp Linux Server Uninstall
set -e
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)"
   exit 1
fi

echo "Uninstalling cfrp Server..."
systemctl stop cfrp-server || true
systemctl disable cfrp-server || true
rm -f /etc/systemd/system/cfrp-server.service
systemctl daemon-reload

rm -f /usr/local/bin/cfrp
rm -f /etc/cfrp/server.toml
if [ -d "/etc/cfrp" ] && [ -z "$(ls -A /etc/cfrp)" ]; then
    rm -rf /etc/cfrp
fi

echo "cfrp Server uninstalled."
