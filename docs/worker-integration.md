# Member 2 worker handoff — 21 November 2026

The production worker now executes assignments and speaks the Member 1 scheduler protocol. This branch is based on the Member 1 scheduler-completion commit; the final team repository needs both sets of changes.

```sh
make build/scheduler build/worker
./build/scheduler 8080 PRIORITY
OSN_CACHE_DIR=/tmp/worker_cache_1 ./build/worker -cores 2 -ram 512
# Optional fallback, still discovered over UDP first:
OSN_CACHE_DIR=/tmp/worker_cache_2 ./build/worker 127.0.0.1 8080 -cores 2 -ram 512
make test-worker
make test
```

For evaluation run the worker with sudo, preserving/setting its cache environment as appropriate. Non-root development prints a warning and runs jobs as the worker user. These are the assignment's process limits and privilege drop, not a general secure sandbox.

## Implemented worker behavior

* Strict startup flags, physical CPU/RAM clamping, UDP discovery with three attempts and configured fallback, interruptible discovery and a five-second nonblocking TCP connection deadline.
* Registration waits for WORKER_READY and records the scheduler's worker ID. Registration timeout is ten seconds. Failure/disconnect kills/reaps old executions and cleans their directories before rediscovery/re-registration; IDs/tokens from the old session are never replayed.
* JOB_ASSIGN validates capacities and framing, stages the executable, forks an isolated process group, changes into `/tmp/osn_job_<id>`, drops supplementary groups then gid/uid under root, applies CPU/AS/NPROC/core limits and execvp. Cores and GPUs are reservation bookkeeping, not CPU affinity/GPU isolation.
* Restored asynchronous exec-error-pipe confirmation. Successful exec is acknowledged before output/results. Setup/exec failure sends JOB_FAILED without a fake start acknowledgement; normal exit 127 is distinct. A child killed before exec can be observed is conservatively reported as start-unconfirmed.
* Wall timeout and macOS RSS guard, live Linux/macOS RAM/fault/CPU sampling, peak RAM and exit/signal diagnoses. Linux acts as a subreaper and reaps same-process-group descendants after killing them. Jobs that deliberately escape their process group are outside this limited isolation model.
* Both output streams go to output.log. Bounded incremental frames preserve per-execution byte offsets and backpressure. Output/checkpoint bytes are flushed before job directory cleanup; final stop/result is sent after cleanup so an immediate migration/retry can reuse the required job directory on the same host.
* Heartbeats every two seconds with normalized load and per-job telemetry. Runner polling is 100 ms; state publication is sampled at roughly 500 ms.
* Cancel/preempt send SIGKILL and wait for reap. Migration sends SIGTERM and allows up to two seconds before SIGKILL. Final output/checkpoint processing precedes JOB_STOPPED; intentional stop reason survives a natural-exit race.
* Local stdin `drain` or SIGINT marks DRAINING, informs scheduler, rejects unfinished preparation/assignments, continues active jobs/heartbeats, flushes results, then disconnects. SIGTERM is an emergency abort with kill/reap cleanup. There is no client drain command.
* Writable per-job checkpoint directory, absolute OSN_CHECKPOINT_DIR, restore before privilege drop/exec, bounded regular-file reads with no symlink following and atomic-rename consistency checks. Counter sample resumes after migration. Missing state starts normally; invalid or oversized checkpoint publications are logged and ignored.

## Executable cache

`OSN_CACHE_DIR` defaults to `/tmp/osn_cache_<effective_uid>`. Use a distinct directory per simultaneous worker. It must be owned by that worker user and mode 0700; the worker obtains an exclusive lock to prevent two managers sharing it. Existing cache contents are scanned, stale partial transfers removed, LRU eviction enforced, and inventory is reported incrementally after registration/reconnect.

`OSN_CACHE_SIZE_MB` defaults to 512 and accepts a positive decimal integer. Keys are opaque safe filenames (up to 128 ASCII letters/digits/dot/underscore/hyphen, no leading dot). Linked lists and exact string/byte comparisons are used; **no hash function/table is introduced**.

Transfer chunks must be consecutive, within the declared size and at most 64 KiB each. One executable is capped at 64 MiB in this worker implementation; the cap is a team policy, not a value specified by the brief. Temporary files become cache entries only after the exact byte count is complete. An existing key with different bytes is rejected. Missing references request a full resend using the same job/run token, without starting a job or consuming an attempt. Deleted pinned entries can be repaired without disrupting another execution's staged copy.

**Cryptographic identity remains Member 3's responsibility.** The worker checks transfer length and existing-key byte equality; it does not compute SHA or independently prove a first download matches a claimed digest. Member 3's artifact/identity service must provide that verification to meet the assignment's content-hash requirement, respecting the user's no-hash-functions constraint on this implementation. Do not call an opaque filename cryptographic verification.

Cache entries are pinned from staging through execution/result cleanup. Every program is copied into its private job directory; cached files are never executed in place. LRU is touched when execution is started; nanosecond file timestamps retain ordering on restart. Eviction removes the oldest eligible unpinned entry and informs the scheduler. A new/active entry is protected even when this temporarily exceeds budget; the worker logs it and retries eviction after unpinning.

## Additional wire messages

The shared frame header and messages 1–21 retain the Member 1 contract. Every integer below is big-endian. All execution references are job:u32 and run:u32. The scheduler core still has 64-bit tokens and its adapter refuses to wrap the v1 32-bit range.

| Type | Direction | Payload |
|---|---|---|
| 22 EXEC_BEGIN | Scheduler artifact service → worker | job:u32, run:u32, binary_size:u64, key_len:u16, key bytes |
| 23 EXEC_CHUNK | Scheduler artifact service → worker | job:u32, run:u32, offset:u64, up to 65536 raw bytes |
| 24 EXEC_END | Scheduler artifact service → worker | job:u32, run:u32 |
| 25 EXEC_REFERENCE | Scheduler artifact service → worker | job:u32, run:u32, opaque key bytes (no NUL) |
| 26 EXEC_MISS | Worker → scheduler artifact service | job:u32, run:u32, missing key bytes |
| 27 JOB_OUTPUT | Worker → scheduler output service | job:u32, run:u32, offset:u64, up to 16384 raw bytes |

Full dispatch order: JOB_RESTORE when applicable → EXEC_BEGIN/CHUNK/END or EXEC_REFERENCE → JOB_ASSIGN. Without a transferred/reference executable, JOB_ASSIGN executes the supplied local path/PATH command. It does not magically make a client machine's file available on another machine. If a reference misses, wait for EXEC_MISS, resend full bytes on the same token, then send JOB_ASSIGN. The worker rejects assignments while a reference is unresolved or transfer incomplete.

JOB_STOP intent values are 1 cancel, 2 preempt, 3 migrate. JOB_STOPPED uses the JOB_FAILED result layout and is emitted only once local execution has ended and files are cleaned. A stop received during staging acknowledges no execution and discards its partial file/state. Active task count is bounded to 256; unfinished staging expires after 30 seconds by fencing the session. Retired task metadata is cleared if the scheduler immediately dispatches the same job with a new token.

Worker checkpoint sequences start at 1 for each execution. The scheduler supplies its lifetime number in JOB_RESTORE and independently counts accepted publications; the worker does not reuse that number as its local sequence. Empty saved state is distinct from no saved state.

Heartbeats/output do not require per-frame replies. Start/checkpoint/result/cache/drain replies from the current scheduler use SCHED_REPLY; the worker consumes them. Output is retained by the scheduler in a bounded 64 KiB tail through `sched_output()`, with worker identity, execution token and offset validation. Retained output can span attempts; received offsets reset for each run. This small scheduler adapter addition prevents a working worker's output from being discarded. Member 3's follow subscriptions, client logs formatting/footer and owner routing still need to use that data.

## Testing and integration boundary

Tests cover existing runner, limits, privileges, output/backpressure and start acknowledgement, plus cache LRU/pinning/restart/collision handling and protocol fixtures driving actual child processes. Network tests transfer an executable whose dispatch path does not exist locally, then exercise a cache reference, deleted-cache miss/retransfer, checkpoints, restore, preemption, cancellation, timeout and drain. Reconnect tests check the old child and job directory are gone before accepting new registration.

Real scheduler/worker tests run both one-worker migration fallback and two-worker migration to another worker on the same Linux host. They exercise discovery, submission through a protocol client, real fork/exec, heartbeats, checkpoint handoff and completion with zero retry charge. They use a shared local executable path; production scheduler full-byte/cache-reference selection remains Member 3's artifact-service integration. Protocol fixtures separately prove the worker's actual transfer receive path.

Linux normal and root execution are tested. macOS source paths exist but still need a Mac run, as do real multi-machine/hotspot tests. Member 3's production client/cron/follow/executable service is still absent; `make all` still expects the missing client main. This worker handoff does not claim the whole course project is submission-ready.

Verified on WSL Ubuntu: `make -B test test-worker test-worker-discovery`, worker suites under AddressSanitizer/UndefinedBehaviorSanitizer (`-fno-pie`/`-no-pie` on Linux), and root runs of the runner driver and real-worker network tests. Cache/config/loop/runner/start unit groups have 35/63/55/50/45 checks. The runner driver covers 13 execution/limit/cleanup scenarios. Child workload fixtures deliberately compile without sanitizer instrumentation so their requested address-space limits remain meaningful; the supervising worker and scheduler were instrumented.

For the final repo, merge both scheduler and worker branch changes. This worker slice also changes shared model/protocol definitions, scheduler output handling and Makefile entries. Copying only src/worker omits those required interfaces. Keep tests/programs/counter.c as the cooperative demonstration workload.
