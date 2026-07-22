#!/bin/bash
SERVER_IP=127.0.0.1
SERVER_PORT=8800

cleanup() {
    kill $SERVER_PID $CLIENT1_PID $CLIENT2_PID $CLIENT3_PID 2>/dev/null || true
    wait 2>/dev/null || true
    ip link delete tun_a tun_b tun_c 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cmake --build build/ 2>&1 | tail -3

echo "=== Starting ==="
./build/p2p_server $SERVER_PORT > /tmp/is_server.log 2>&1 &
SERVER_PID=$!; sleep 0.5
./build/p2p_client a 10.0.0.1 $SERVER_IP $SERVER_PORT > /tmp/is_a.log 2>&1 &
CLIENT1_PID=$!; sleep 2
./build/p2p_client b 10.0.0.2 $SERVER_IP $SERVER_PORT > /tmp/is_b.log 2>&1 &
CLIENT2_PID=$!; sleep 4
./build/p2p_client c 10.0.0.3 $SERVER_IP $SERVER_PORT > /tmp/is_c.log 2>&1 &
CLIENT3_PID=$!; sleep 6

echo "=== Phase 1: Basic connectivity ==="
ping -c 3 -W 1 -I tun_a 10.0.0.2 > /dev/null 2>&1 && echo "A->B OK" || echo "A->B FAIL"
ping -c 3 -W 1 -I tun_a 10.0.0.3 > /dev/null 2>&1 && echo "A->C OK" || echo "A->C FAIL"

echo ""
echo "=== Phase 2: iperf3 TCP test A->C (10s) ==="
# Start iperf3 server on C's TUN IP
iperf3 -s -B 10.0.0.3 -p 5201 -1 > /tmp/iperf_server.log 2>&1 &
IPERF_SRV=$!
sleep 0.5
# Start iperf3 client from A to C
iperf3 -c 10.0.0.3 -p 5201 -B 10.0.0.1 -t 5 > /tmp/iperf_result.log 2>&1
IPERF_RESULT=$?
wait $IPERF_SRV 2>/dev/null || true
if [ $IPERF_RESULT -eq 0 ]; then
    echo "iperf3 A->C SUCCESS"
    grep -E "(sender|receiver|bits/sec)" /tmp/iperf_result.log
else
    echo "iperf3 A->C FAILED (exit=$IPERF_RESULT)"
    tail -10 /tmp/iperf_result.log
fi

echo ""
echo "=== Phase 3: Check connectivity after iperf ==="
ping -c 3 -W 2 -I tun_a 10.0.0.3 > /dev/null 2>&1 && echo "A->C OK" || echo "A->C FAIL"
ping -c 3 -W 2 -I tun_b 10.0.0.3 > /dev/null 2>&1 && echo "B->C OK" || echo "B->C FAIL"
ping -c 3 -W 2 -I tun_c 10.0.0.1 > /dev/null 2>&1 && echo "C->A OK" || echo "C->A FAIL"
ping -c 3 -W 2 -I tun_c 10.0.0.2 > /dev/null 2>&1 && echo "C->B OK" || echo "C->B FAIL"

echo ""
echo "=== Phase 4: Multi-direction iperf (A->C + B->C + C->A simultaneously) ==="
# Start iperf3 servers
iperf3 -s -B 10.0.0.3 -p 5202 -1 > /tmp/iperf_srv_c.log 2>&1 &
IPERF_SRV1=$!
iperf3 -s -B 10.0.0.1 -p 5203 -1 > /tmp/iperf_srv_a.log 2>&1 &
IPERF_SRV2=$!
sleep 0.5
# Start clients in parallel
timeout 10 iperf3 -c 10.0.0.3 -p 5202 -B 10.0.0.1 -t 8 > /tmp/iperf_ac.log 2>&1 &
P1=$!
timeout 10 iperf3 -c 10.0.0.3 -p 5202 -B 10.0.0.2 -t 8 > /tmp/iperf_bc.log 2>&1 &
P2=$!
timeout 10 iperf3 -c 10.0.0.1 -p 5203 -B 10.0.0.3 -t 8 > /tmp/iperf_ca.log 2>&1 &
P3=$!
wait $P1 $P2 $P3 2>/dev/null || true
wait $IPERF_SRV1 $IPERF_SRV2 2>/dev/null || true
echo "Multi-direction iperf complete"

echo ""
echo "=== Phase 5: Final connectivity check ==="
ping -c 3 -W 2 -I tun_a 10.0.0.3 > /dev/null 2>&1 && echo "A->C OK" || echo "A->C FAIL"
ping -c 3 -W 2 -I tun_b 10.0.0.3 > /dev/null 2>&1 && echo "B->C OK" || echo "B->C FAIL"
ping -c 3 -W 2 -I tun_c 10.0.0.1 > /dev/null 2>&1 && echo "C->A OK" || echo "C->A FAIL"

echo ""
echo "=== Errors in logs ==="
for f in /tmp/is_a.log /tmp/is_b.log /tmp/is_c.log; do
    echo "--- $f ---"
    grep -i "error\|timeout\|fail\|reset\|No ARP\|unknown.*session\|timed out" "$f" | grep -v "Unknown peer" | head -15
done
echo ""
echo "=== Connection state (last line per peer) ==="
grep -h "State=" /tmp/is_a.log /tmp/is_b.log /tmp/is_c.log 2>/dev/null | tail -10
