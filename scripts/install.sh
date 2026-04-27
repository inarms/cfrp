#!/bin/bash

# cfrp System Installation Script
# This script installs the cfrp binary to /usr/local/bin and initializes directories.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${CYAN}Installing cfrp as a system tool...${NC}"

if [[ $EUID -ne 0 ]]; then
   echo -e "${RED}Error: This script must be run as root (use sudo)${NC}"
   exit 1
fi

# 1. Locate binary
BINARY="./cfrp"
if [[ ! -f "$BINARY" ]]; then
    # Check in build directory if running from source
    if [[ -f "./build/cfrp" ]]; then
        BINARY="./build/cfrp"
    elif [[ -f "./build_release/cfrp" ]]; then
        BINARY="./build_release/cfrp"
    fi
fi

if [[ ! -f "$BINARY" ]]; then
    echo -e "${RED}Error: cfrp binary not found in current directory or build folder.${NC}"
    echo "Please build the project first or run this script from the package directory."
    exit 1
fi

# 2. Install binary
echo -e "Copying binary to /usr/local/bin/cfrp..."
cp "$BINARY" /usr/local/bin/cfrp
chmod +x /usr/local/bin/cfrp

# 3. Initialize system-wide config directory
echo -e "Initializing /etc/cfrp..."
mkdir -p /etc/cfrp
mkdir -p /etc/cfrp/config.d

# 4. Initialize user-specific config directory for the current user (if sudo was used)
if [ -n "$SUDO_USER" ]; then
    USER_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    echo -e "Initializing $USER_HOME/.cfrp for user $SUDO_USER..."
    mkdir -p "$USER_HOME/.cfrp"
    chown -R "$SUDO_USER" "$USER_HOME/.cfrp"
fi

echo -e "${GREEN}--------------------------------------------------${NC}"
echo -e "${GREEN}cfrp has been successfully installed!${NC}"
echo -e "You can now run '${CYAN}cfrp --help${NC}' from anywhere."
echo -e "System Config: /etc/cfrp/"
echo -e "${GREEN}--------------------------------------------------${NC}"
