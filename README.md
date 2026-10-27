# OSN Course Project — Distributed Compute Cluster

Current work: **Member 1's foundation for the 26–27 October tasks**, implemented on 27 October 2026. The full assignment is not implemented yet.

## What works so far

- Job and worker records, including all assignment state names.
- Linked-list registries, unique IDs and input validation.
- Bounded admission of 1024 pending jobs with `QUEUE_FULL`.
- Explicit start/finish operations and accounting for cores, RAM and GPUs.
- Execution IDs that reject stale results, and duplicate-result protection.
- Worker availability checks and guarded state changes.
- Interactive core driver and automated boundary/accounting tests.

There are no hash functions or hash tables in this implementation. Registries use linked lists with linear lookup.

## Build and test

Use a C11 compiler, Make and Python 3 in Linux, WSL Ubuntu, or macOS. Python is only for a driver test; the core itself is C.

```sh
make
make test
make sanitize
```

`make sanitize` does a clean rebuild with AddressSanitizer and UndefinedBehaviorSanitizer and runs the same tests. Sanitizer support depends on the compiler/runtime.

From PowerShell in this Windows folder:

```powershell
wsl -d Ubuntu --cd '/mnt/c/Users/Aryan/Desktop/OSN Course Project' --exec make test
wsl -d Ubuntu --cd '/mnt/c/Users/Aryan/Desktop/OSN Course Project' --exec ./build/scheduler_demo
```

If you ran the sanitizer target and want a normal build again, run `make clean` followed by `make test`.

## Try the scheduler core

```sh
./build/scheduler_demo
```

The driver accepts your own commands repeatedly. For example:

```text
worker 4 2048 1
submit aryan ./hello 2 512 10 2 1
submit kaush ls 1 128 5 3
nodes
start 1 1001
status 1
finish 1 1 COMPLETED
cancel 2
nodes
quit
```

`start` prints the execution ID; use that ID with `finish`. IDs start from 1 for jobs and 1001 for workers in each new driver session. RAM values are MB. Run `help` for the complete driver commands.

**This driver only changes in-memory records.** It does not open sockets, read/transfer the executable, run programs, or verify that a path exists. Its `finish` command simulates a confirmed worker result, and `worker-state` simulates a state transition. Paths in this driver cannot contain spaces. This is not the assignment's final scheduler/client CLI.

## File ownership

| Files | Purpose |
|---|---|
| `include/model.h` | Shared job/worker/resource data model |
| `include/scheduler.h` | Member 1 core API and error codes |
| `src/scheduler/registry.c` | Record creation, lookup, validation and memory cleanup |
| `src/scheduler/lifecycle.c` | State rules, basic start/finish/cancel and resource accounting |
| `tools/scheduler_demo.c` | Interactive standalone driver for the core |
| `tests/test_scheduler.c` | Unit tests with selectable test groups |
| `tests/test_demo.py` | Driver tests with multiple command/input sequences |

The Makefile builds a core library and driver only. Member 3 can extend it with the final scheduler, worker and client targets when transport integration is ready; there are no placeholder worker/client executables.

## What comes next

The next Member 1 task is the scheduler event loop and service boundaries. FIFO/PRIORITY/aging, dispatch-in-progress reservations, automatic heartbeat handling, retries, dependencies, quotas, preemption and migration remain to be implemented. Worker execution belongs to the separate worker workstream. A live TCP scheduler cannot be claimed complete based on this foundation.

See `docs/member1-foundation.md` for API contracts and integration notes, `docs/progress.md` for actual progress, and `IMPLEMENTATION_PLAN.md` for the original requirements and full baseline schedule.
