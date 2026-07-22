#!/usr/bin/env python3
"""Quick test: send a punch request to trigger peer discovery."""
import socket
import struct
import time

MAGIC = 0xCAFE
MSG_PUNCH_REQ = 0x03

def make_punch_req(target_id):
    hdr = struct.pack('!HHII', MAGIC, MSG_PUNCH_REQ, 0, len(target_id))
    return hdr + target_id.encode()

def main():
    # Send punch request from client1's perspective to trigger server notification
    # Actually, we need to send from client1's UDP port. Let's just connect to server
    # and send a punch request for client2.

    # But clients already have their own sockets. Let's use a simpler approach:
    # just send a raw UDP punch request to the server pretending to be client1.

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    print(f"Test socket bound to {s.getsockname()}")

    # First, login as client1
    login_body = struct.pack('!32s I 6s', b'client1', 0x0100000A, b'\x00'*6)
    login_hdr = struct.pack('!HHII', MAGIC, 0x01, 0, len(login_body))
    s.sendto(login_hdr + login_body, ('127.0.0.1', 8800))
    time.sleep(0.3)

    # Now send punch request for client2
    target = 'client2'.encode().ljust(32, b'\x00')
    punch_body = target
    punch_hdr = struct.pack('!HHII', MAGIC, MSG_PUNCH_REQ, 0, len(punch_body))
    s.sendto(punch_hdr + punch_body, ('127.0.0.1', 8800))
    print("Sent punch request for client2")

    # Listen for responses for a few seconds
    s.settimeout(5)
    while True:
        try:
            data, addr = s.recvfrom(4096)
            if len(data) >= 12:
                magic, mtype, seq, blen = struct.unpack('!HHII', data[:12])
                print(f"  Received type=0x{mtype:02x} from {addr} body_len={blen}")
        except socket.timeout:
            break

    s.close()

if __name__ == '__main__':
    main()
