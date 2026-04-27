#!/bin/bash
# cfrp Linux Client Uninstall
set -e
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)"
   exit 1
fi

echo "Uninstalling cfrp Client..."
systemctl stop cfrp-client || true
systemctl disable cfrp-client || true
rm -f /etc/systemd/system/cfrp-client.service
systemctl daemon-reload

rm -f /usr/local/bin/cfrp
rm -f /etc/cfrp/client.toml
rm -rf /etc/cfrp/config.d
if [ -d "/etc/cfrp" ] && [ -z "$(ls -A /etc/cfrp)" ]; then
    rm -rf /etc/cfrp
fi

echo "cfrp Client uninstalled."
