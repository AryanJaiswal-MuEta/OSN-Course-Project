import socket
import struct
import subprocess
import sys
import time

packet = struct.Struct("!IBBHQ")
program = sys.argv[1]
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
    server.bind(("", 0))
    server.settimeout(3)
    port = server.getsockname()[1]
    with subprocess.Popen([program, str(port), "fallback"], stdout=subprocess.PIPE,
                          text=True) as client:
        try:
            data, peer = server.recvfrom(64)
            magic, version, kind, tcp_port, nonce = packet.unpack(data)
            assert (magic, version, kind, tcp_port) == (0x4F53444E, 1, 1, 0)
            server.sendto(packet.pack(magic, version, 2, 9998, nonce ^ 1), peer)
            server.sendto(packet.pack(magic, version, 2, 0, nonce), peer)
            server.sendto(packet.pack(magic, version, 2, 8080, nonce), peer)
            parts = client.communicate(timeout=4)[0].split()
            assert client.returncode == 0 and parts[:3] == ["0", "8080", "0"]
            socket.inet_pton(socket.AF_INET, parts[3])
        finally:
            if client.poll() is None:
                client.kill()

    # Keep UDP port bound but silent so fallback only happens after all retries.
    start = time.monotonic()
    proc = subprocess.run([program, str(port), "fallback"], check=True,
                          capture_output=True, text=True, timeout=9)
    assert proc.stdout.strip() == "0 9876 1 127.0.0.1"
    assert time.monotonic() - start >= 5.8
    nonces = []
    for _ in range(3):
        data, _ = server.recvfrom(64)
        nonces.append(packet.unpack(data)[4])
    assert len(set(nonces)) == 3, nonces
print("PASS discovery response validation and timed fallback")
