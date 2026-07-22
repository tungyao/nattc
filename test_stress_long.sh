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

./build/p2p_server $SERVER_PORT > /tmp/ls_server.log 2>&1 &
SERVER_PID=$!; sleep 0.5
./build/p2p_client a 10.0.0.1 $SERVER_IP $SERVER_PORT > /tmp/ls_a.log 2>&1 &
CLIENT1_PID=$!; sleep 2
./build/p2p_client b 10.0.0.2 $SERVER_IP $SERVER_PORT > /tmp/ls_b.log 2>&1 &
CLIENT2_PID=$!; sleep 4
./build/p2p_client c 10.0.0.3 $SERVER_IP $SERVER_PORT > /tmp/ls_c.log 2>&1 &
CLIENT3_PID=$!; sleep 6

echo "=== Basic connectivity ==="
ping -c 3 -W 1 -I tun_c 10.0.0.1 > /dev/null 2>&1 && echo "C->A OK" || echo "C->A FAIL"
ping -c 3 -W 1 -I tun_c 10.0.0.2 > /dev/null 2>&1 && echo "C->B OK" || echo "C->B FAIL"

echo ""
echo "=== Long stress: iperf + continuous ping check (60s) ==="

# Start iperf server on C
iperf3 -s -B 10.0.0.3 -p 5201 -1 > /tmp/ls_iperf.log 2>&1 &
IPERF_SRV=$!
sleep 0.5

# Start iperf from A to C in background
iperf3 -c 10.0.0.3 -p 5201 -B 10.0.0.1 -t 30 > /tmp/ls_iperf_ac.log 2>&1 &
IPERF_CLIENT=$!

# While iperf runs, continuously check connectivity
CHECK_FAIL=0
for i in $(seq 1 60); do
    sleep 1
    if ! ping -c 1 -W 1 -I tun_c 10.0.0.1 > /dev/null 2>&1; then
        CHECK_FAIL=$((CHECK_FAIL + 1))
        echo "t=${i}s: C->A FAIL (fail #$CHECK_FAIL)"
        if [ $CHECK_FAIL -ge 5 ]; then
            echo "*** C->A unreachable for 5 checks - connection broken! ***"
            break
        fi
    else
        if [ $CHECK_FAIL -gt 0 ]; then
            echo "t=${i}s: C->A recovered"
        fi
        CHECK_FAIL=0
    fi
done

wait $IPERF_CLIENT 2>/dev/null || true
wait $IPERF_SRV 2>/dev/null || true

echo ""
echo "=== Post-stress check ==="
ping -c 3 -W 2 -I tun_a 10.0.0.3 > /dev/null 2>&1 && echo "A->C OK" || echo "A->C FAIL"
ping -c 3 -W 2 -I tun_b 10.0.0.3 > /dev/null 2>&1 && echo "B->C OK" || echo "B->C FAIL"
ping -c 3 -W 2 -I tun_c 10.0.0.1 > /dev/null 2>&1 && echo "C->A OK" || echo "C->A FAIL"
ping -c 3 -W 2 -I tun_c 10.0.0.2 > /dev/null 2>&1 && echo "C->B OK" || echo "C->B FAIL"

echo ""
echo "=== iperf result ==="
grep -E "(sender|receiver|bits/sec)" /tmp/ls_iperf_ac.log 2>/dev/null | tail -2

echo ""
echo "=== Errors ==="
for f in /tmp/ls_a.log /tmp/ls_b.log /tmp/ls_c.log; do
    echo "--- $f ---"
    grep -i "error\|timeout\|fail\|reset\|No ARP\|unknown.*session\|timed out" "$f" | grep -v "Unknown peer" | head -10
done
grep -i "timed out\|initiate\|reset" /tmp/ls_a.log /tmp/ls_b.log /tmp/ls_c.log 2>/dev/null | head -10
