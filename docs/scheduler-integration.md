# Member 1 scheduler handoff — 21 November 2026

**22 November follow-up:** [final Member 1 scheduler check](member1-final.md) documents the preemption/cancellation/backpressure fixes, owner-checked query APIs and final tests.

The scheduler core and TCP adapter are implemented. Build on Linux/macOS:

**Worker handoff update:** the [Member 2 worker implementation](worker-integration.md) now executes real jobs. One-worker and two-worker loopback integration tests have been added there; the scheduler-only fixture tests described below still remain separate from those real-process tests. Output frames are now retained through `sched_output()`.

```sh
make build/scheduler
./build/scheduler 8080 PRIORITY -discovery-port 9999
make test-scheduler
make test
```

The executable takes real connections and uses the shared framing, transport and UDP discovery implementation. The scheduler-only tests use simulated protocol peers; the Member 2 handoff adds separate tests with real workers and child processes. Member 3's production client and artifact/follow/cron services are still needed for the full distributed cluster interface.

## Member 1 coverage

| Area | Implementation |
|---|---|
| Startup and concurrency | Strict CLI, checked socket setup/cleanup, poll loop, bounded peers, nonblocking writes, bounded reads per peer, one-second scheduling ticks |
| Registration | Validated capacities, unique worker ID, socket-to-worker binding and WORKER_READY reply |
| Admission and accounting | 1024 pending external admissions, reservation/start barrier, conserved cores/RAM/GPU, no dropped internal requeues, stale report rejection |
| Ordering | FIFO head blocking; PRIORITY with cumulative queued-time aging every 10 seconds and stable submission ties |
| Placement | Eligible ALIVE workers, migration-source avoidance, idle → opaque content identity present → load → active/reserved count → worker ID |
| Recovery | SUSPECTED after 2 seconds, DEAD after 6 seconds; heartbeat clears suspicion; socket loss fences the old session immediately; third lost attempt is FAILED_PERMANENT |
| Dependencies | Existing job IDs or group snapshots, deduplication, WAITING release, iterative failure/cancellation cascade |
| Quota | Current RUNNING cores only; all non-DEAD capacity in denominator; exactly half allowed; competing user required; skip before FIFO fit check |
| Preemption | Effective priority 1, feasible worker before mutation, preemptible victims in lowest-priority/newest order, minimum sorted prefix sufficient for all resources, beneficiary holds and worker capacity protection |
| Cancel/drain | Intent first, release after stopped/result; queued cancellation immediate; DRAINING excludes new jobs while active jobs keep running |
| Migration/checkpoints | RUNNING validation, stop intent, stop-confirmed requeue without failure charge, latest checkpoint retained, different fitting worker preferred, restore frame queued before assignment |
| Queries/logs | Owner-bound basic client queries, pointer-free service snapshots, shared timestamped log_line helper and state/decision logs |

## Contracts for Member 3

Call `sched_submit()` or `sched_submit_deps()` using the session username, not an owner supplied by another client. Cron copies use the ordinary admission API; cron template timers and full-queue occurrence policy remain in Member 3's service.

`job_add_deps()` accepts `{id,group}` references. `group=0` means one job; `group=1` expands every existing job in that group at submission time. Unknown references reject before creating a job. References can only point to existing jobs, so new jobs cannot introduce cycles. WAITING consumes admission capacity but does not age. Previously accumulated QUEUED time survives restarts. Running time does not age a victim's priority.

Use `job_reserve()` before preparing an assignment, `job_confirm_start()` after confirmed exec, and `sched_report()` for terminal worker reports. A socket send failure must fence the session, not release a possibly running execution on its own. `job_abort_dispatch()` is for positively confirmed no-execution/reaped preparation aborts only. A launch error becomes FAILED without a fake RUNNING interval; ordinary program failure is not retried. Worker/session loss consumes the three-attempt failure budget, including a dispatch whose start outcome was lost.

Use `sched_cancel()`/`sched_migrate()` to set stop intent. Send the matching stop request and report only after process-group termination/reap and final output/checkpoint processing. A result racing with a pending intentional stop follows that stop intent. Preemption/migration restart without failure charge; worker death during either still counts as a worker-loss failure. Cancellation wins over restart when the owner has requested it.

Core changes publish log events through `scheduler.event`; callbacks must not mutate the scheduler recursively. Existing service callbacks request another coalesced scheduling pass. If Member 3 adds another adapter, it must request a pass after its successful core mutations. The production adapter already does this.

`sched_content_set()` attaches a Member 3-supplied opaque content identity; `sched_cache_set()` maintains per-worker presence/eviction information using string comparisons. No hash function/table is introduced in scheduler logic. The basic path assignment adapter does not transfer executable bytes or select a cache-reference message. Member 3's artifact service must attach identities and implement full transfer/reference/miss coordination through this reservation boundary. Do not claim path execution proves executable transfer.

Checkpoint storage is bounded to 512 KiB per job (a documented team limit, not specified by the brief). `sched_checkpoint()` validates session/token, accepts increasing worker sequence numbers within each execution and increments the scheduler's lifetime checkpoint number. A restarted worker may restart its sequence at 1. Latest checkpoint bytes survive all requeue paths. Member 3 may feed checkpoint transport into this API; status includes lifetime number, size and receipt time.

Logging currently goes to stdout. Member 3 can route the shared helper to their desired sink. Output retention/follow subscriptions, asynchronous owner-event routing, binary/cache transfers and cron services remain Member 3's responsibilities; this patch does not implement them. Client username binding follows the assignment's login model; it is not password authentication.

## Wire additions for Member 2/3

The 16-byte shared frame header remains unchanged. All integers below are unsigned big-endian. Existing message numbers 1–7 keep their payload layouts in `protocol.md`. The following semantics/additions are authoritative for this scheduler adapter:

* `JOB_ACCEPTED` (4) means **successful exec observed**, not just receipt. Payload remains job:u32, run:u32. Failed child setup/exec sends JOB_FAILED while reservation is pending. Finish/output must follow start acknowledgement in wire order for a successfully started program.
* Every job report is bound to the registered worker socket and its current job/run token. Another worker cannot report its execution. Duplicate/late reports do not change accounting.
* The core uses 64-bit execution tokens. The v1 job wire format uses 32-bit tokens; this adapter stops assigning at UINT32_MAX rather than wrapping/truncating. A later shared protocol version can widen the fields together.

| Type | Direction | Payload |
|---|---|---|
| 8 WORKER_READY | Scheduler → worker | worker_id:u32, correlated registration request ID |
| 9 WORKER_HEARTBEAT | Worker → scheduler | load:u32 (hundredths of percent, 0–10000), count:u32, then count entries: job:u32, run:u32, live_ram_mb:u64, peak_ram_mb:u64, faults:u64, cpu_ms:u64 |
| 10 JOB_STOP | Scheduler → worker | job:u32, run:u32, intent:u8 (1 cancel, 2 preempt, 3 migrate) |
| 11 JOB_STOPPED | Worker → scheduler | Same payload as JOB_FAILED: job:u32, run:u32, peak_ram_mb:u64, faults:u64, cpu_ms:u64, reason_len:u16, reason bytes |
| 12 WORKER_DRAIN | Worker → scheduler | Empty; only worker's local drain/interrupt initiates this |
| 13 JOB_CHECKPOINT | Worker → scheduler | job:u32, run:u32, sequence:u64, size:u32, state bytes |
| 14 WORKER_CACHE | Worker → scheduler | present:u8 (0 eviction/miss, 1 present), nonempty opaque identity bytes, no NUL, max 128 bytes |
| 15 CLIENT_LOGIN | Client → scheduler | Username bytes, no NUL, 1–31 bytes; login once per connection |
| 16 NODES_QUERY | Client → scheduler | Empty |
| 17 STATUS_QUERY | Client → scheduler | job:u32 |
| 18 JOB_CANCEL | Client → scheduler | job:u32, owner only |
| 19 JOB_MIGRATE | Client → scheduler | job:u32, owner only |
| 20 SCHED_REPLY | Scheduler → peer | sched_err:u32 (enum scheduler.h), id:u32 (0 if absent), text_len:u16, text bytes |
| 21 JOB_RESTORE | Scheduler → worker | job:u32, run:u32, lifetime_checkpoint_number:u64, size:u32, state bytes |

JOB_SUBMIT retains its existing fields, including user; user must match login. It optionally appends dep_count:u16 followed by dep_count entries of group_flag:u8 and id:u32. No trailer means no dependencies. Submission success reply contains the new job ID. Queries return human-readable text in SCHED_REPLY. Nodes response is bounded; oversized responses return S_BUFFER_SMALL (the in-process snapshot API supports caller-sized arrays). Future richer/paged client service responses can replace this basic adapter together with the client.

JOB_RESTORE is sent before JOB_ASSIGN for a job with a checkpoint, even if the latest checkpoint is empty. Worker must stage it by job/run, then restore before exec. JOB_ASSIGN still carries the original fields and a zero checkpoint_size, because state was sent first. Without a saved checkpoint only JOB_ASSIGN is sent. Request ID 0 marks unsolicited assignment/stop/restore frames.

Heartbeats with valid framing refresh worker liveness; stale per-job entries are ignored. Their count is not trusted for scheduler resource accounting. WORKER_DRAIN retains the original heartbeat deadline. Fresh connection registration always gets a new worker ID; a DEAD session is never revived. Worker must kill old local processes on disconnect before re-registering.

Scheduler startup accepts at most 256 simultaneous peers. Unidentified peers expire after 10 seconds. An unconfirmed dispatch after 30 seconds, or an unacknowledged stop after 10 seconds, closes/fences the whole worker session. It never releases resources while pretending the same remote session is still usable. Worker migration must implement SIGTERM → 2-second grace → SIGKILL/reap before JOB_STOPPED. These worker execution steps are Member 2's next integration requirement.

## Verification and remaining joint work

`make test-scheduler` runs existing lifecycle, service, loop, dispatch and policy regressions, new conservation/advanced-policy tests and real TCP/UDP tests with protocol fixtures. Network fixtures check registration, fragmented frames, start barrier, wrong-worker/stale reports, live telemetry, migration restore ordering, cancel, heartbeat death, retries and dependency cascades. They do not fork a user job.

`make test` also runs shared framing/discovery/transport and the existing in-process integration tests. The missing `src/client/main.c` means the unrelated `make all` target still needs Member 3's client. macOS, sudo execution and a real multi-machine worker/client run are joint release gates and have not been verified by this scheduler handoff.

Verified on WSL Ubuntu on 21 November: strict `make -B test build/worker`; scheduler suites with AddressSanitizer and UndefinedBehaviorSanitizer; 1,249 advanced checks in addition to existing lifecycle/service/dispatch tests. The sanitizer run used `-fno-pie`/`-no-pie` on Linux. Archive recipes recreate their generated archive so old members from earlier build layouts cannot mask missing dependencies.

For the final team repository, copy the changed scheduler source files plus the changed model/scheduler/service headers, merge the shared protocol/logging/transport changes, and merge the Makefile entries. Include the test files and this guide for reproducible verification. Copying only `src/scheduler` will miss required shared definitions. Do not replace a newer teammate's shared files wholesale.
