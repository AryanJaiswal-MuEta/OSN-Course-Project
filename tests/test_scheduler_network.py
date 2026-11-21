"""Scheduler executable tests with explicit protocol peers (not a real worker)."""
import contextlib
import socket
import struct
import subprocess
import sys
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/scheduler"


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def exact(s, n):
    data = b""
    while len(data) < n:
        part = s.recv(n-len(data))
        assert part, "unexpected disconnect"
        data += part
    return data


def recv(s):
    magic, version, kind, n, request = struct.unpack("!IHHII", exact(s, 16))
    assert (magic, version) == (0x47505436, 1)
    return kind, request, exact(s, n)


def send(s, kind, data=b"", request=1, split=False):
    data = struct.pack("!IHHII", 0x47505436, 1, kind, len(data), request)+data
    if split:
        for i in range(0, len(data), 3):
            s.sendall(data[i:i+3])
    else:
        s.sendall(data)


def response(s, err=0):
    kind, request, data = recv(s)
    assert kind == 20, (kind, data)
    code, jid, n = struct.unpack("!IIH", data[:10])
    assert code == err, (code, data[10:])
    assert n == len(data)-10
    return jid, data[10:].decode()


@contextlib.contextmanager
def server(policy="PRIORITY"):
    port, udp = free_port(), free_port(socket.SOCK_DGRAM)
    proc = subprocess.Popen([BIN, str(port), policy, "-discovery-port", str(udp)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    peers = []

    def connect():
        s = socket.create_connection(("127.0.0.1", port), timeout=3)
        s.settimeout(4)
        peers.append(s)
        return s

    try:
        deadline = time.monotonic()+5
        while True:
            assert proc.poll() is None, proc.stderr.read().decode()
            try:
                s = connect()
                s.close()
                break
            except OSError:
                assert time.monotonic() < deadline, "scheduler did not start"
                time.sleep(.02)
        yield connect, udp
    finally:
        for s in peers:
            s.close()
        proc.terminate()
        try:
            proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        err = proc.stderr.read().decode()
        proc.stderr.close()
        assert proc.returncode == 0, (proc.returncode, err)


def login(connect, user=b"alice"):
    s = connect()
    send(s, 15, user, split=True)
    response(s)
    return s


def worker(connect, cores=4):
    s = connect()
    send(s, 1, struct.pack("!IQIH", cores, 4096, 4, 0), split=True)
    kind, _, data = recv(s)
    assert kind == 8
    return s, struct.unpack("!I", data)[0]


def submit(s, cores=1, prio=2, deps=None):
    data = struct.pack("!H", 5)+b"alice"+struct.pack("!H", 4)+b"true"
    data += struct.pack("!IQIIBIB", cores, 64, 1, 10, prio, 0, 1)
    if deps is not None:
        data += struct.pack("!H", len(deps))+b"".join(struct.pack("!BI", 0, x) for x in deps)
    send(s, 2, data)
    return response(s)[0]


def assigned(s, jid):
    kind, _, data = recv(s)
    assert kind == 3, (kind, data)
    actual, run = struct.unpack("!II", data[:8])
    assert actual == jid
    assert struct.unpack("!I", data[8:12])[0] > 0
    return run


def started(s, jid, run):
    send(s, 4, struct.pack("!II", jid, run))
    response(s)


def finished(s, jid, run, kind=5):
    send(s, kind, struct.pack("!IIQQQH", jid, run, 12, 7, 130, 0))
    response(s)


def status(s, jid):
    send(s, 17, struct.pack("!I", jid))
    return response(s)[1]


def pipeline():
    with server() as (connect, udp):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as d:
            d.settimeout(2)
            d.sendto(struct.pack("!IBBHQ", 0x4f53444e, 1, 1, 0, 0xffaabbcc), ("127.0.0.1", udp))
            packet, _ = d.recvfrom(64)
            magic, version, kind, port, nonce = struct.unpack("!IBBHQ", packet)
            assert (magic, version, kind, nonce) == (0x4f53444e, 1, 2, 0xffaabbcc)
            assert port > 0
        c = login(connect)
        w, wid = worker(connect)
        other, _ = worker(connect)
        jid = submit(c)
        run = assigned(w, jid)
        assert "QUEUED" in status(c, jid) and "pending=1" in status(c, jid)
        send(other, 4, struct.pack("!II", jid, run))
        response(other, 7)  # Other worker cannot acknowledge this session's job.
        started(w, jid, run)
        assert "RUNNING" in status(c, jid)
        send(w, 9, struct.pack("!II", 1250, 1)+struct.pack("!IIQQQQ", jid, run, 9, 12, 6, 100))
        # A heartbeat and a status query use different sockets; wait for observed telemetry.
        deadline = time.monotonic()+2
        while "ram=9" not in status(c, jid):
            assert time.monotonic() < deadline
            time.sleep(.01)
        finished(w, jid, run)
        assert "COMPLETED" in status(c, jid)
        send(w, 5, struct.pack("!IIQQQH", jid, run, 12, 7, 130, 0))
        response(w, 7)  # Late duplicate is fenced.
        send(c, 16)
        assert f"{wid} ALIVE cores=4/4" in response(c)[1]
        # Malformed payload rejected without killing the next valid request.
        send(c, 2, b"\x00")
        response(c, 1)
        assert "COMPLETED" in status(c, jid)
    print("PASS TCP discovery, registration, fragmented submission, start/result, telemetry, stale peer")


def migration():
    with server() as (connect, _):
        c = login(connect)
        w, _ = worker(connect)
        w2, _ = worker(connect)
        jid = submit(c)
        run = assigned(w, jid)
        started(w, jid, run)
        state = b"counter=128\x00\xff"
        send(w, 13, struct.pack("!IIQI", jid, run, 1, len(state))+state)
        response(w)
        send(c, 19, struct.pack("!I", jid))
        response(c)
        kind, _, data = recv(w)
        assert kind == 10 and data == struct.pack("!IIB", jid, run, 3)
        assert "RUNNING" in status(c, jid)
        finished(w, jid, run, 11)
        kind, _, data = recv(w2)
        assert kind == 21 and data[20:] == state
        run2 = assigned(w2, jid)
        assert run2 != run
        started(w2, jid, run2)
        assert "failures=0" in status(c, jid)
        send(c, 18, struct.pack("!I", jid))
        response(c)
        kind, _, data = recv(w2)
        assert kind == 10 and data[-1] == 1
        finished(w2, jid, run2, 11)
        assert "CANCELLED" in status(c, jid)
    print("PASS TCP migration stop barrier, restore-before-assignment, fresh execution token and cancel")


def recovery():
    with server() as (connect, _):
        c = login(connect)
        w, _ = worker(connect)
        jid = submit(c)
        run = assigned(w, jid)
        started(w, jid, run)
        child = submit(c, deps=[jid])
        assert "WAITING" in status(c, child)
        # Deliberately send no heartbeat while TCP stays connected.
        deadline = time.monotonic()+9
        while "failures=1" not in status(c, jid):
            assert time.monotonic() < deadline, "heartbeat expiry did not rescue job"
            time.sleep(.1)
        assert "WAITING" in status(c, child)
        for attempt in (2, 3):
            w, _ = worker(connect)
            run = assigned(w, jid)
            started(w, jid, run)
            w.shutdown(socket.SHUT_RDWR)
            w.close()
            deadline = time.monotonic()+3
            while f"failures={attempt}" not in status(c, jid):
                assert time.monotonic() < deadline
                time.sleep(.01)
        assert "FAILED_PERMANENT" in status(c, jid)
        assert "CANCELLED" in status(c, child)
    print("PASS TCP heartbeat death, three-attempt recovery and dependency failure cascade")


def preemption_and_drain():
    with server() as (connect, _):
        c = login(connect)
        w, _ = worker(connect)
        victim = submit(c, cores=4, prio=3)
        run = assigned(w, victim)
        started(w, victim, run)
        urgent = submit(c, cores=4, prio=1)
        kind, _, data = recv(w)
        assert kind == 10 and data == struct.pack("!IIB", victim, run, 2)
        finished(w, victim, run, 11)
        run2 = assigned(w, urgent)
        started(w, urgent, run2)
        assert "QUEUED" in status(c, victim) and "failures=0" in status(c, victim)
        finished(w, urgent, run2)
        run3 = assigned(w, victim)
        assert run3 != run
        started(w, victim, run3)
        finished(w, victim, run3)
        send(w, 12)
        response(w)
        jid = submit(c)
        assert "QUEUED" in status(c, jid)
        w2, _ = worker(connect)
        run = assigned(w2, jid)
        # Failure before any successful-exec acknowledgement stays a launch failure.
        finished(w2, jid, run, 6)
        assert "FAILED" in status(c, jid) and "failures=0" in status(c, jid)
        jid = submit(c)
        run = assigned(w2, jid)
        send(c, 18, struct.pack("!I", jid))
        response(c)
        kind, _, data = recv(w2)
        assert kind == 10 and data == struct.pack("!IIB", jid, run, 1)
        finished(w2, jid, run, 11)
        assert "CANCELLED" in status(c, jid)
        bob = login(connect, b"bob")
        send(bob, 18, struct.pack("!I", victim))
        assert "permission denied" in response(bob, 1)[1]
    print("PASS TCP preemption/holds, drain exclusion, launch failure, reserved cancellation and owner binding")


def cli():
    for args in ([], ["abc", "FIFO"], ["123junk", "FIFO"], ["1234", "bad"],
                 ["1234", "FIFO", "-unknown", "5"], ["1234", "FIFO", "-discovery-port", "65536"]):
        p = subprocess.run([BIN]+args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=2)
        assert p.returncode != 0, args
    print("PASS scheduler CLI validation")


cli()
pipeline()
migration()
preemption_and_drain()
recovery()
