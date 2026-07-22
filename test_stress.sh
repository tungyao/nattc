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

cmake --build build/ 2>&1 | tail -3

echo "=== Starting server ==="
./build/p2p_server $SERVER_PORT > /tmp/stress_server.log 2>&1 &
SERVER_PID=$!
sleep 0.5

echo "=== Starting client A (10.0.0.1) ==="
./build/p2p_client a 10.0.0.1 $SERVER_IP $SERVER_PORT > /tmp/stress_a.log 2>&1 &
CLIENT1_PID=$!
sleep 2

echo "=== Starting client B (10.0.0.2) ==="
./build/p2p_client b 10.0.0.2 $SERVER_IP $SERVER_PORT > /tmp/stress_b.log 2>&1 &
CLIENT2_PID=$!
sleep 4

echo "=== Starting client C (10.0.0.3) ==="
./build/p2p_client c 10.0.0.3 $SERVER_IP $SERVER_PORT > /tmp/stress_c.log 2>&1 &
CLIENT3_PID=$!
sleep 6

echo ""
echo "=== Phase 1: Basic connectivity check ==="
echo "--- A -> B ---"
ping -c 3 -W 1 -I tun_a 10.0.0.2 2>&1 || echo "FAILED"
echo "--- A -> C ---"
ping -c 3 -W 1 -I tun_a 10.0.0.3 2>&1 || echo "FAILED"
echo "--- B -> C ---"
ping -c 3 -W 1 -I tun_b 10.0.0.3 2>&1 || echo "FAILED"

echo ""
echo "=== Phase 2: Stress test (ping flood A->C for 30s) ==="
PING_COUNT=0
FAIL_COUNT=0
for i in $(seq 1 30); do
    RESULT=$(ping -c 10 -W 1 -i 0.1 -I tun_a 10.0.0.3 2>&1)
    LOSS=$(echo "$RESULT" | grep -oP '\d+(?=% packet loss)')
    LOSS=${LOSS:-100}
    if [ "$LOSS" = "100" ]; then
        echo "Iteration $i: ALL LOST"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    else
        TRANS=$(echo "$RESULT" | grep -oP '\d+(?= packets transmitted)')
        RECV=$(echo "$RESULT" | grep -oP '\d+(?= received)')
        echo "Iteration $i: $TRANS sent, $RECV received, $LOSS% loss"
    fi
    PING_COUNT=$((PING_COUNT + 10))
    # Check if we need to stop early
    if [ $FAIL_COUNT -ge 3 ]; then
        echo "*** 3 consecutive failures, marking as FAILED ***"
        break
    fi
done

echo ""
echo "=== Phase 3: Check if still connected after stress ==="
echo "--- A -> C ---"
ping -c 3 -W 2 -I tun_a 10.0.0.3 2>&1 || echo "FAILED"
echo "--- B -> C ---"
ping -c 3 -W 2 -I tun_b 10.0.0.3 2>&1 || echo "FAILED"
echo "--- C -> A ---"
ping -c 3 -W 2 -I tun_c 10.0.0.1 2>&1 || echo "FAILED"
echo "--- C -> B ---"
ping -c 3 -W 2 -I tun_c 10.0.0.2 2>&1 || echo "FAILED"

echo ""
echo "=== Summary ==="
echo "Pings sent: $PING_COUNT"
echo "Failures: $FAIL_COUNT"
echo ""
echo "=== Error/fail/timeout lines in logs ==="
for f in /tmp/stress_a.log /tmp/stress_b.log /tmp/stress_c.log; do
    echo "--- $f ---"
    grep -i "error\|timeout\|fail\|reset\|No ARP\|unknown" "$f" | grep -v "Unknown peer" | head -20
done
