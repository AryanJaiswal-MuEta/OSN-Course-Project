import subprocess
import sys


def run(path, commands):
    proc = subprocess.run([path], input=commands, text=True,
                          capture_output=True, timeout=10)
    if proc.returncode:
        raise RuntimeError(proc.stdout + proc.stderr)
    return proc.stdout


def check(ok, msg):
    if not ok:
        raise RuntimeError(msg)


def main():
    path = sys.argv[1]
    out = run(path, """worker 4 2048 1
submit aryan ./hello 2 512 10 2 1
submit kaush ls 1 128 5 3
start 1 1001
nodes
finish 1 999 COMPLETED
status 1
finish 1 1 COMPLETED
finish 1 1 COMPLETED
cancel 2
nodes
status 1
status 2
quit
""")
    check("job submitted: 1" in out and "job submitted: 2" in out,
          "submit with/default GPU did not both work")
    check("1001 ALIVE 2/4 1536/2048 0/1 1" in out,
          "running resource accounting is wrong")
    check("older execution" in out and "State:RUNNING" in out,
          "stale result changed the active job")
    check("State:COMPLETED" in out and "State:CANCELLED" in out,
          "final states missing")
    check("1001 ALIVE 4/4 2048/2048 1/1 0" in out,
          "resources were not restored")

    # Galat input pe process band nahi hona chahiye; next valid command bhi chale.
    out = run(path, "x" * 2200 + "\nworker -1 1 0\nworker 1 256 0\n"
              "submit bad/name ls 1 1 1 1\n"
              "submit aryan ls 1 128 5 2 -1\n"
              "submit aryan ls 1 128 5 2\nstart 1 1001\n"
              "finish 1 -1 COMPLETED\nstatus 1\nquit\n")
    check("input too long" in out, "long line was not rejected")
    check("worker registered: 1001" in out and "job submitted: 1" in out,
          "bad input broke the next command")
    check("State:RUNNING" in out, "negative token incorrectly completed a job")
    print("PASS interactive driver: multiple jobs, both submit forms, stale results, bad input")


if __name__ == "__main__":
    main()
