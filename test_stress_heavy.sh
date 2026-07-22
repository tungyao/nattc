#!/bin/bash
# No set -e - we handle errors manually
SERVER_IP=127.0.0.1
SERVER_PORT=8800

cleanup() {
    kill $SERVER_PID $CLIENT1_PID $CLIENT2_PID $CLIENT3_PID $PING_PID 2>/dev/null || true
    wait 2>/dev/null || true
    ip link delete tun_a tun_b tun_c 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cmake --build build/ 2>&1 | tail -3

./build/p2p_server $SERVER_PORT > /tmp/hs_server.log 2>&1 &
SERVER_PID=$!; sleep 0.5
./build/p2p_client a 10.0.0.1 $SERVER_IP $SERVER_PORT > /tmp/hs_a.log 2>&1 &
CLIENT1_PID=$!; sleep 2
./build/p2p_client b 10.0.0.2 $SERVER_IP $SERVER_PORT > /tmp/hs_b.log 2>&1 &
CLIENT2_PID=$!; sleep 4
./build/p2p_client c 10.0.0.3 $SERVER_IP $SERVER_PORT > /tmp/hs_c.log 2>&1 &
CLIENT3_PID=$!; sleep 6

echo "=== Phase 1: Basic connectivity ==="
ping -c 3 -W 1 -I tun_a 10.0.0.2 > /dev/null 2>&1 && echo "A->B OK" || echo "A->B FAIL"
ping -c 3 -W 1 -I tun_a 10.0.0.3 > /dev/null 2>&1 && echo "A->C OK" || echo "A->C FAIL"

echo ""
echo "=== Phase 2: Continuous ping flood A->C for 30s ==="
PING_FAIL=0
for i in $(seq 1 30); do
    if ! ping -c 5 -W 1 -i 0.05 -I tun_a 10.0.0.3 > /dev/null 2>&1; then
        PING_FAIL=$((PING_FAIL + 1))
        echo "Iteration $i: FAIL"
        if [ $PING_FAIL -ge 5 ]; then
            echo "*** 5 consecutive failures - connection likely dropped ***"
            break
        fi
    else
        PING_FAIL=0
    fi
done
echo "Stress test complete"

echo ""
echo "=== Phase 3: Check connectivity after stress ==="
ping -c 3 -W 2 -I tun_a 10.0.0.3 > /dev/null 2>&1 && echo "A->C OK" || echo "A->C FAIL"
ping -c 3 -W 2 -I tun_b 10.0.0.3 > /dev/null 2>&1 && echo "B->C OK" || echo "B->C FAIL"
ping -c 3 -W 2 -I tun_c 10.0.0.1 > /dev/null 2>&1 && echo "C->A OK" || echo "C->A FAIL"
ping -c 3 -W 2 -I tun_c 10.0.0.2 > /dev/null 2>&1 && echo "C->B OK" || echo "C->B FAIL"

echo ""
echo "=== Errors in logs ==="
for f in /tmp/hs_a.log /tmp/hs_b.log /tmp/hs_c.log; do
    echo "--- $f ---"
    grep -i "error\|timeout\|fail\|reset\|No ARP\|unknown.*session\|timed out" "$f" | head -15
done
