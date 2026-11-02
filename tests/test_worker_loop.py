import os
import pathlib
import signal
import subprocess
import sys
import time


def check(ok, msg):
    if not ok:
        raise RuntimeError(msg)


def main():
    demo = str(pathlib.Path(sys.argv[1]).resolve())
    fixture = pathlib.Path(sys.argv[2]).resolve()
    cwd = str(fixture.parent)
    # Console mein shell quoting nahi hai; space-free relative program path
    # do, test workspace ke absolute path mein spaces ho sakte hain.
    program = "./" + fixture.name
    base = 500000 + os.getpid() * 10
    commands = (f"run {base} 7 64 5 {program} stream\n"
                f"run {base+1} 8 64 5 {program} hello\n"
                "jobs\n")
    proc = subprocess.run([demo], input=commands, text=True,
                          capture_output=True, timeout=12, cwd=cwd)
    check(proc.returncode == 0, proc.stdout + proc.stderr)
    check("tick 0" in proc.stdout and "tick 3" in proc.stdout,
          "streaming output missing:\n" + proc.stdout + proc.stderr)
    check("hello stdout" in proc.stdout and "hello stderr" in proc.stdout,
          "both captured streams were not forwarded")
    check(f"Job {base} run 7 COMPLETED" in proc.stdout and
          f"Job {base+1} run 8 COMPLETED" in proc.stdout,
          "EOF did not wait for both jobs/results")
    for job in (base, base+1):
        check(not pathlib.Path(f"/tmp/osn_job_{job}").exists(), "job directory leaked")

    commands = ("x" * 2200 + "\nrun bad 1 64 5 printf\n"
                f"run {base} 1 64 5 printf path-ok\n")
    proc = subprocess.run([demo], input=commands, text=True,
                          capture_output=True, timeout=12, cwd=cwd)
    check(proc.returncode == 0 and "command too long" in proc.stdout and
          "invalid runner input" in proc.stdout and "path-ok" in proc.stdout,
          "invalid command broke later valid commands")

    commands = (f"run {base} 1 64 5 {program} sleep 5000\n"
                f"cancel {base} 99\ncancel {base} 1\n")
    proc = subprocess.run([demo], input=commands, text=True,
                          capture_output=True, timeout=12, cwd=cwd)
    check(proc.returncode == 0 and "execution not found" in proc.stdout and
          f"Job {base} run 1 CANCELLED" in proc.stdout,
          "execution token/cancellation handling failed")

    proc = subprocess.Popen([demo], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, cwd=cwd)
    try:
        proc.stdin.write(f"run {base} 1 64 5 {program} sleep 5000\n")
        proc.stdin.flush()
        time.sleep(0.5)
        proc.send_signal(signal.SIGINT)
        out, err = proc.communicate(timeout=8)
        check(proc.returncode == 0, out + err)
        check(not pathlib.Path(f"/tmp/osn_job_{base}").exists(), "interrupt cleanup failed")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()
    print("PASS worker loop console: concurrent jobs/EOF, invalid input, cancel and interrupt")


if __name__ == "__main__":
    main()
