# Worker runner foundation

This is Member 2's local process-execution layer. It runs actual programs, but it is not yet the networked `worker` executable required by the assignment.

## Files to bring into the final team repository

```text
include/model.h          # already shared with Member 1; don't overwrite a newer team version
include/runner.h
src/worker/internal.h
src/worker/runner.c
src/worker/files.c
src/worker/platform.c
```

Merge the runner build rules from the Makefile. The driver and tests are development helpers, not dependencies of the library:

```text
tools/worker_runner.c
tests/test_runner.c
tests/test_runner.py
tests/programs/job.c
```

Current `make all` builds the drivers, and `make test` uses the tests. If leaving those helpers out of the final repo, change those targets to match the team's real executables.

## What the code does

- Accepts job requirements, job ID, execution ID and a NULL-terminated argv.
- Creates `/tmp/osn_job_<ID>` exclusively. An existing folder is an error; it is never deleted or reused by a new start.
- Copies a supplied local path containing `/` into `program` in that directory. A bare PATH command executes by name without copying. This is local staging, not executable transfer or caching.
- Forks a separate process group, enters the job directory, and under root chowns its files and drops supplementary groups/GID/UID to `nobody`. Without root, warns and skips those ownership changes.
- Applies CPU, address-space, process-count and no-core-dump limits. Cores and GPUs remain scheduler accounting values.
- Redirects stdin to `/dev/null` and both output streams to `output.log`. Worker descriptors are closed before exec; `execvp()` receives argv directly without a shell.
- Polls the child, sends output through a caller callback, enforces a wall deadline, measures process usage and reaps it. On macOS, the sampled RSS also enforces RAM because address-space enforcement differs there.
- Cancels with SIGKILL to the process group and reports the known stop reason. Root setup/exec failures use a close-on-exec error pipe so the parent can diagnose them.
- Keeps output files until the caller explicitly removes the finished record. Directory cleanup does not follow job-created symlinks.

This implements the assignment's basic restrictions, not a full filesystem/syscall sandbox. RLIMIT_NPROC counts processes for the dummy user across jobs, not 64 independent slots per job.

## API sequence for the real worker loop

1. `runner_init(&r)` once on a fresh context.
2. After assignment/executable preparation, call `runner_start(&r,id,run_id,&req,args)`.
3. Call `runner_poll(&r,send_output,arg)` roughly every 50–500 ms from the worker loop.
4. Read matching records through `runner_find()`. `state` is the result after reap, but **only `finished` means the child was reaped and all captured output was accepted**.
5. Send the final result to the scheduler, then call `runner_remove()`. Do not delete output before the result/output transport accepts it.
6. On shutdown/disconnection, `runner_free()` kills/reaps remaining direct children and cleans their directories. Shutdown is an abort path; it does not deliver pending output.

The callback returns 0 when it accepts the whole chunk, otherwise nonzero. Rejected chunks are retried with the same bytes; a NULL callback does not discard output. Queueing the bytes internally counts as acceptance only if the caller preserves that queue until delivery. The runner does not itself implement network delivery acknowledgements.

`runner_start()` returning success means the child was forked, not that exec has already succeeded. Setup/exec errors arrive asynchronously through `runner_poll()`. A production started-acknowledgement handshake is still needed in the network adapter; do not interpret successful fork alone as confirmed program execution.

Multiple jobs can run in one context. Calls must be made by the worker's owning event loop; this is not a concurrent-caller/thread-safe interface. One job ID can have only one retained execution at a time, even when run IDs differ. Release the old record before restarting the same ID.

`runner_start()` prepares a local file synchronously. A future network adapter should finish incoming transfers before calling it and avoid scheduling large blocking copies in heartbeat handling. Future cache/checkpoint integration will need a preparation stage; it is not implemented here.

## Try it

```sh
make all build/test_job
./build/worker_runner -- ./build/test_job hello
./build/worker_runner -time 1 -- ./build/test_job sleep 5000
./build/worker_runner -ram 32 -- ./build/test_job memory
./build/worker_runner -- printf path-command-ok
sudo ./build/worker_runner -- ./build/test_job hello
```

Driver options: `-id`, `-run`, `-ram` and `-time`. Job IDs default to the driver's PID to reduce accidental temp-directory collisions. Ctrl+C cancels the local job. The driver intentionally renders captured bytes on its stdout so the developer can inspect them; the library itself forwards bytes only to the supplied callback.

## Verification completed

- WSL Ubuntu, normal user: five runner API test groups and thirteen process-driver scenarios pass.
- WSL Ubuntu, root: the same groups/scenarios pass, including the child UID check for `nobody`.
- `make sanitize`: scheduler regression tests, runner API tests and both driver suites pass under address/undefined-behavior sanitizers.
- Fixture workloads are compiled without sanitizer instrumentation so an intentionally small address-space limit does not prevent the sanitizer shadow mapping. The runner/library/test harness themselves are instrumented.
- macOS implementation is present behind platform guards but has not been compiled or run here.

## Still pending

Final worker CLI, capacity clamping, UDP discovery, registration/reconnect, TCP assignments, heartbeats, local draining, executable caching, checkpoint collection/restore, migration grace handling and production timestamped logging through the shared logger. These must be integrated before Member 2's complete workstream can be called finished.
