"""Real worker processes against protocol fixtures and the real scheduler."""
import contextlib
import os
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

WORKER, SCHEDULER, COUNTER = map(lambda s: str(pathlib.Path(s).resolve()), sys.argv[1:4])
STREAM = str(pathlib.Path(COUNTER).with_name('stream_fixture'))
BASE = 9000000+os.getpid()*10


def exact(s, n):
    data = b""
    while len(data) < n:
        part = s.recv(n-len(data))
        assert part, "worker disconnected"
        data += part
    return data


def frame(s):
    magic, version, kind, size, req = struct.unpack("!IHHII", exact(s, 16))
    assert (magic, version) == (0x47505436, 1)
    return kind, req, exact(s, size)


def send(s, kind, data=b"", req=0):
    s.sendall(struct.pack("!IHHII", 0x47505436, 1, kind, len(data), req)+data)


class Peer:
    def __init__(self, s):
        self.s = s
        self.events = []
        self.outputs = {}
        self.checkpoints = {}
        self.results = {}
        self.started = set()
        self.inventory = set()
        self.misses = set()
        self.heartbeats = 0
        self.draining = False

    def read(self):
        kind, _, data = frame(self.s)
        self.events.append((kind, data))
        if kind == 9:
            load, count = struct.unpack("!II", data[:8])
            assert load <= 10000 and len(data) == 8+40*count
            self.heartbeats += 1
        elif kind == 14:
            key = data[1:].decode()
            if data[0]:
                self.inventory.add(key)
            else:
                self.inventory.discard(key)
        elif kind == 4:
            self.started.add(struct.unpack("!II", data))
        elif kind == 27:
            jid, run, off = struct.unpack("!IIQ", data[:16])
            out = self.outputs.setdefault((jid, run), bytearray())
            assert len(out) == off, (len(out), off)
            assert (jid, run) in self.started, "output preceded exec confirmation"
            out.extend(data[16:])
        elif kind == 13:
            jid, run, seq, size = struct.unpack("!IIQI", data[:20])
            assert len(data) == 20+size and (jid, run) in self.started
            previous = self.checkpoints.get((jid, run), (0, b""))[0]
            assert seq > previous
            self.checkpoints[jid, run] = seq, data[20:]
        elif kind in (5, 6, 11):
            jid, run, peak, faults, cpu, size = struct.unpack("!IIQQQH", data[:34])
            assert len(data) == 34+size
            self.results[jid, run] = kind, data[34:].decode()
        elif kind == 26:
            self.misses.add(struct.unpack("!II", data[:8]))
        elif kind == 12:
            self.draining = True
        else:
            raise AssertionError((kind, data))

    def until(self, test, timeout=8):
        end = time.monotonic()+timeout
        while not test():
            assert time.monotonic() < end, "worker event deadline exceeded"
            self.s.settimeout(max(.05, end-time.monotonic()))
            try:
                self.read()
            except TimeoutError as exc:
                raise AssertionError((self.results,self.started,self.checkpoints,
                                      {k: bytes(v[:300]) for k,v in self.outputs.items()})) from exc

    def assign(self, jid, run, path="true", limit=10, cores=1):
        path = path.encode()
        send(self.s, 3, struct.pack("!IIIQIIH", jid, run, cores, 64, 0, limit, len(path))+
             path+struct.pack("!I", 0))

    def stop(self, jid, run, intent):
        send(self.s, 10, struct.pack("!IIB", jid, run, intent))

    def transfer(self, jid, run, key, data):
        raw = key.encode()
        send(self.s, 22, struct.pack("!IIQH", jid, run, len(data), len(raw))+raw)
        for off in range(0, len(data), 16384):
            send(self.s, 23, struct.pack("!IIQ", jid, run, off)+data[off:off+16384])
        send(self.s, 24, struct.pack("!II", jid, run))


@contextlib.contextmanager
def fixture(initial_cache=None):
    tcp = socket.socket()
    tcp.bind(("0.0.0.0", 0))
    tcp.listen()
    tcp.settimeout(10)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", 0))
    udp.settimeout(.2)
    done = threading.Event()

    def discover():
        while not done.is_set():
            try:
                data, addr = udp.recvfrom(64)
                if len(data) == 16:
                    magic, version, kind, _, nonce = struct.unpack("!IBBHQ", data)
                    if (magic, version, kind) == (0x4f53444e, 1, 1):
                        udp.sendto(struct.pack("!IBBHQ", magic, 1, 2, tcp.getsockname()[1], nonce), addr)
            except socket.timeout:
                pass

    thread = threading.Thread(target=discover, daemon=True)
    thread.start()
    peers = []
    with tempfile.TemporaryDirectory(prefix="osn_worker_test_") as cache:
        for key,data in (initial_cache or {}).items():
            path=pathlib.Path(cache)/key
            path.write_bytes(data)
            path.chmod(0o700)
        env = dict(os.environ, OSN_CACHE_DIR=cache, OSN_CACHE_SIZE_MB="2")
        proc = subprocess.Popen([WORKER, "-cores", "2", "-ram", "512", "-discovery-port", str(udp.getsockname()[1])],
                                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)

        def accept():
            s, _ = tcp.accept()
            peers.append(s)
            s.settimeout(8)
            kind, request, data = frame(s)
            assert kind == 1 and len(data) >= 18
            send(s, 8, struct.pack("!I", 1000+len(peers)), request)
            return Peer(s)

        try:
            yield proc, accept, pathlib.Path(cache)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            for s in peers:
                s.close()
            proc.stdin.close()
            stderr = proc.stderr.read().decode()
            proc.stderr.close()
            done.set()
            thread.join(timeout=1)
            tcp.close()
            udp.close()
            assert proc.returncode == 0, (proc.returncode, stderr)


def process_and_cache():
    with fixture() as (proc, accept, cache):
        p = accept()
        p.assign(BASE, 1)
        p.until(lambda: (BASE, 1) in p.results)
        assert p.results[BASE, 1][0] == 5 and (BASE, 1) in p.started
        assert not pathlib.Path(f"/tmp/osn_job_{BASE}").exists()
        p.assign(BASE+1, 2, "osn_command_does_not_exist_87353")
        p.until(lambda: (BASE+1, 2) in p.results)
        assert p.results[BASE+1, 2][0] == 6 and (BASE+1, 2) not in p.started
        assert "execvp" in p.results[BASE+1, 2][1]
        # Transferred binary path deliberately does not exist on the worker.
        binary = pathlib.Path(COUNTER).read_bytes()
        p.transfer(BASE+2, 3, "counter-v1", binary)
        p.assign(BASE+2, 3, "remote-counter")
        p.until(lambda: (BASE+2, 3) in p.checkpoints and int(p.checkpoints[BASE+2, 3][1]) >= 5)
        started = time.monotonic()
        p.stop(BASE+2, 3, 3)
        p.until(lambda: (BASE+2, 3) in p.results)
        assert time.monotonic()-started < 3
        assert p.results[BASE+2, 3][0] == 11
        saved = p.checkpoints[BASE+2, 3][1]
        assert not pathlib.Path(f"/tmp/osn_job_{BASE+2}").exists()
        send(p.s, 21, struct.pack("!IIQI", BASE+2, 4, 1, len(saved))+saved)
        send(p.s, 25, struct.pack("!II", BASE+2, 4)+b"counter-v1")
        p.assign(BASE+2, 4, "remote-counter")
        p.until(lambda: f"resume={int(saved)}".encode() in p.outputs.get((BASE+2, 4), b""))
        p.stop(BASE+2, 3, 1)  # Old token must not cancel the resumed execution or disconnect it.
        p.stop(BASE+2, 4, 2)
        p.until(lambda: (BASE+2, 4) in p.results)
        assert p.results[BASE+2, 4][0] == 11
        # Removed cached file must request a transfer, not execute stale path.
        (cache/"counter-v1").unlink()
        send(p.s, 25, struct.pack("!II", BASE+3, 5)+b"counter-v1")
        p.until(lambda: (BASE+3, 5) in p.misses)
        assert (BASE+3, 5) not in p.started
        p.transfer(BASE+3, 5, "counter-v1", binary)
        p.assign(BASE+3, 5, "remote-counter")
        p.until(lambda: (BASE+3, 5) in p.started)
        p.stop(BASE+3, 5, 1)
        p.until(lambda: (BASE+3, 5) in p.results)
        # Wall clock timeout and final streamed output are real process behavior.
        script = b"#!/bin/sh\nprintf 'before sleep\\n'\nsleep 10\n"
        p.transfer(BASE+4, 6, "sleep-script", script)
        p.assign(BASE+4, 6, "remote-script", limit=1)
        p.until(lambda: (BASE+4, 6) in p.results)
        assert p.results[BASE+4, 6][0] == 6 and "timeout" in p.results[BASE+4, 6][1]
        assert b"before sleep" in p.outputs[BASE+4, 6]
        proc.stdin.write(b"drain\n")
        proc.stdin.flush()
        p.until(lambda: p.draining)
        proc.wait(timeout=4)
    print("PASS real worker exec/failure, transfer/cache/reference/miss, output, checkpoint restore, migration, preemption, timeout and drain")


def reconnect():
    with fixture() as (proc, accept, cache):
        p = accept()
        script = b"#!/bin/sh\nprintf 'pid=%d\\n' $$\nsleep 15\n"
        p.transfer(BASE+5, 7, "long-script", script)
        p.assign(BASE+5, 7, "remote-script", limit=20)
        p.until(lambda: b"pid=" in p.outputs.get((BASE+5, 7), b""))
        pid = int(p.outputs[BASE+5, 7].split(b"pid=")[1].splitlines()[0])
        p.s.shutdown(socket.SHUT_RDWR)
        p.s.close()
        p2 = accept()
        assert not pathlib.Path(f"/proc/{pid}").exists(), "old execution survived reconnect"
        assert not pathlib.Path(f"/tmp/osn_job_{BASE+5}").exists()
        p2.until(lambda: "long-script" in p2.inventory)
        proc.send_signal(signal.SIGINT)
        p2.until(lambda: p2.draining)
        proc.wait(timeout=4)
    print("PASS disconnect kill/reap before rediscovery, persistent inventory and SIGINT drain")


def real_cluster(workers=1):
    import test_scheduler_network as scheduler_test
    scheduler_test.BIN = SCHEDULER
    with scheduler_test.server() as (connect, udp):
        with tempfile.TemporaryDirectory(prefix="osn_cluster_cache_") as cache:
            procs = []
            for i in range(workers):
                directory = pathlib.Path(cache)/str(i)
                directory.mkdir(mode=0o700)
                env = dict(os.environ, OSN_CACHE_DIR=str(directory))
                procs.append(subprocess.Popen([WORKER, "-cores", "2", "-ram", "512", "-discovery-port", str(udp)],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env))
            try:
                c = scheduler_test.login(connect)
                end = time.monotonic()+6
                while True:
                    scheduler_test.send(c, 16)
                    if scheduler_test.response(c)[1].count("ALIVE") >= workers:
                        break
                    assert time.monotonic() < end
                    time.sleep(.05)
                path = COUNTER.encode()
                data = struct.pack("!H", 5)+b"alice"+struct.pack("!H", len(path))+path
                data += struct.pack("!IQIIBIB", 1, 64, 0, 15, 2, 0, 1)
                scheduler_test.send(c, 2, data)
                jid = scheduler_test.response(c)[0]
                end = time.monotonic()+8
                while "checkpoint=0" in scheduler_test.status(c, jid):
                    assert time.monotonic() < end
                    time.sleep(.1)
                original = scheduler_test.status(c, jid).split("worker=")[1].split()[0]
                scheduler_test.send(c, 19, struct.pack("!I", jid))
                scheduler_test.response(c)
                end = time.monotonic()+10
                moved = False
                while True:
                    status = scheduler_test.status(c, jid)
                    assert "FAILED" not in status, status
                    current = status.split("worker=")[1].split()[0]
                    if "RUNNING" in status and current != original:
                        moved = True
                    if "COMPLETED" in status:
                        assert "failures=0" in status
                        assert workers == 1 or moved, "migration did not move to other worker"
                        break
                    assert time.monotonic() < end, status
                    time.sleep(.1)
                for proc in procs:
                    proc.send_signal(signal.SIGINT)
                for proc in procs:
                    proc.wait(timeout=4)
                    assert proc.returncode == 0
            finally:
                for proc in procs:
                    if proc.poll() is None:
                        proc.terminate()
                        proc.wait(timeout=5)
                    stderr = proc.stderr.read().decode()
                    proc.stderr.close()
                    assert proc.returncode == 0, stderr
    print(f"PASS real scheduler + {workers} worker(s): discovery, admission, exec, heartbeat, checkpoint, migration, completion and accounting")


def independent_completions():
    with fixture() as (_,accept,cache):
        p=accept()
        producer,fast=BASE+10,BASE+11
        p.transfer(producer,20,'continuous-stream',pathlib.Path(STREAM).read_bytes())
        p.assign(producer,20,'remote-stream')
        p.until(lambda:len(p.outputs.get((producer,20),b''))>=32768)
        start=time.monotonic()
        p.assign(fast,21)
        p.until(lambda:(fast,21) in p.results,timeout=2)
        latency=time.monotonic()-start
        assert p.results[fast,21][0]==5
        assert (producer,20) not in p.results,'producer ended before concurrent result was checked'
        assert not pathlib.Path(f'/tmp/osn_job_{fast}').exists()
        p.stop(producer,20,1)
        p.until(lambda:(producer,20) in p.results,timeout=8)
        assert p.results[producer,20][0]==11
        # Failed setup/exec must not make an unusable cached program look recently used.
        failed=BASE+12
        p.transfer(failed,22,'bad-interpreter',b'#!/osn/no_such_interpreter_94851\n')
        p.until(lambda:'bad-interpreter' in p.inventory)
        before=(cache/'bad-interpreter').stat().st_mtime_ns
        p.assign(failed,22,'remote-invalid')
        p.until(lambda:(failed,22) in p.results)
        assert p.results[failed,22][0]==6 and (failed,22) not in p.started
        assert (cache/'bad-interpreter').stat().st_mtime_ns==before
    print(f'PASS completed job reports during unrelated output ({latency:.2f}s); exec failure leaves LRU unchanged')


def migration_grace_and_active_drain():
    with fixture() as (proc,accept,_):
        p=accept()
        jid=BASE+13
        program=b"#!/bin/sh\ntrap '' TERM\nprintf 'ready\\n'\nwhile :; do sleep 1; done\n"
        p.transfer(jid,23,'ignore-term',program)
        p.assign(jid,23,'remote-ignore-term')
        p.until(lambda:b'ready' in p.outputs.get((jid,23),b''))
        begin=time.monotonic()
        p.stop(jid,23,3)
        p.until(lambda:(jid,23) in p.results,timeout=4)
        elapsed=time.monotonic()-begin
        assert 1.8<=elapsed<3.5,elapsed
        assert p.results[jid,23][0]==11
        assert not pathlib.Path(f'/tmp/osn_job_{jid}').exists()
        jid=BASE+14
        p.transfer(jid,24,'drain-counter',pathlib.Path(COUNTER).read_bytes())
        p.assign(jid,24,'remote-counter')
        p.until(lambda:(jid,24) in p.started)
        heartbeat_count=p.heartbeats
        proc.stdin.write(b'drain\n'); proc.stdin.flush()
        p.until(lambda:p.draining)
        assert proc.poll() is None and (jid,24) not in p.results
        p.until(lambda:(jid,24) in p.results,timeout=8)
        assert p.results[jid,24][0]==5
        assert p.heartbeats>heartbeat_count
        proc.wait(timeout=4)
    print('PASS migration enforces two-second grace; active drain keeps heartbeats and waits for completion')


def control_precedes_inventory():
    with fixture({'inventory-A':b'a','inventory-B':b'b'}) as (proc,accept,_):
        p=accept()
        p.read()
        assert p.events[0][0]==9,'bulk inventory was queued ahead of due heartbeat'
        p.until(lambda:len(p.inventory)==2)
        proc.send_signal(signal.SIGINT)
        p.until(lambda:p.draining)
        proc.wait(timeout=4)
    print('PASS startup inventory is reported after due heartbeat control traffic')


if __name__=='__main__':
    process_and_cache()
    reconnect()
    real_cluster()
    real_cluster(2)
    independent_completions()
    migration_grace_and_active_drain()
    control_precedes_inventory()
