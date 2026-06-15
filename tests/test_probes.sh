#!/bin/bash

# Configuration
SERVER_IP="one.fufutechnologies.com"
SERVER_PORT=7001
INTERVAL=1  # Delay between checks (seconds)

echo "=== CFRp Continuous Probing Started ==="
echo "Target: $SERVER_IP:$SERVER_PORT"
echo "Interval: $INTERVAL second(s)"
echo "Press [CTRL+C] to stop..."
echo "----------------------------------------"

while true; do
    TIMESTAMP=$(date +"%Y-%m-%d %H:%M:%S")

    # 1. Plain TCP Probe (Instant check)
    echo "[$TIMESTAMP] Sending plain TCP probe..."
    echo "PING" | nc -w 1 $SERVER_IP $SERVER_PORT > /dev/null 2>&1

    # 2. OPTIONAL: Stalled Connection (Uncomment to enable)
    # echo "[$TIMESTAMP] Starting stalled connection..."
    # nc -w 12 $SERVER_IP $SERVER_PORT > /dev/null 2>&1 &

    # 3. OPTIONAL: TLS Handshake without login (Uncomment to enable)
    # echo "[$TIMESTAMP] Starting TLS handshake probe..."
    # (sleep 12) | openssl s_client -connect $SERVER_IP:$SERVER_PORT -servername $SERVER_IP -tls1_3 -quiet > /dev/null 2>&1 &

    sleep $INTERVAL
done
