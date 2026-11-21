# Member 1 completion check — 22 November 2026

Based on GitHub main 49f4f0a (Member 3 client upload). This scheduler-only follow-up closes core edge cases and service query boundaries; it does not implement Member 3's client/cron/follow/artifact services or introduce hash functions.

## Final scheduler fixes

* PRIORITY preemption now continues past an impossible priority-1 candidate to a later feasible candidate. FIFO still blocks at its first eligible head. No victim is stopped until a complete resource-feasible plan exists.
* Preemption holds release when the worker drains, the beneficiary ceases to qualify, or the conditional user quota changes. Outstanding stop requests still require confirmation; releasing a hold never releases resources belonging to a live execution.
* Repeated cancellation is idempotent. It keeps the original stop deadline and already-sent stop token, preventing a stream of owner cancellations from extending an unresponsive execution indefinitely. Cancel still overrides migration/preemption.
* Worker transport backpressure pauses selection instead of treating a full send queue as worker failure. A dispatch (checkpoint then assignment) is bounded to the existing frame/checkpoint limits. Further dispatches to that worker wait until its pending queue flushes, and the flush requests another immediate scheduling pass. A healthy worker is not disconnected merely because multiple 512 KiB checkpoint restores are ready together.

`sched_worker_writable(s,id,0/1)` is the core boundary for transport readiness. It changes neither worker capacity nor retry accounting. The production loop maintains it automatically. An additional artifact adapter should respect this boundary rather than repeatedly reserving work into a full socket queue. A socket failure still fences the whole old session and uses the normal recovery path.

## Member 3 service boundaries

Use the session username for these calls. They reject another owner's job with `S_FORBIDDEN` (11); existing error numbers 0–10 remain unchanged. The basic client already handles nonzero scheduler errors generically.

| API | Purpose |
|---|---|
| `sched_status_owned()` | Copy an authorized, pointer-free status snapshot |
| `sched_cancel_owned()` | Authorize cancellation and wake scheduling after success |
| `sched_migrate_owned()` | Authorize migration and wake scheduling after success |
| `sched_output_get()` | Copy the retained binary output tail with execution/state metadata |
| `sched_checkpoint_get()` | Copy latest checkpoint bytes with lifetime number and receipt timestamp |

For output/checkpoint size queries, pass `buf=NULL,cap=0`. A nonempty result returns `S_BUFFER_SMALL` and supplies the required size in the metadata. A short buffer is never partially written. Successful calls copy bytes into caller-owned storage; there are no borrowed internal data pointers. Another owner's query returns zeroed metadata and leaves its supplied data buffer untouched. Empty checkpoint number 0 means no checkpoint; a nonzero number with size 0 is a published empty checkpoint.

The output tail may contain bytes from earlier attempts. `current_run_received` counts bytes for the current execution only; it resets on reserve. Use `run_id` when handling a restart, rather than interpreting the whole retained tail as a single execution's offsets. Binary output is not NUL-terminated text.

Status snapshots now also include submission/start/end times, accumulated queue wait, dependency count and retained output size. Node snapshots expose whether dispatch is paused by transport backpressure. These are service metadata; the client need not expose implementation fields unless useful to its user.

Cron occurrences should keep calling `sched_submit()` with the template's immutable requirement and session/template owner. It uses the same 1024 admission bound, ID allocation, quota/policy and lifecycle as ordinary submissions. No internal retry may be admitted as a fresh cron/user submission. Cron template timing, delayed-occurrence policy and client commands remain Member 3's service work; adding a second scheduler-owned cron implementation would conflict with that ownership.

Logs/follow should read `sched_output_get()` and use their own bounded subscriptions and completion footer. Artifact services should read `sched_checkpoint_get()` before staging binary/reference bytes, and continue to supply opaque content identities through the existing cache placement APIs. This patch supplies safe data/control APIs; the current CLI logs request remains unsupported until its service handler is integrated. CLI validation, stdin handling, discovery and content verification defects from the audit also remain Member 3 work.

## Verification

* Full `make test test-worker`: existing lifecycle/policy/service/dispatch tests, shared framing/transport tests, real one/two-worker execution/migration tests and new final scheduler regressions.
* Added regressions for preemption starvation, transport-paused workers, quota/drain hold release, repeated cancellation deadlines, owner isolation and binary-safe copy APIs.
* Real TCP tests restore three simultaneous 512 KiB checkpoints without worker loss, and keep sending cancellation plus heartbeat packets to prove a refusing worker is fenced after its original stop deadline.
* Restored the signedness casts and bounded-read return check overwritten in the Member 3 sync, so the project's strict test build is usable again.
* Focused AddressSanitizer/UndefinedBehaviorSanitizer builds exercise core and TCP regressions. Linux is verified; macOS and real multi-machine/hotspot validation remain joint release gates.
* Final focused regression suite: 84 checks, alongside 1,272 advanced scheduler checks. Actual worker integration passes under normal and root execution, including one-worker fallback and two-worker migration.
* Scheduler load rerun: 64 actual CLI processes completed 256 jobs in 13.25 seconds; four simultaneous 1 MiB output jobs, cancellation and checkpoint migration completed. A non-reading peer issuing 10,000 queries did not stall the monitor (worst measured response 1.35 ms in that run). With no workers, 1,100 CLI submissions admitted exactly 1,024 and rejected 76. These bounded local tests do not replace a real multi-machine test or clear the separately reported Member 3 CLI failures.

The independent Member 1 implementation is complete against these interfaces. Cross-member integration is still required before declaring the entire project complete.
