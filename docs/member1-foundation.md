# Member 1: 26–27 October foundation

## Planned tasks and actual delivery

| Planned date | Task | Delivery on 27 October |
|---|---|---|
| 26 Oct | State model and domain headers | `model.h`: every public job/worker state, requested resources, lifecycle timestamps and future retry/dependency/migration/checkpoint fields |
| 26 Oct | Resource/lifecycle ownership and invariants | API contracts below and centralized changes in `lifecycle.c` |
| 26 Oct | Queue/retry policy agreement | Baseline assumptions below; team review still needed for unspecified assignment cases |
| 27 Oct | Job and worker registries | Linked-list creation/lookup/cleanup, unique IDs, stable record addresses |
| 27 Oct | Lifecycle skeleton | Guarded queued→running→terminal path, pending cancellation, state-edge reference, no double resource release |

The interactive driver and tests support checking this foundation. They do not replace the later event loop, scheduling policy or real worker acknowledgements.

## Read the code in this order

1. `model.h`: job requirement, current state, worker capacity and scheduler-owned lists.
2. `registry.c`: initialization, validation, insert, find and free.
3. `lifecycle.c`: fit checks, start/finish and pending cancellation.
4. `test_scheduler.c`: examples of valid calls and edge cases.
5. `scheduler_demo.c`: a reusable command driver around the same API.

Practical viva points: lookup is O(n), append is O(1) with a tail, IDs are separate from list addresses, and requested resources are separate from later measured usage.

## API contracts

- Call `sched_init()` on a fresh context before using it. Do not call it on a populated context: use `sched_free()` first. `sched_free()` releases records and owned dependency/checkpoint buffers and resets the context. It is safe to call twice after initialization.
- The scheduler owns all records. Pointers returned by `job_find()`/`worker_find()` remain valid until `sched_free()`. Treat them as read-only outside scheduler-owned implementation. Do not `free()` them individually or change state/resource counters directly.
- `job_add()` copies the fixed-size request, so the caller can reuse its request afterwards. It records a path, not file content; reading a path is Member 3's artifact-service responsibility.
- Both worker and job resources must have positive cores/RAM and nonnegative GPUs. Priority is 1–3; runtime must be positive. Usernames follow the specified ASCII set and length 1–31. Fixed-size input arrays must be NUL terminated.
- Resource units are cores, MB of RAM and GPU count. The scheduler does not detect physical machine capacity or enforce OS limits; the worker handles those.
- `job_start()` currently performs reservation and RUNNING transition together. A later network dispatch path must split preparation/reservation from confirmed start so two transfers cannot reserve the same free capacity. Do not call this function blindly on an unconfirmed network send.
- `job_finish()` only accepts a RUNNING job, its exact execution ID, a terminal result and a nondecreasing timestamp. Call it after the worker confirms exit/stop, not immediately when a cancel request is sent. The first valid result releases resources; duplicates cannot release them again.
- `job_cancel_pending()` cancels QUEUED or WAITING records; it cannot kill a running process. WAITING is part of the model but dependency-aware submission is not exposed yet.
- `job_can_move()` describes the full allowed edge set, including future recovery/preemption edges. It is not a generic transition function. The currently implemented mutation functions expose only the foundation's supported operations.
- `worker_set_state()` refuses to mark a worker DEAD while active jobs remain. The later recovery module must atomically handle LOST/requeue before finalizing death. The guard deliberately prevents a partial implementation from silently orphaning jobs.
- All calls are currently single-thread-owned. Future `poll()` handlers should run them in the scheduler loop. They are not a general thread-safe library for concurrent callers.
- Caller supplies monotonic timestamps in milliseconds. The driver uses `CLOCK_MONOTONIC`; unit tests inject timestamps so they do not sleep.

## Data reserved for later work

Failure count and run ID are separate. A run ID changes each dispatch; migration/preemption must not consume failure attempts. The constant `MAX_ATTEMPTS` is 3; automatic retry accounting is not implemented yet.

Dependency IDs, preemption hold, migration source, stop intent, telemetry and checkpoint metadata are initialized to zero/NULL. Their presence does not mean those features already work. Member 3's output service can own retained log buffers separately from this record.

## Working policy decisions to review with teammates

1. IDs start at job 1 / worker 1001 and increase within one scheduler lifetime; reconnect later means a newly registered worker ID. Persistence across scheduler restarts is not included.
2. Pending admission capacity is 1024; currently every accepted job begins QUEUED. Cancellation/start frees a pending slot. Finished records stay queryable and do not count against pending admission.
3. When dependencies are added, accepted WAITING jobs will also count against pending admission and will stay out of runnable sorting.
4. Internal requeues must never be dropped. A separate recovery backlog/merged ordering is the proposed later solution; there is no requeue API in this slice, so its saturation behavior is not implemented or claimed tested.
5. DEAD records remain in the registry and cannot be revived; a reconnect adds a new worker record.
6. No hash function or hash-based lookup is used. Future cache-aware policy should query Member 3's cache-presence service rather than compute executable identity in Member 1 code.

## Test coverage

`make test` exercises eight C groups: registry lifetime, input validation, resource fit, result handling, worker states, queue capacity, integer/ID boundaries and the full state-edge table. It also runs command-driver sequences, including optional/default GPU, stale/duplicate results and malformed input.

Individual groups are runnable, for example:

```sh
./build/test_scheduler resources
./build/test_scheduler capacity
```

Actual Linux/WSL results are recorded in `progress.md`. macOS has not been tested in this workspace. None of these tests claim to validate socket concurrency, actual signals/limits, retries or distributed execution.
