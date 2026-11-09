# Worker startup outcome — 9 November

This Member 2 slice gives the network adapter an asynchronous startup outcome, separate from successful fork. It uses the runner's existing close-on-exec error pipe and adds an ordered worker-loop callback. No shared protocol/transport implementation is changed.

## Runner states

`runner_start()` still returns RUN_OK when process creation succeeds. Check `run_job.exec_state` after polling:

| Value | Meaning |
|---|---|
| RUN_START_PENDING | Startup is not yet observed; do not confirm RUNNING to the scheduler |
| RUN_START_OK | Normal close-on-exec success observation, with no setup error packet or known pre-confirmation stop |
| RUN_START_FAILED | The child reported a setup/exec error, with stage and errno |
| RUN_START_UNCONFIRMED | The child stopped before success could be established; do not guess that it started |

The pipe carries a stage/errno packet on setup or exec failure, and closes on successful exec. A program that actually runs and returns 127 is therefore different from execvp itself failing with 127.

The close-on-exec pipe is the conventional exec observation mechanism, not an application health/readiness check. Unexpected signals can race startup; when the child is already observed as signalled/stopped before confirmation, the runner conservatively reports UNCONFIRMED. An already-observed successful start is retained if that job later crashes or is cancelled. This is not a cooperative application-level "ready" handshake.

## Worker-loop callback

`worker_hooks.start` receives a temporary `worker_start` snapshot with job ID, execution token, exec state, setup stage/errno and reason. Return 0 only when the notification is safely accepted by the caller's queue; return nonzero for retry. Copy any data needed after the callback returns.

With a start hook configured, ordering is:

```text
startup outcome accepted -> output accepted -> final result accepted -> files removed
```

Output stays in the job log while the startup callback is blocked. Final results are also held. After acceptance, `start_reported` prevents duplicate callback delivery. Fast jobs that already exited still report their startup outcome before the terminal result. The final `worker_result` also includes `exec_state` so the receiver can distinguish failed launch from program exit.

The hook is optional for compatibility with existing local runner/loop users. **The production network adapter must configure it** if it needs start-before-output/result ordering. Use designated `worker_hooks` initializers when merging the new header; the callback was appended to that struct.

## Member 3 integration contract

1. A successful `worker_loop_assign()` means the child was forked; do not immediately send JOB_ACCEPTED from that return value.
2. In the start callback, only RUN_START_OK is a successful exec/start notification. Queue its job ID/token on the worker's ordered transport before output or final result.
3. On the scheduler side, match the connection's worker identity and call `job_confirm_start()` for that successful start. The job's reservation is already held.
4. FAILED/UNCONFIRMED are startup outcomes, not successful JOB_ACCEPTED events. Keep their phase/state available in the failure message. A failed startup can be reported before reap; wait for the terminal result before assuming process cleanup is complete.
5. A terminal result for a job with no successful start requires the scheduler's pre-start failure/stop handling. The current Member 1 confirm/finish path covers successful start followed by a result; its failure-to-terminal adapter still needs integration. `job_abort_dispatch()` only releases a confirmed-stopped reservation back to QUEUED; do not pretend it records a FAILED terminal state.
6. Callback acceptance is local queue acceptance, not a remote acknowledgement. Delivery/reconnect handling belongs to the shared transport/session work.

The in-process bridge test exercises successful reserve -> worker start callback -> scheduler confirmation -> output -> terminal result -> resource release. It does not claim that TCP registration, serialization or startup-failure handling on the scheduler is complete.

## Changed project files

Merge `include/runner.h`, `include/worker_loop.h`, `src/worker/runner.c`, `src/worker/loop.c` and the Makefile together. The build now restores the existing worker-loop/config objects, drivers and tests lost during earlier syncs. Those are previously implemented modules, not new networking features.

The development console prints `[Start]` before captured output. Test workloads now include a program returning 127. `tests/test_start_ack.c` covers startup retry/order, exec failure, program exit 127, cancellation before observation and the in-process scheduler bridge. The console test also forces a resource-limit setup failure to check that it never becomes a successful start.

MacOS/runtime network verification remains pending. See `remaining-plan.md` for the revised milestones; no additional backlog features are implemented by this slice.

Verified on WSL Ubuntu: all five startup groups pass as normal user and root, including the in-process scheduler bridge. The console suite also passes under both users, including forced resource-setup failure. The complete configured test suite passes normally and with AddressSanitizer/UndefinedBehaviorSanitizer. Shared protocol/transport compilation is not covered by that suite and remains Member 3's integration dependency.
