#!/bin/bash
SERVER_IP=127.0.0.1
SERVER_PORT=8800

cleanup() {
    kill $SERVER_PID $CLIENT1_PID $CLIENT2_PID 2>/dev/null || true
    wait 2>/dev/null || true
    ip link delete tun_a 2>/dev/null || true
    ip link delete tun_b 2>/dev/null || true
}
trap cleanup EXIT INT TERM

ip link delete tun_a 2>/dev/null || true
ip link delete tun_b 2>/dev/null || true

./build/p2p_server $SERVER_PORT > /tmp/qs.log 2>&1 &
SERVER_PID=$!
sleep 0.5

./build/p2p_client a 10.0.0.1 $SERVER_IP $SERVER_PORT > /tmp/qa.log 2>&1 &
CLIENT1_PID=$!
sleep 2

echo "=== Interface A ==="
ip addr show tun_a 2>/dev/null

./build/p2p_client b 10.0.0.2 $SERVER_IP $SERVER_PORT > /tmp/qb.log 2>&1 &
CLIENT2_PID=$!
sleep 4

echo "=== Routes ==="
ip route show | grep tun_
echo "=== Interfaces ==="
ip addr show tun_a 2>/dev/null | head -3
ip addr show tun_b 2>/dev/null | head -3

echo "=== Log A (tail) ==="
tail -30 /tmp/qa.log
echo "=== Log B (tail) ==="
tail -30 /tmp/qb.log

echo "=== PING A->B ==="
ping -c 3 -W 2 -I tun_a 10.0.0.2 2>&1

sleep 2
echo "=== DONE ==="
