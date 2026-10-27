import os
import pathlib
import pwd
import re
import signal
import subprocess
import sys
import tempfile
import time


def check(ok, msg):
    if not ok:
        raise RuntimeError(msg)


def main():
    runner = str(pathlib.Path(sys.argv[1]).resolve())
    fixture = str(pathlib.Path(sys.argv[2]).resolve())
    count = 0

    def run(args, opts=(), expected=0, **kwargs):
        nonlocal count
        proc = subprocess.run([runner, *opts, "--", *args], text=True,
                              capture_output=True, timeout=12, **kwargs)
        check(proc.returncode == expected,
              f"unexpected exit {proc.returncode}: {proc.stdout}\n{proc.stderr}")
        match = re.search(r"\[RUNNER\] Job (\d+) run", proc.stderr)
        if match:
            check(not pathlib.Path(f"/tmp/osn_job_{match[1]}").exists(),
                  "job directory was not cleaned")
        count += 1
        return proc

    proc = run([fixture, "hello"])
    check("hello stdout" in proc.stdout and "hello stderr" in proc.stdout,
          "both child streams must be captured")
    check("hello stdout" not in proc.stderr and "hello stderr" not in proc.stderr,
          "child output leaked into worker diagnostics")
    check("cwd=/tmp/osn_job_" in proc.stdout, "job did not enter its own cwd")
    uid = pwd.getpwnam("nobody").pw_uid if os.geteuid() == 0 else os.geteuid()
    check(f"uid={uid} " in proc.stdout, "unexpected job UID")
    if os.geteuid():
        check("without privilege-dropping" in proc.stderr, "non-root warning missing")

    proc = run([fixture, "limits"], ["-ram", "48", "-time", "3"])
    check("address_limit=50331648" in proc.stdout and "cpu_limit=3" in proc.stdout,
          "requested rlimits were not applied")
    check("process_limit=64" in proc.stdout, "fork-bomb guard missing")

    start = time.monotonic()
    proc = run([fixture, "sleep", "5000"], ["-time", "1"], expected=1)
    check("Wall-clock timeout exceeded" in proc.stderr, "sleep deadline was not enforced")
    check(time.monotonic() - start < 4, "sleep timeout took too long")
    proc = run([fixture, "cpu"], ["-time", "1"], expected=1)
    check("timeout exceeded" in proc.stderr, "CPU timeout diagnosis missing")
    proc = run([fixture, "exit"], expected=1)
    check("code 7" in proc.stderr, "nonzero exit diagnosis missing")
    proc = run([fixture, "segv"], expected=1)
    check("signal" in proc.stderr, "signal diagnosis missing")
    proc = run(["osn_command_that_does_not_exist_28471"], expected=1)
    check("code 127" in proc.stderr and "execvp" in proc.stderr, "exec failure diagnosis missing")
    proc = run([fixture, "memory"], ["-ram", "32"], expected=1)
    if sys.platform.startswith("linux"):
        check("allocation rejected" in proc.stdout, "Linux address-space limit did not reject allocation")
    else:
        check("Memory limit exceeded" in proc.stderr, "macOS RSS limit was not enforced")
    proc = run(["printf", "path-command-ok"])
    check(proc.stdout == "path-command-ok", "bare PATH execution failed")

    with tempfile.TemporaryFile() as f:
        proc = run([fixture, "fd", str(f.fileno())], pass_fds=(f.fileno(),))
        check("fd closed" in proc.stdout, "parent descriptor leaked through exec")
    run([fixture, "nested"])
    check(pathlib.Path("/tmp").is_dir(), "cleanup followed a job-created symlink")

    # Existing job folder ko chhedna nahi hai, chahe abhi usme koi process na ho.
    job_id = 700000 + os.getpid()
    folder = pathlib.Path(f"/tmp/osn_job_{job_id}")
    folder.mkdir(mode=0o700)
    marker = folder / "keep"
    marker.write_text("leave this alone")
    try:
        proc = subprocess.run([runner, "-id", str(job_id), "--", fixture, "hello"],
                              text=True, capture_output=True, timeout=12)
        check(proc.returncode == 1 and marker.read_text() == "leave this alone",
              "directory collision overwrote existing data")
        count += 1
    finally:
        marker.unlink()
        folder.rmdir()

    proc = subprocess.Popen([runner, "--", fixture, "sleep", "5000"],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        time.sleep(0.5)
        proc.send_signal(signal.SIGINT)
        out, err = proc.communicate(timeout=8)
        check(proc.returncode == 2 and "CANCELLED" in err, "driver interrupt did not cancel/reap")
        match = re.search(r"\[RUNNER\] Job (\d+) run", err)
        check(match is not None and not pathlib.Path(f"/tmp/osn_job_{match[1]}").exists(),
              "interrupted job directory leaked")
        count += 1
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()
    print(f"PASS worker driver: {count} process/limit/output/cleanup scenarios (uid {os.geteuid()})")


if __name__ == "__main__":
    main()
