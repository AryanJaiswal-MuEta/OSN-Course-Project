# OSN Course Project — Distributed Compute Cluster

C foundations for a distributed compute cluster. Current code contains a scheduler registry/lifecycle library and a local worker process runner. The full networked scheduler, worker and client are still in progress.

## Build and test

Linux/WSL or macOS with a C11 compiler, Make and Python 3 for tests:

```sh
make
make test
make sanitize
```

Sanitizer builds replace the normal objects. Run `make clean` then `make` for a normal build afterwards. Linux/WSL normal-user and root worker tests have passed. macOS code is present but has not been verified here.

## Project files

| Location | Purpose |
|---|---|
| `include/model.h` | Shared jobs, resources, states and registry records |
| `include/scheduler.h`, `src/scheduler/` | Scheduler registry, validation and basic lifecycle/accounting |
| `include/runner.h`, `src/worker/` | Actual process execution, limits, output, usage sampling and cleanup |
| `Makefile` | Library, development-driver and test builds |
| `tools/` | Standalone development drivers; not the final production CLIs |
| `tests/` | API/driver checks and controlled workload programs |
| `docs/scheduler-core.md` | Scheduler API contracts and current limitations |
| `docs/worker-runner.md` | Runner API, final-repo file list and integration notes |

Registries use linked lists. No hash functions or hash tables are used in the current logic. Comments in C are practical Hinglish notes.

## Scheduler core driver

```sh
./build/scheduler_demo
```

Example commands:

```text
worker 4 2048 1
submit aryan ./hello 2 512 10 2 1
start 1 1001
status 1
finish 1 1 COMPLETED
nodes
quit
```

This driver changes records only. It does not execute a program, transfer files or open sockets. `help` lists its commands.

## Local worker runner

```sh
make all build/test_job
./build/worker_runner -- ./build/test_job hello
./build/worker_runner -time 1 -- ./build/test_job sleep 5000
./build/worker_runner -- printf path-command-ok
```

The runner forks/execs a real job in its own directory, captures both output streams, applies limits, enforces timeout and reaps it. Under sudo/root it drops the job to `nobody`; otherwise it warns. The driver renders captured output for inspection. The library delivers output to a callback for future network integration.

## Remaining integration

Scheduler policies, network dispatch and recovery are pending. Worker discovery/registration, heartbeats, draining, cache and checkpoints are pending. The two libraries are not yet connected through a production transport. See the API notes before integrating with the team's shared headers and Makefile.

Build artifacts, local planning notes and question screenshots are excluded from the current source tree's tracked project files. The final submission needs the completed three-program source/build setup; these foundations alone are not the full assignment.
