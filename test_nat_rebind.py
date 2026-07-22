#!/usr/bin/env python3
"""Test NAT rebinding fix: client ID in heartbeat + address change detection."""
import socket
import struct
import time
import subprocess
import signal
import sys

MAGIC = 0xCAFE
MSG_LOGIN_V2 = 0x0D
MSG_LOGIN_RESP = 0x02
MSG_LOGIN_RESP_V2 = 0x0E
MSG_HEARTBEAT = 0x08
MSG_HEARTBEAT_RESP = 0x09
MSG_PUNCH_REQ = 0x03
MSG_PUNCH_NOTIFY = 0x04
MSG_PUNCH_NOTIFY_V2 = 0x0F

SERVER = '127.0.0.1'
PORT = 18810


def make_hdr(msg_type, body_len, seq=0):
    return struct.pack('!HHII', MAGIC, msg_type, seq, body_len)


def make_login_v2(client_id, vip, local_ip='192.168.1.100'):
    id_field = client_id.encode().ljust(32, b'\x00')
    vip_bytes = socket.inet_aton(vip)
    mac = b'\x02' + b'\x00' * 5
    # C struct login_req_v2 has 2 bytes padding between mac[6] and sockaddr_in
    padding = b'\x00' * 2
    local_addr = struct.pack('!HH4s8s', socket.AF_INET, 0,
                             socket.inet_aton(local_ip), b'\x00' * 8)
    body = id_field + vip_bytes + mac + padding + local_addr
    return make_hdr(MSG_LOGIN_V2, len(body)), body


def make_heartbeat(client_id):
    id_field = client_id.encode().ljust(32, b'\x00')
    return make_hdr(MSG_HEARTBEAT, len(id_field)), id_field


def make_punch_req(requester_id, target_id):
    rid = requester_id.encode().ljust(32, b'\x00')
    tid = target_id.encode().ljust(32, b'\x00')
    body = rid + tid
    return make_hdr(MSG_PUNCH_REQ, len(body)), body


def recv_msgs(sock, timeout=2):
    msgs = []
    sock.settimeout(timeout)
    while True:
        try:
            data, addr = sock.recvfrom(4096)
            if len(data) >= 12:
                magic, mtype, seq, blen = struct.unpack('!HHII', data[:12])
                if magic == MAGIC:
                    body = data[12:12 + blen]
                    msgs.append((mtype, body, addr))
        except socket.timeout:
            break
    return msgs


def parse_pub_addr(data, offset=0):
    """Parse sockaddr_in from data at given offset."""
    sin_family = struct.unpack('!H', data[offset:offset + 2])[0]
    sin_port = struct.unpack('!H', data[offset + 2:offset + 4])[0]
    sin_addr = socket.inet_ntoa(data[offset + 4:offset + 8])
    return sin_addr, sin_port


def parse_login_resp(body):
    """Parse login response body (V1). Returns (server_addr, peers)."""
    pub_ip, pub_port = parse_pub_addr(body, 0)
    count = struct.unpack('!I', body[16:20])[0]
    peers = []
    offset = 20
    for i in range(count):
        if offset + 60 > len(body):
            break
        pid = body[offset:offset + 32].rstrip(b'\x00').decode()
        pvip = socket.inet_ntoa(body[offset + 32:offset + 36])
        ppub_ip, ppub_port = parse_pub_addr(body, offset + 44)
        peers.append({'id': pid, 'vip': pvip, 'pub_ip': ppub_ip, 'pub_port': ppub_port})
        offset += 60
    return {'pub_ip': pub_ip, 'pub_port': pub_port, 'count': count}, peers


def parse_login_resp_v2(body):
    """Parse V2 login response body."""
    pub_ip, pub_port = parse_pub_addr(body, 0)
    count = struct.unpack('!I', body[16:20])[0]
    peers = []
    offset = 20
    for i in range(count):
        if offset + 76 > len(body):
            break
        pid = body[offset:offset + 32].rstrip(b'\x00').decode()
        pvip = socket.inet_ntoa(body[offset + 32:offset + 36])
        ppub_ip, ppub_port = parse_pub_addr(body, offset + 44)
        plocal_ip, plocal_port = parse_pub_addr(body, offset + 60)
        peers.append({
            'id': pid, 'vip': pvip,
            'pub_ip': ppub_ip, 'pub_port': ppub_port,
            'local_ip': plocal_ip, 'local_port': plocal_port
        })
        offset += 76
    return {'pub_ip': pub_ip, 'pub_port': pub_port, 'count': count}, peers


def main():
    passed = 0
    failed = 0

    # Start server
    print("=" * 60)
    print("Starting server on port %d..." % PORT)
    server_proc = subprocess.Popen(
        ['./build/p2p_server', str(PORT)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    time.sleep(0.5)

    try:
        # ---- TEST 1: Normal login ----
        print("\n--- Test 1: Client A logs in from port X ---")
        sock_a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock_a.bind(('127.0.0.1', 0))
        print("  Client A socket: port %d" % sock_a.getsockname()[1])

        hdr, body = make_login_v2('clientA', '10.0.0.1')
        sock_a.sendto(hdr + body, (SERVER, PORT))
        time.sleep(0.3)

        msgs = recv_msgs(sock_a, timeout=1)
        login_resp = [m for m in msgs if m[0] in (MSG_LOGIN_RESP, MSG_LOGIN_RESP_V2)]
        if login_resp:
            mtype, resp_body, _ = login_resp[0]
            if mtype == MSG_LOGIN_RESP_V2:
                info, peers = parse_login_resp_v2(resp_body)
            else:
                info, peers = parse_login_resp(resp_body)
            print("  Login OK: server sees us at %s:%d" % (info['pub_ip'], info['pub_port']))
            if info['count'] == 0:
                print("  PASS: No other clients (expected)")
                passed += 1
            else:
                print("  FAIL: Expected 0 peers, got %d" % info['count'])
                failed += 1
        else:
            print("  FAIL: No login response")
            failed += 1

        # ---- TEST 2: Heartbeat from original port ----
        print("\n--- Test 2: Client A heartbeat from original port ---")
        hdr, body = make_heartbeat('clientA')
        sock_a.sendto(hdr + body, (SERVER, PORT))
        time.sleep(0.3)

        msgs = recv_msgs(sock_a, timeout=1)
        hb_resp = [m for m in msgs if m[0] == MSG_HEARTBEAT_RESP]
        if hb_resp:
            print("  PASS: Heartbeat response received")
            passed += 1
        else:
            print("  FAIL: No heartbeat response")
            failed += 1

        # ---- TEST 3: NAT rebinding - heartbeat from NEW port ----
        print("\n--- Test 3: NAT rebinding - heartbeat from NEW port ---")
        sock_a_new = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock_a_new.bind(('127.0.0.1', 0))
        print("  Client A new socket: port %d (simulating NAT rebinding)" %
              sock_a_new.getsockname()[1])

        hdr, body = make_heartbeat('clientA')
        sock_a_new.sendto(hdr + body, (SERVER, PORT))
        time.sleep(0.3)

        msgs = recv_msgs(sock_a_new, timeout=1)
        hb_resp = [m for m in msgs if m[0] == MSG_HEARTBEAT_RESP]
        if hb_resp:
            print("  PASS: Heartbeat response received from new port")
            passed += 1
        else:
            print("  FAIL: No heartbeat response from new port")
            failed += 1

        # ---- TEST 4: Client B logs in and discovers client A ----
        print("\n--- Test 4: Client B logs in and discovers client A ---")
        sock_b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock_b.bind(('127.0.0.1', 0))
        print("  Client B socket: port %d" % sock_b.getsockname()[1])

        hdr, body = make_login_v2('clientB', '10.0.0.2')
        sock_b.sendto(hdr + body, (SERVER, PORT))
        time.sleep(0.3)

        msgs = recv_msgs(sock_b, timeout=1)
        login_resp = [m for m in msgs if m[0] in (MSG_LOGIN_RESP, MSG_LOGIN_RESP_V2)]
        if login_resp:
            mtype, resp_body, _ = login_resp[0]
            if mtype == MSG_LOGIN_RESP_V2:
                info, peers = parse_login_resp_v2(resp_body)
            else:
                info, peers = parse_login_resp(resp_body)
            print("  Login OK: server sees us at %s:%d (msg_type=0x%02x)" %
                  (info['pub_ip'], info['pub_port'], mtype))
            found_a = [p for p in peers if p['id'] == 'clientA']
            if found_a:
                print("  PASS: Client B discovered client A (pub=%s:%d)" %
                      (found_a[0]['pub_ip'], found_a[0]['pub_port']))
                passed += 1
            else:
                print("  FAIL: Client B did NOT discover client A. Peers: %s" %
                      [p['id'] for p in peers])
                failed += 1
        else:
            print("  FAIL: No login response for client B")
            failed += 1

        # ---- TEST 5: Heartbeat keeps client A alive ----
        print("\n--- Test 5: Heartbeat from client A new port keeps it alive ---")
        hdr, body = make_heartbeat('clientA')
        sock_a_new.sendto(hdr + body, (SERVER, PORT))
        time.sleep(0.3)
        msgs = recv_msgs(sock_a_new, timeout=1)
        hb_resp = [m for m in msgs if m[0] == MSG_HEARTBEAT_RESP]
        if hb_resp:
            print("  PASS: Client A still alive after NAT rebind")
            passed += 1
        else:
            print("  FAIL: Client A heartbeat failed")
            failed += 1

        # ---- TEST 6: Punch request from client B to client A ----
        print("\n--- Test 6: Client B sends punch request to client A ---")
        hdr, body = make_punch_req('clientB', 'clientA')
        sock_b.sendto(hdr + body, (SERVER, PORT))
        time.sleep(0.3)

        msgs = recv_msgs(sock_b, timeout=1)
        punch_notify = [m for m in msgs if m[0] in (MSG_PUNCH_NOTIFY, MSG_PUNCH_NOTIFY_V2)]
        if punch_notify:
            print("  PASS: Client B received punch notify for client A")
            passed += 1
        else:
            print("  FAIL: Client B did not receive punch notify")
            failed += 1

    finally:
        server_proc.send_signal(signal.SIGTERM)
        try:
            server_proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server_proc.kill()
        sock_a.close()
        sock_a_new.close()
        sock_b.close()

    print("\n" + "=" * 60)
    print("Results: %d passed, %d failed" % (passed, failed))
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
