# Worker loop skeleton — 28 October task

Implemented on 29 October for Member 2's 28 October work. The earlier runner already provided job-directory/executable preparation; this slice reuses it inside a nonblocking worker loop. It does not add a competing network protocol or the final worker CLI.

## Files for the final team repo

```text
include/worker_loop.h
src/worker/loop.c
```

Merge the Makefile additions: `worker_loop.o` belongs in `librunner.a`, and `worker_loop.h` belongs in the runner's header dependencies. The existing runner/model/files/platform code is still required. The development-only additions are `tools/worker_loop_demo.c`, `tests/test_worker_loop.c` and `tests/test_worker_loop.py`.

## API and ownership

1. Initialize a fresh `worker_loop` with callbacks using `worker_loop_init()`.
2. Attach a nonblocking scheduler connection through `worker_loop_watch(loop,WORKER_CONN,fd,events)`. An optional local-command fd uses WORKER_ADMIN. The function sets O_NONBLOCK/FD_CLOEXEC; neither slot owns/closes the fd.
3. In the I/O callback, decode incoming frames through the shared transport. After a complete assignment and local executable preparation, call `worker_loop_assign()` with the job ID, run ID, requirements and argv. The existing runner stages local paths into the job directory; bare PATH commands need no copy.
4. Run `worker_loop_step()` within an existing driver, or `worker_loop_run()` until stop. Its return convention is 0 on success and -1 with errno on loop errors. Assignment/cancel/init functions use the RUN_* codes.
5. The loop polls running jobs at a nominal 100 ms cadence, forwards output, and invokes the final-result callback only after all captured output has been accepted and the child has been reaped.
6. Return 0 from the result callback only when the result is safely accepted by the transport/service queue. Nonzero keeps the job record and directory for retry. Successful acceptance removes the record and cleans its files automatically.
7. Stop the loop, then free it. `worker_loop_free()` aborts/reaps remaining jobs through `runner_free()`; it does not flush pending transport data or close caller-owned descriptors.

The result callback receives a temporary pointer-free snapshot. Copy it if needed after the callback returns. It contains ID/run token, terminal state, usage and diagnosis. Do not retain the callback pointer.

Output and result callbacks both receive the same context arg. Their acceptance must preserve ordering in the transport queue: output before result. They are queue-acceptance hooks, not proof that a remote scheduler has acknowledged delivery. A real delivery-ack protocol is still pending.

Only the worker owner thread may call this API. Reentrant stepping is rejected. Do not free the loop or mutate/remove runner records from a callback; the loop owns final-result cleanup. Assigning another job or requesting stop from a callback is supported. Same-job execution replacement must wait for removal of the retained old record.

## Readiness and timer hooks

The I/O callback receives the ready fd and POLLIN/POLLOUT/error/hangup flags. Keep it bounded and nonblocking. Enable POLLOUT only while output is queued, and remove a watch with fd=-1 before closing/replacing it. Watch tags prevent an old readiness event from being sent to a replacement with the same fd number.

The tick callback runs alongside job polling, about every 100 ms. This is a future hook for heartbeat/admin bookkeeping, not an implemented heartbeat sender. All work is cooperative on one thread; blocking callback work or a large synchronous local executable copy can delay the loop. Transfers/caching need a later preparation stage outside time-sensitive socket handling.

`worker_loop_stop()` ends stepping; it is an abort request, not graceful drain. A signal handler can set the stop flag; other-thread control requires a wakeup-fd mechanism on the owner thread. Runtime discovery, registration, capacity clamping, heartbeat/reconnect and drain semantics remain pending.

## Interactive development driver

```sh
make all build/test_job
./build/worker_loop_demo
```

Commands:

```text
run 101 1 64 5 ./build/test_job stream
run 102 1 64 5 ./build/test_job hello
jobs
cancel 101 1
```

Use arbitrary positive IDs/run IDs and program arguments. Paths/arguments with spaces or shell quoting are not supported by this small driver. It is not the final worker command syntax. `runner_start()` success means a child was forked, not an exec/start acknowledgement from the network.

EOF stops taking commands and waits for all jobs/results. `quit` or Ctrl+C aborts active jobs. This EOF convenience in the driver is not the assignment's graceful drain implementation. The driver prints callback output for inspection; production transport should queue it to the scheduler instead. Development stdout may block, so this console is not a slow-client transport implementation.

## Verification coverage

The C groups test staged executable permissions, output-before-result ordering, output/result backpressure, retry-before-cleanup, concurrent jobs, cancellation by execution token, I/O readiness, stale watch tags, nonblocking flags, reentrant-step rejection and abort/reap/cleanup. The console suite covers concurrent submissions, EOF completion, malformed/long commands, PATH execution, cancellation and SIGINT.

All four new C groups and the console suite pass on WSL Ubuntu as the normal user and as root. The complete scheduler/runner/worker-loop regression suite also passes with AddressSanitizer and UndefinedBehaviorSanitizer.

Platform scope: Linux/WSL runtime validation; macOS code paths have not been run here. Shared `protocol.h` and common transport/discovery/logging files are unchanged by this slice. No hash functions are introduced.
