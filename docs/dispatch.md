# Dispatch reservation and start confirmation

Member 1 slice implemented on 9 November 2026. This separates resource reservation from confirmed execution; it does not send assignments or implement a scheduling policy.

## API sequence

1. After choosing a worker, call `job_reserve(s,id,worker_id,now,&run_id)`. It checks eligibility and free cores/RAM/GPUs, reserves all three, and allocates a fresh execution token. The public state stays QUEUED with `dispatch_pending=1`.
2. Send the assignment through the transport. Its messages must carry the job ID, execution token and the bound worker identity.
3. When that worker confirms the program actually started, call `job_confirm_start(s,id,worker_id,run_id,now)`. The state becomes RUNNING, the pending-admission slot is released, and reserved count moves to running count. Resources are not added a second time.
4. Apply final results with the existing `job_finish()` only after start confirmation. It releases held resources once and records the terminal state.

The local `job_start()` helper still works for the standalone scheduler driver and existing tests. It reserves and confirms immediately in one owner-thread call. Never use that shortcut merely because a network send succeeded.

## Accounting

| Stage | Public state | Dispatch pending | Pending queue slots | Worker reserved count | Worker running count |
|---|---|---:|---|---|---|
| Before assignment | QUEUED | 0 | Held | Unchanged | Unchanged |
| Preparation/transfer | QUEUED | 1 | Still held | +1 | Unchanged |
| Start confirmed | RUNNING | 0 | Released | -1 | +1 |
| Confirmed pre-start abort | QUEUED | 0 | Still held | -1 | Unchanged |

`worker.used` includes both reservations and running jobs. `worker.jobs` counts confirmed running jobs only. `reserved_jobs` exposes the preparation count in nodes snapshots; status snapshots include `dispatch_pending` and `dispatch_ms`.

Keeping the pending slot during preparation means an abort cannot lose the job when admission is full. Abort does not allocate or reinsert a record, reset submission order, or consume the worker-failure retry budget. Worker-loss rescue of already RUNNING jobs remains a separate unfinished feature.

## Abort and cancellation boundary

Call `job_abort_dispatch()` only when the worker has confirmed that execution did not start, or has stopped it, or a later fault-recovery mechanism has established that the worker/session is dead. The current function does not itself establish any of those facts.

A failed write/timeout alone is not proof that the worker never received the assignment. Do not release the reservation and dispatch a second execution merely because a socket operation failed. Recovery/handshake integration must resolve that ambiguity.

Abort releases the reservation and leaves the job QUEUED. A new reservation gets a new execution token. Late acknowledgements/results for an earlier token or the wrong worker are rejected. Duplicate confirmation/abort cannot change accounting twice.

Pending cancellation returns S_BUSY while a dispatch reservation exists. Coordinate a remote stop/abort first, then use `job_cancel_pending()`. Similarly, the existing worker-state helper refuses DEAD while there are running or reserved jobs; the future recovery path must reconcile them before marking death.

A valid acknowledgement of an existing reservation can arrive while a worker is SUSPECTED or DRAINING; it can still confirm that already-assigned job. Those states remain ineligible for a new reservation. DEAD acknowledgements are rejected.

## Integration still needed

The worker runner currently reports successful fork before exec confirmation. The production worker must implement an actual start acknowledgement before the scheduler uses `job_confirm_start()`. Connection/session checks must ensure the supplied worker identity belongs to the sender.

Automatic reservation deadlines/recovery, running-job retries, pre-start failure-to-terminal handling, pending cancellation handshake, lifecycle event routing and full timestamped logging remain later work. The protocol/transport files are unchanged by this slice.

## Files and checks

New source: `src/scheduler/dispatch.c`. Shared model/API edits are in `include/model.h`, `include/scheduler.h`, `include/scheduler_service.h`, `src/scheduler/lifecycle.c` and `src/scheduler/service.c`. Merge all of them together, plus the Makefile's dispatch object rule; copying only the new C file is insufficient.

`tests/test_dispatch.c` covers reservation counters, nodes/status snapshots, no overcommit, confirmation, duplicate/stale/wrong-worker messages, abort/reassignment, pending cancellation and rollback at full queue capacity. Existing local-start tests remain regression coverage.

Verified on WSL Ubuntu: four dispatch groups (1096 checks), the existing configured scheduler/runner suites and driver tests pass. The same configured suite passes with AddressSanitizer and UndefinedBehaviorSanitizer. This build does not include the unfinished common protocol/transport implementation; its compile failures are tracked in `remaining-plan.md`. macOS execution remains unverified.
