#!/bin/bash
set -e

SERVER_IP=127.0.0.1
SERVER_PORT=8800

cleanup() {
    echo "Cleaning up..."
    kill $SERVER_PID $CLIENT1_PID $CLIENT2_PID $CLIENT3_PID 2>/dev/null || true
    wait 2>/dev/null || true
    ip link delete tun_a 2>/dev/null || true
    ip link delete tun_b 2>/dev/null || true
    ip link delete tun_c 2>/dev/null || true
    echo "Done"
}
trap cleanup EXIT INT TERM

# Build
cmake --build build/ 2>&1 | tail -3

# Start server
echo "=== Starting server ==="
./build/p2p_server $SERVER_PORT > /tmp/server.log 2>&1 &
SERVER_PID=$!
sleep 0.5

# Start client A
echo "=== Starting client A (10.0.0.1) ==="
./build/p2p_client a 10.0.0.1 $SERVER_IP $SERVER_PORT > /tmp/client_a.log 2>&1 &
CLIENT1_PID=$!
sleep 2

# Start client B
echo "=== Starting client B (10.0.0.2) ==="
./build/p2p_client b 10.0.0.2 $SERVER_IP $SERVER_PORT > /tmp/client_b.log 2>&1 &
CLIENT2_PID=$!
sleep 4

echo ""
echo "=== Client A log (tail) ==="
tail -20 /tmp/client_a.log
echo ""
echo "=== Client B log (tail) ==="
tail -20 /tmp/client_b.log

# Test A <-> B
echo ""
echo "=== Testing ping A -> B (10.0.0.2) ==="
ping -c 3 -W 2 -I tun_a 10.0.0.2 2>&1 || echo "PING FAILED"
echo ""
echo "=== Testing ping B -> A (10.0.0.1) ==="
ping -c 3 -W 2 -I tun_b 10.0.0.1 2>&1 || echo "PING FAILED"

# Now start client C
echo ""
echo "=== Starting client C (10.0.0.3) ==="
./build/p2p_client c 10.0.0.3 $SERVER_IP $SERVER_PORT > /tmp/client_c.log 2>&1 &
CLIENT3_PID=$!
sleep 6

echo ""
echo "=== Client A log (tail all) ==="
cat /tmp/client_a.log
echo ""
echo "=== Client B log (tail all) ==="
cat /tmp/client_b.log
echo ""
echo "=== Client C log ==="
cat /tmp/client_c.log

# Test with 3 clients
echo ""
echo "=== Testing ping A -> C (10.0.0.3) ==="
ping -c 3 -W 2 -I tun_a 10.0.0.3 2>&1 || echo "PING FAILED"
echo ""
echo "=== Testing ping B -> C (10.0.0.3) ==="
ping -c 3 -W 2 -I tun_b 10.0.0.3 2>&1 || echo "PING FAILED"
echo ""
echo "=== Testing ping C -> A (10.0.0.1) ==="
ping -c 3 -W 2 -I tun_c 10.0.0.1 2>&1 || echo "PING FAILED"
echo ""
echo "=== Testing ping C -> B (10.0.0.2) ==="
ping -c 3 -W 2 -I tun_c 10.0.0.2 2>&1 || echo "PING FAILED"

# Check for errors in logs
echo ""
echo "=== Error/fail/timeout lines in logs ==="
for f in /tmp/client_a.log /tmp/client_b.log /tmp/client_c.log; do
    echo "--- $f ---"
    grep -i "error\|timeout\|fail\|drop\|reset\|No ARP\|unknown" "$f" | head -10
done

echo ""
echo "=== Tests completed ==="
