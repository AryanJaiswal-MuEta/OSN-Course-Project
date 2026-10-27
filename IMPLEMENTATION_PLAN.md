# Distributed Compute Cluster — Explanation and Implementation Plan

Prepared from all 39 screenshots in `Questions/`, read in filename/time order.

Assignment deadline: **2 December 2026**. Total: **400 marks**. Team: **3 members**.

**Updated 26 October 2026:** Full member ownership is integrated in Section 6. Section 7 contains task estimates and a daily schedule for all three members from **26 October through 30 November**, with feature freeze on **25 November** and final validation completed two days before submission. Budget: **347 planned team person-hours + 42 contingency hours**.

This document separates requirements from recommended implementation choices. The screenshots are the source of requirements; architecture, internal message names, file layout, proposed dates, and unspecified edge-case policies below are recommendations. **Actual status as of 27 October:** Member 1's 26–27 October foundation is implemented; see `docs/progress.md`. The dates below are the original three-person baseline, not evidence of completed work or an unchanged forecast for one person covering multiple roles.

## 1. What you are building

Build a small distributed batch-computing system in **C**, using ordinary TCP sockets for communication and UDP broadcast for scheduler discovery. Users submit programs; the system chooses a machine, transfers the executable if necessary, runs it with limits, and returns status and output. Multiple users and workers operate concurrently.

There are three executables:

| Program | Responsibility |
|---|---|
| `scheduler` | Central job registry and queue, worker registry, scheduling, user accounting, output retention, checkpoints, recurring templates, and client events |
| `worker` | Advertise capacity, receive assignments, execute child processes, enforce limits, capture output, send heartbeats and telemetry, maintain executable cache |
| `client` | Interactive login and commands, asynchronous events, status, output following, cancellation and migration |

The client and scheduler are on the same machine or share a filesystem. Submission sends the scheduler an executable path; the scheduler reads the file and transfers it to a worker. A bare command available through PATH can run by name without file transfer. This does not require uploading binary bytes from the client.

Each job runs on **one worker**. Requesting multiple cores changes scheduler accounting; the platform does not split an arbitrary program across machines or automatically make it parallel. GPUs are also accounting counters. Time and memory require actual enforcement.

This is a POSIX/Linux/macOS systems project. Windows is the current workspace host, but native Windows is not the specified runtime. Plan for Linux and macOS builds and real network testing. A Linux development environment can help locally; it does not replace macOS and multi-machine discovery validation. Docker is not part of the required deployment.

## 2. Complete requirements map

### 2.1 Startup and discovery — 25 marks

Match these command lines exactly:

```text
./scheduler <port> <scheduling_algorithm> [-discovery-port <port>]
./worker [<scheduler_ip> <scheduler_port>] -cores <num> -ram <MB> [-gpu <num>] [-discovery-port <port>]
./client [<scheduler_ip> <scheduler_port>] [-user <username>] [-discovery-port <port>]
```

- Algorithm is `FIFO` or `PRIORITY`; discovery port defaults to `9999`.
- Worker and client attempt UDP broadcast discovery first, even when a fallback address was supplied. Each attempt waits up to 2 seconds; use three attempts before fallback. Discover again after disconnection. Without discovery or fallback, report an error and exit.
- Scheduler replies with its TCP endpoint. Use the required reuse socket options and test immediate restarts.
- Clamp advertised worker cores and RAM to physical values, with a warning; GPU is a counter and is not clamped.
- Scheduler assigns a unique integer worker ID and returns it in registration. Subsequent messages/logs use it. Before registration, log without an ID.
- On connection loss, a worker kills its running jobs, then reconnects and registers as a new worker with a new ID.
- Test broadcast on a personal hotspot; use distinct discovery ports for separate clusters on the same LAN.

### 2.2 Client, login, commands and events — 25 marks

- Use `-user` if provided; otherwise prompt. Valid usernames have 1–31 characters: letters, digits, `_`, `-`, `.`. Invalid interactive input must be requested again.
- Username is the client's first application message and is sent again automatically after reconnect. No password.
- Keep both connections when clients use the same username. Recommended: treat username as owner identity and maintain a set of sessions per username.
- Every job records its owner. Scheduler logs of client actions identify the acting user.
- Handle socket input while the client is idle at its prompt. `poll()` over stdin/socket is a suitable design.

| Command | Required behavior |
|---|---|
| `nodes` | Worker ID/state; free and total cores/RAM/GPUs; running-job count |
| `submit <binary> <flags>` | Validate submission and return integer job ID, or explicit error |
| `status <job_id>` | State, owner, assignment; later telemetry, diagnosis and checkpoint information |
| `logs <job_id>` | Retained output so far, then return to prompt |
| `logs <job_id> -f` | Retained output plus live output; Ctrl+C leaves follow mode without exiting client |
| `cancel <job_id>` | Cancel WAITING, QUEUED or RUNNING job appropriately |
| `migrate <job_id>` | Move a RUNNING job through cooperative checkpoint restart |
| `crons` | Template ID, interval, runs, user and command |
| `cancel-cron <template_id>` | Disable future instances; existing jobs remain |

Submission flags:

| Flag | Requirement |
|---|---|
| `-cores <num>` | Required; scheduler core accounting |
| `-ram <MB>` | Required; accounting and memory limit |
| `-time <secs>` | Required; wall-clock deadline and CPU rlimit |
| `-prio <1-3>` | Required; 1 highest, 3 lowest |
| `-gpu <num>` | Optional, default 0 |
| `-depends-on <list>` | Optional comma-separated job IDs and group references such as `1,g3` |
| `-group <id>` | Optional positive group ID |
| `-preemptible` | Optional permission for scheduler to kill/requeue the job |
| `-every <secs>` | Optional recurring template; incompatible with dependencies |

Push `[Event]` messages for job state changes, failure reasons and affected worker warnings. Only the owner's sessions receive job events. Logs are a separate stream. Display events even while idle. The brief permits small differences in status/nodes formatting if the required information is present.

### 2.3 Scheduler — 105 marks

**Concurrency (30):** choose either one event loop or thread-per-connection with protected shared state. Status queries, heartbeats, scheduling and clients must not block one another. Queue capacity is 1024; reject fresh submissions with `QUEUE_FULL` without waiting or losing jobs. Internal requeues must never be discarded, even when admission is full. Schedule on relevant events and at least once every second.

**Ordering (25):** FIFO is strict submission order with head-of-line blocking: do not skip an older job because a smaller one fits. PRIORITY orders by effective priority, then submission age; if a job cannot fit, try the next. Every 10 seconds spent QUEUED improves priority by one, to a minimum of 1. A worker must have enough free cores, RAM and GPUs.

**Fault tolerance (40):** heartbeats every 2 seconds carry CPU load, active-job count and per-job telemetry. One missed heartbeat makes a worker SUSPECTED; three consecutive misses, around 6 seconds, make it DEAD. SUSPECTED workers receive no new jobs and recover to ALIVE on heartbeat. Running jobs on a DEAD worker go through LOST and requeue. Default maximum is 3 total failure attempts, implemented as a code constant; exhausted jobs become FAILED_PERMANENT. Preemption and migration do not consume this budget.

**Load awareness (10):** before caching is added, choose the qualifying worker with lower CPU load, then fewer jobs. The suggested load measurement is normalized `getloadavg()`, capped at 100%. Once caching is added, idle status and cache presence come before these comparisons.

### 2.4 Worker execution — 70 marks

**Isolation/limits (30):** use `fork()` and `execvp()`; never `system()`. Before exec, perform the required preparation in this order:

1. Create and enter `/tmp/osn_job_<JobID>`; copy transferred executable into it; under root, chown the job directory tree to the dummy user.
2. Drop privileges with `setgid()` followed by `setuid()`, looking up a dummy user such as `nobody`. Check return values and refuse to run on failure. When not root, warn clearly and skip chown/privilege dropping; continue for development. Evaluation uses sudo.
3. Apply `RLIMIT_CPU` using the requested seconds.
4. Apply `RLIMIT_AS` using RAM converted from MB to bytes.
5. Apply a low `RLIMIT_NPROC`, e.g. 64, recognizing that it is a per-user budget shared by jobs using the same dummy user.

Also enforce a real wall-clock deadline with SIGKILL; a sleeping job must time out. Report CPU-limit termination as the requested timeout diagnosis as well. On macOS, the brief says `RLIMIT_AS` is ignored, so periodically sample resident memory and kill jobs exceeding RAM. Cores/GPUs remain bookkeeping only. This is the assignment's limited isolation, not a full sandbox.

**Output (10):** redirect both stdout and stderr to the job's `output.log`. No child output should leak into the worker terminal. Read newly appended bytes roughly every 500 ms, send them to the scheduler, and flush the remainder before the final report. Clean up the job directory only after output has been sent.

**Cancellation (10):** remove queued jobs, cancel dependency-blocked jobs, and SIGKILL/reap running jobs. Release all reserved resources, including GPUs. No zombies.

**Telemetry (10):** live RAM, page faults and CPU time. Linux uses `/proc/<pid>/stat` and `statm`; macOS uses `proc_pidinfo()` with `<libproc.h>`. `getrusage(RUSAGE_CHILDREN)` is not a live telemetry source. Finished status shows peak RAM.

**Diagnosis (10):** inspect exit status with `WIFEXITED`, `WIFSIGNALED` and `WTERMSIG`. Explain exit codes, including 127, and common signals. Prefer known platform action reasons for timeout, cancellation, preemption and migration over generic signal numbers.

### 2.5 Output streaming and system logging — 30 marks

- Scheduler retains at least the latest 64 KB of each job's output in memory.
- Worker output updates reach subscribed clients during execution. Follow ends with a footer and restored prompt when the job finishes.
- Use one shared `log_line()` function for scheduler/worker state changes and decisions. Format timestamps `[HH:MM:SS]` in 24-hour time with uppercase component tags.
- Log every job/worker transition, discovery outcome, assignment decision and reason. Include the acting username for client actions.

### 2.6 Dependencies and groups — 20 marks

- A job with dependencies is WAITING until all dependencies COMPLETED; WAITING jobs do not participate in queue sorting.
- A FAILED, FAILED_PERMANENT or CANCELLED prerequisite cancels dependents, recursively.
- LOST is transient and does not itself cancel dependents.
- Resolve `g<id>` at submit time into the IDs currently in that group. Later group additions do not change existing dependencies.
- Reject unknown job/group references at submission. Handle already-terminal prerequisites immediately.

### 2.7 Graceful draining — 10 marks

Only local worker stdin `drain` or Ctrl+C initiates draining; there is intentionally no client drain command. Inform scheduler, exclude the worker from new work, keep sending heartbeats, complete active jobs, flush output, then disconnect and exit. Normal drain completion is not a failure and does not trigger rescue.

### 2.8 Preemption — 20 marks

When a priority-1 job has no fitting worker, look for an ALIVE worker where killing enough preemptible jobs would free every required resource. Victims: lowest priority first (largest number), then most recently submitted among equals. Kill just enough jobs, then stop selecting victims. Requeue victims without consuming failure attempts.

Hold victims out of scheduling until the priority-1 beneficiary starts; otherwise they could take resources back. Release holds if the beneficiary is cancelled or fails before starting. SIGKILL gives victims no final checkpoint opportunity; use the latest previously saved checkpoint.

### 2.9 Fair-share quota — 15 marks

For each username, count cores held by RUNNING jobs. Cluster total includes all workers except DEAD, including SUSPECTED and DRAINING. If a user holds **more than 50%** and another user who is not over quota has a QUEUED job, skip the first user's next job. Exactly 50% is allowed. This can override priority and FIFO order. If there is no eligible competing user, do not restrict that user. Log skips once per relevant pair per pass, avoiding noisy duplicates.

This is a conditional scheduling rule, not a hard 50% admission ceiling and not a test of projected usage after starting the candidate job.

### 2.10 Recurring jobs — 15 marks

`-every` creates a template, with a separate ID sequence starting at 1. Spawn the first job immediately and subsequent copies at the interval, even if earlier copies are still running. A roughly one-second timer handles due templates. Every copy gets a new job ID and inherits user/resources/allowed flags. Reject combining `-every` and `-depends-on`. `cancel-cron` stops future copies only and errors for unknown/already-cancelled templates.

### 2.11 Executable cache — 35 marks

- Identify content with SHA-256 or SHA-1, not just filename; cache files under filename/hash-based names in a worker cache directory.
- Scheduler maintains per-worker hash inventories. Transfer full bytes once, then use a hash reference on cache hits.
- Copy the cached program into each job's own directory before running it.
- Final worker comparison: **idle first → cached binary → lowest CPU load → fewest running jobs**. An idle uncached worker beats a busy cached one.
- Default cache budget 512 MB, configurable via `OSN_CACHE_SIZE_MB`; separate workers can use `OSN_CACHE_DIR`.
- Evict least recently used eligible files. Usage is updated when a job starts, not only on download. Never evict the newly stored file or a file currently used by a running job.
- Notify scheduler after eviction. Scan and report cache contents after restart.
- If a referenced cached file is missing, request retransmission, remove stale scheduler knowledge, and continue without consuming an attempt.
- If scheduler cannot read the program, the brief allows execution by path without transfer/cache. Bare PATH commands are the principal example; relative non-transferred paths may not exist in the job directory, so document errors clearly.

### 2.12 Cooperative checkpoints and migration — 30 marks

This is application-assisted restart, not an external snapshot of arbitrary process memory.

- Give each job `OSN_CHECKPOINT_DIR`, pointing inside its working directory. Make it writable by the job user.
- Participating programs write a small `state` file, using temporary-file-plus-rename for atomic publication. On startup, read it to resume; absence means start over.
- Roughly every 500 ms, detect checkpoint changes using `stat()` timestamps; send new state to scheduler, which keeps the latest bytes per job in memory.
- Restore saved checkpoint on every restart: migration, preemption or worker failure.
- `migrate` requires an existing RUNNING job. Send SIGTERM, allow 2 seconds, then SIGKILL if it remains alive. Capture any final checkpoint, reap, and requeue without using a failure attempt.
- Prefer a different qualifying worker after migration; use the source if no other worker qualifies.
- After selecting a worker, send saved checkpoint **before** binary/cache reference. Worker restores it before exec.
- Status includes checkpoint number, byte size and age.

## 3. Recommended architecture

### 3.1 Pick an event loop

Use one `poll()`-based loop in the scheduler, owning all mutable cluster state. This is one of the brief's permitted models and works across the two required operating systems. An event-loop-owned queue does not need cross-thread access; if helper threads are later introduced, route their results through the loop or add the required protection.

Each connection owns incremental receive parsing and a transmit queue. Never wait for a whole message, a slow client, a binary download, or child termination inside the loop. Use nonblocking sockets and bounded per-iteration work. Large transfers use chunks, so heartbeats and queries continue between chunks. Use monotonic time for deadlines/aging and wall-clock time only for display timestamps.

The worker can also use `poll()` plus timed tasks: scheduler socket, local stdin, heartbeat timer, output/checkpoint scans, telemetry, deadlines and nonblocking `waitpid()`. The client polls stdin/socket and uses a signal flag to stop following. Keep signal handlers minimal and perform normal work in the main loop.

### 3.2 Suggested repository layout

```text
Makefile
README.md
docs/
  protocol.md
  state-machines.md
  testing.md
include/
  protocol.h
  model.h
  net.h
  discovery.h
  logger.h
src/
  common/       # framing, socket buffers, discovery, time, logging, hashing
  scheduler/    # registries, admission, policies, lifecycle, dependencies,
                # user accounting, cron, subscriptions, dispatch/recovery
  worker/       # registration, runner, limits, output, telemetry,
                # cache, checkpoints, local admin
  client/       # arguments, login, parser, prompt/events, log follow
tests/
  unit/
  integration/
  programs/     # controlled sample workloads
```

Keep three small `main.c` entry points. Share protocol definitions, state enums, parsing helpers and logging; do not duplicate them across executables. Separate policy decisions from socket handling and process execution.

### 3.3 Define the full data model at the start

| Record | Suggested fields |
|---|---|
| Job | ID, owner, program/path/hash, resource request, base priority, submission sequence, queued-time accounting, state, active execution token, worker ID, retry accounting, dependency IDs, group, preemptible flag, hold beneficiary, migration source, output tail, telemetry/peak, diagnosis, checkpoint metadata/bytes |
| Worker | ID/session generation, connection, state, total/reserved resources, heartbeat timestamp, CPU load, running/reserved job IDs, cache inventory |
| Client session | Connection, username, request IDs, output subscription and offset |
| User | Username, active sessions, running cores |
| Cron template | Independent ID, owner, immutable job specification, interval, next due time, run count, active flag |
| Cache entry | Hash, filename/path, byte size, last-start timestamp, active-use count |

The public job states are WAITING, QUEUED, RUNNING, COMPLETED, FAILED, CANCELLED, LOST and FAILED_PERMANENT. Worker states are ALIVE, SUSPECTED, DEAD and DRAINING. Use internal dispatch/cancel/migrate flags or substates where needed without inventing incompatible public states.

Separate a unique **execution token** (new for every dispatch) from **failure retry accounting**. A migrated/preempted dispatch needs a fresh token to reject stale reports but must not consume the three-attempt failure budget.

### 3.4 Protocol design

Use an explicitly serialized, versioned frame header, e.g. magic, version, type, request ID and payload length in network byte order. Do not send raw C structs: padding, size and byte order are not a portable wire format. Bound lengths and validate numeric conversions.

Recommended message families:

| Family | Messages/concepts |
|---|---|
| Discovery | Request/reply with protocol identity and TCP endpoint |
| Session | Client login; worker registration/ack; disconnect |
| Client RPC | Nodes, submit, status, logs, subscribe/unsubscribe, cancel, migrate, cron list/cancel, response/error |
| Execution | Assignment metadata, checkpoint chunks, binary chunks/reference, ready/start, started acknowledgement, final result |
| Worker updates | Heartbeat, output chunk, checkpoint update, drain request/completion |
| Cache | Inventory, hit/miss, eviction/update, transfer acknowledgement |
| Async | Owner event, live log data, follow-end |

Every execution-specific message includes job ID and execution token. Chunks carry transfer IDs and offsets. Output chunks carry offsets within an execution so retries/duplicate delivery do not corrupt a log. Keep retained output across restarts and add attempt/restart markers as a recommended user-facing convention.

Transfer through temporary files, verify expected size/hash, then rename into cache. A start barrier ensures a job does not execute before checkpoint and executable are ready. Reserve scheduler resources while dispatching, and roll back once if dispatch fails; expose RUNNING after the worker confirms it started. This avoids assigning the same free resources to two concurrent transfers.

Support short reads/writes, multiple frames per read, fragmented headers, EOF halfway through transfer, unknown types, malformed lengths and broken pipes. Keep slow follower queues bounded; disconnect or require resubscription with a clear error rather than blocking the scheduler. File reading/hashing must also avoid long pauses in the central loop: process chunks incrementally or return completed work from a narrowly scoped helper.

### 3.5 Centralize state changes and accounting

Implement a lifecycle function that validates transitions, updates resource/user accounting, logs, sends owner events, propagates dependency outcomes and triggers scheduling. A resource reservation is acquired/released exactly once. Keep requested reservations separate from observed CPU/RAM telemetry.

Use nonblocking action sequences for cancellation, preemption and migration. Do not reassign resources until the worker confirms the old execution stopped, or its session has been declared dead. Ignore old execution tokens after recovery. Worker disconnect kills local children before re-registration.

Recommended job process groups allow cleanup of descendants as well as the immediate child. This supplements the specified job-PID kill/reap behavior and avoids orphaned workloads. Close inherited worker sockets in children and prevent child stdin from consuming worker admin commands. Under root, also consider clearing supplementary groups before dropping GID/UID.

## 4. Scheduling algorithm: implement this order exactly

```text
schedule(now):
  candidates = QUEUED jobs only
  refresh effective priorities from QUEUED waiting time (PRIORITY only)
  order by submission sequence (FIFO)
    or effective priority, then submission sequence (PRIORITY)

  for job in candidates:
    if held for a preemption beneficiary that has not started:
      continue
    if the user quota rule says skip:
      log the skip with duplicate suppression
      continue

    matches = ALIVE workers fitting all free cores, RAM and GPUs
    if job was migrated and another match exists:
      remove the migration source from matches

    if matches is not empty:
      choose by idle, then cache hit, then CPU load, then job count
      reserve resources and begin dispatch
      send saved checkpoint before binary/reference; start after preparation
      continue

    if job qualifies as priority 1:
      try a feasible preemption plan on one eligible worker
      hold victims for this beneficiary; await stop acknowledgements

    if FIFO:
      break
    else:
      continue
```

Quota and preempted-job holds are deliberate skips before FIFO's no-fit stopping rule. Do not accidentally stop at quota-skipped jobs. Use a deterministic final worker-ID tie-break if every prescribed comparison is equal.

Trigger this on submission, completion, worker registration/availability, heartbeat and dependency release, as well as a periodic timer no slower than once per second. Prevent recursive scheduling from state-change callbacks; mark scheduling needed and run a controlled pass.

## 5. Implementation phases and acceptance gates

### Phase 0 — Foundation and interface agreement

1. Agree wire format, data ownership, state transitions and accounting invariants.
2. Create modular tree, Makefile and shared headers; enable useful compiler warnings.
3. Define timer units, byte units, error codes and common logger.
4. Implement framing and loopback tests before independently building components.
5. Decide edge-case policies in section 8 and record them in `docs/protocol.md`.

**Gate:** all three binaries build; two endpoints exchange fragmented/coalesced messages correctly; invalid frames are rejected cleanly.

### Milestone 1 — Core pipeline (Sections 2, 3, 4A–B, 5A–B, 7)

1. Startup argument validation; discovery server/client; fallback and reconnect structure.
2. Worker registration/capacity clamps and username login.
3. Registry, bounded admission and nodes/status responses.
4. FIFO and PRIORITY/aging as separately testable policy functions.
5. Basic chunked executable transfer and PATH execution. Do not postpone initial transfer until caching.
6. Worker fork/exec, working directory, privilege drop, limits and wall-clock timer.
7. Output capture/incremental forwarding, final result and resource release.
8. Owner events and timestamped logs.

**Gate:** submit a transferred test program from a client, run it on a worker, see output/status, finish, and recover capacity. Verify FIFO blocks a fitting smaller job behind a non-fitting older one; PRIORITY can skip. A second client stays responsive during job execution/transfer. Verify non-root warning and actual root privilege-drop behavior.

### Milestone 2 — Resilience and telemetry (Sections 4C–D, 5C–E, 6, 9)

1. Heartbeat timers and ALIVE/SUSPECTED/DEAD lifecycle.
2. Worker-loss rescue, three-total-attempt budget and stale-message rejection.
3. Live process telemetry and load-aware worker choice.
4. Cancel acknowledgement/reap flow; reasoned crash diagnosis.
5. Output tail, followers, follow Ctrl+C and terminal footer.
6. Local drain/SIGINT handling and reconnect cleanup.

**Gate:** kill/partition a worker, observe suspicion/death and retry on another; reach FAILED_PERMANENT after the allowed attempts. Cancel queued/running jobs, follow live output, drain while running, and verify no double release, zombie, or accidental rescheduling on a draining worker.

### Milestone 3 — Advanced scheduling (Sections 8, 10, 11, 12, 13)

1. Dependencies, group snapshots, immediate terminal-prerequisite handling and recursive cancellation.
2. Quota accounting/checks in the specified place in the pass.
3. Preemption planning, victim ordering, holds and beneficiary failure cleanup.
4. Independent cron-template registry and timer injection.
5. Content hashing, cache inventory/hit/miss protocol, worker ranking and pinned LRU eviction.

**Gate:** dependency chain and group tests pass; quota overrides FIFO in the given 4-core example; priority-1 job preempts only enough permitted victims and they cannot steal capacity back; cron copies overlap and cancellation leaves existing copies; all cache tests below pass.

### Milestone 4 — Checkpoints and migration (Section 14)

1. Add a cooperative sample workload that atomically saves progress and handles SIGTERM.
2. Set up writable checkpoint directory/environment for every job.
3. Detect/transfer checkpoint changes, retain latest and show metadata.
4. Restore before execution on every restart path.
5. Implement migration's nonblocking SIGTERM/grace/SIGKILL sequence, source avoidance and requeue.

**Gate:** migrate midway through a long workload; progress continues on a different worker. Kill its worker and verify recovery from the latest scheduler-held checkpoint. Preempt it and verify the same restore path. A program without checkpoints simply restarts.

### Final integration gate

Clean Linux/macOS builds; sudo and non-sudo runs; multi-machine hotspot discovery; simultaneous clients/workers; combined features; repeated start/stop; README commands and reproducible demo. Keep time for failures that only appear when features interact.

## 6. Full member split and coordination

**Updated 26 October 2026.** Member 1 = A, Member 2 = B, Member 3 = C from the original split. Replace these placeholders with your names. This section and Section 7 are the authoritative integrated work plan; TEAM_WORK_SPLIT.md remains the companion ownership reference.

The aim is roughly equal engineering effort, not dividing marks into thirds. Several marked features span all three programs. Every member implements tests, fixes integration defects, documents their modules and authors their own commits. The recommended split assumes all three have basic C/POSIX familiarity.

### Ownership at a glance

| Member | Owns | Main paths |
|---|---|---|
| 1 | Scheduler core, job lifecycle, scheduling policies and resource accounting | `src/scheduler/main.c`, `loop.c`, `registry.c`, `admission.c`, `lifecycle.c`, `policy.c`, `recovery.c`, `dependencies.c`, `quotas.c`, `preemption.c`, `dispatch.c` |
| 2 | Worker, process execution, OS limits, telemetry, local cache and checkpoint handling | `src/worker/` and `tests/programs/` |
| 3 | Common transport/discovery, client, and scheduler-facing service modules | `src/common/`, `src/client/`, `src/scheduler/client_service.c`, `output_service.c`, `artifact_service.c`, `cron.c`, plus initial build configuration |

Suggested filenames are design choices, not required names from the assignment. Member 3 owns the initial Makefile; each member updates build entries for their own new files.

### Member 1 — Scheduler core and scheduling policy

#### Milestone 1: core pipeline

- Scheduler command-line validation and TCP listening/event loop, using Member 3's transport and discovery helpers.
- Worker registration, ID allocation, worker table and job registry.
- Bounded submission admission and explicit QUEUE_FULL responses through the service interface.
- All public job/worker state transitions and common lifecycle functions.
- Requested resource reservations and release, including GPUs and dispatch-in-progress reservations.
- FIFO ordering/head-of-line blocking; PRIORITY ordering and aging.
- Resource matching and assignment orchestration, including worker start acknowledgement.
- Read-only query APIs for Member 3's nodes/status handlers.
- Scheduler-specific transition/assignment logs using the shared logger.

#### Milestone 2: resilience

- Scheduler-side heartbeat deadlines, SUSPECTED recovery and DEAD transitions.
- LOST/requeue/FAILED_PERMANENT behavior and three-total-attempt failure accounting.
- Execution tokens and rejection of stale worker reports.
- Load-aware worker choice from heartbeat data.
- Scheduler cancellation lifecycle, resource release and drain-state handling.
- Guaranteed preservation of internally requeued jobs under queue saturation.

#### Milestone 3: advanced policy

- Dependency graph, group snapshot resolution, WAITING release and cascading cancellation.
- Per-user running-core accounting and conditional fair-share quota rule.
- Preemption feasibility, victim selection/order, holds and beneficiary cleanup.
- Final worker ranking: idle, cache hit, CPU load, running-job count.
- Integrate cron-created jobs through the same admission and lifecycle APIs.

#### Milestone 4: migration scheduling

- Validate RUNNING state, initiate migration and track migration intent.
- Requeue after stop confirmation without consuming failure budget.
- Prefer another qualifying worker, and reuse the normal dispatch path for restoration.
- Ensure preemption, failure recovery and migration all share correct restart accounting.

#### Tests and review responsibility

Own policy/lifecycle tests: FIFO blocking, priority aging, quota ordering, dependencies, resource conservation, retries and preemption holds. Review Member 3's scheduler service integrations for state ownership and accounting correctness.

### Member 2 — Worker execution and OS behavior

#### Milestone 1: core pipeline

- Worker flags, capacity measurement/clamping and registration client.
- Worker event loop and assignment handling using Member 3's transport.
- Receive/stage executable content; prepare the job directory and executable permissions.
- Fork/execvp, child descriptor setup and process cleanup.
- Required chown and setgid/setuid behavior; checked failures and non-root warning.
- CPU/address-space/process limits and independent wall-clock deadlines.
- Linux/macOS differences, including macOS resident-memory enforcement.
- stdout/stderr redirection, incremental output reads and final output flush.
- Report start/exit results and clean up temporary directories at the correct time.

#### Milestone 2: resilience

- Heartbeat production every two seconds with load and running-job telemetry.
- Live RAM, CPU time, page faults and final peak memory.
- SIGKILL/reap cancellation and readable exit/signal diagnosis.
- Local drain command and SIGINT behavior; complete output/results before graceful exit.
- Kill local executions on scheduler connection loss, then rediscover/re-register.
- Keep intentional cancellation/preemption/migration reasons distinct from crashes.

#### Milestone 3: worker cache and preemption execution

- Cache directory and OSN_CACHE_DIR support.
- Content-verified cache storage, startup scan and inventory reporting.
- Cache hit/miss handling and executable copy into the per-job directory.
- OSN_CACHE_SIZE_MB, LRU timestamps updated on job start, active-use pinning and eviction notifications.
- Handle oversized/pinned-cache cases according to the team's documented policy.
- Execute preemption stop requests and acknowledge only after reaping.

#### Milestone 4: cooperative checkpoints

- Writable checkpoint directory and OSN_CHECKPOINT_DIR environment setup.
- Roughly 500 ms change detection and checkpoint updates to scheduler.
- Restore checkpoint state before exec.
- Migration SIGTERM, nonblocking two-second grace and SIGKILL if necessary.
- Capture final output/checkpoint even if the process exits during grace.
- Build the cooperative progress-counter sample workload.

#### Tests and review responsibility

Own controlled workload programs and worker integration tests: privileges, cwd, limits, CPU/sleep timeouts, crash signals, output, cancellation, child cleanup, cache eviction and checkpoint resumption. Review Member 1's stop/retry interactions against actual worker behavior.

### Member 3 — Networking, client and scheduler services

This member also owns concrete scheduler modules so the workload includes more than the interactive client.

#### Milestone 1: shared foundation and client

- Shared framing/serialization, message validation and protocol documentation.
- Nonblocking send/receive buffers and chunked transfer support.
- UDP discovery responder/request helpers, retries, fallback support and reuse options.
- Shared timestamps/log_line implementation and timing helpers.
- Initial Makefile and framing/discovery tests.
- Client flags, username validation/login, reconnect login and command parser.
- Submit/nodes/status/logs client commands and asynchronous owner-event rendering.
- Scheduler client-session and command-response handlers in client_service.c.
- Scheduler-side executable reading/basic transfer coordination in artifact_service.c.
- Scheduler-side output retention/query service in output_service.c.

#### Milestone 2: streaming and sessions

- At least 64 KB retained output per job, follower subscriptions and final footer.
- Client follow mode and Ctrl+C unsubscribe without exiting.
- Owner-to-session routing, including multiple same-username sessions.
- Client cancellation request handling and display of telemetry/diagnoses.
- Reconnect handling, explicit disconnection errors and no accidental submit replay.
- Bounded slow-client output queues so followers cannot freeze the cluster.
- Multi-client/multi-worker integration harness.

#### Milestone 3: cron and executable transfer services

- Scheduler cron template storage, independent IDs, due timer and immediate first injection.
- crons/cancel-cron handlers and -every validation/inherited specification.
- Integrate injected jobs through Member 1's admission API; implement the agreed full-queue policy.
- Scheduler-side content hashing and per-worker cache inventory.
- Full transfer versus cache reference, stale-cache miss recovery and inventory updates.
- All advanced client flags and response/event formatting.

#### Milestone 4: checkpoint transport and client integration

- Scheduler storage of the latest checkpoint bytes, sequence, size and receipt time.
- Transfer saved checkpoint before binary/reference during dispatch.
- Client migrate command and checkpoint metadata in status.
- End-to-end migration/recovery/preemption test orchestration.
- Assemble README build/run/demo instructions, with technical content supplied by Members 1 and 2.

#### Tests and review responsibility

Own framing, partial transfer, discovery/fallback, client event/follow, cache protocol and cron tests. Review Member 2's socket/file-transfer handling and ensure worker activity cannot block networking.

### Interfaces to agree before independent implementation

Spend the first joint session writing headers and one-page protocol/state documentation. These are conceptual interfaces; choose final C signatures together.

| Boundary | Contract |
|---|---|
| Common transport → components | Feed received bytes; deliver complete validated messages; queue frames; expose pending-write readiness; never block waiting for a full message |
| Client service → scheduler core | Validate/submit immutable job specification with owner; query snapshots; request cancel/migrate; receive typed success/error result |
| Scheduler core → event/output services | Publish immutable state-change data; service routes owner events/subscriptions without changing job state itself |
| Scheduler core → artifact service | Prepare dispatch for job/execution token; send checkpoint before executable/reference; return preparation success/failure |
| Scheduler → worker | Job/execution token, resource limits, program identity, checkpoint and executable/reference; separate ready/start protocol |
| Worker → scheduler | Started/stopped/final results, output offsets, heartbeat/telemetry, cache changes and checkpoint updates tagged to the execution |
| Cron → scheduler core | Request creation through ordinary admission; do not directly modify the queue or resource accounting |

Member 1 owns shared domain state definitions; Member 3 owns wire/message definitions. Both review changes to the boundary. Member 2 reviews execution and telemetry fields. Do not maintain conflicting copies of structs.

### Collaboration rules

1. Each module has one primary editor. Cross-module changes go through the owner or a small reviewed patch.
2. Member 1 owns the scheduler loop and calls Member 3's services through APIs. Member 3 does not independently mutate job states or resource counters.
3. Decide queue/recovery capacity, cron overflow, aging after requeue, priority basis and oversized-cache behavior together; record decisions.
4. Integrate a minimal end-to-end slice early. Do not wait for all components to be complete.
5. Each member writes relevant tests and documentation; Member 3 coordinates integration rather than becoming the sole tester.
6. Commit your own work with descriptive messages. Review in a cycle: 1 reviews 3, 3 reviews 2, 2 reviews 1.
7. All three participate in Linux/macOS, sudo and real hotspot testing. Assign actual machines based on availability.


## 7. Realistic task estimates and daily timeline

### 7.1 Dates, availability and what the estimates mean

- **Start:** Monday, 26 October 2026.
- **Internal completion:** Monday, 30 November 2026, two calendar days before the official deadline.
- **Official deadline:** Wednesday, 2 December 2026. The screenshots do not specify a submission time; verify it on the course portal.
- **1 December:** emergency-only reserve after planned completion; no planned development, testing or documentation depends on this day. Submit on 30 November if the portal permits, or retain a verified final package for submission before the official cutoff.
- There are **36 calendar days** in the working schedule. Feature development finishes on **25 November**; 26–30 November is a separate validation/release period.
- Budget around **3 focused hours per member on most weekdays and 4 on most weekend days**, plus flexible contingency. Two explicitly heavier sessions are Member 3's 27 October and Member 2's 2 November, both 4 hours.
- Planned task effort is **115 / 116 / 116 hours** for Members 1 / 2 / 3, totaling **347 person-hours**. These totals already include implementation, normal local tests, code review, planned integration and documentation.
- Separately reserve **14 hours per member**, totaling **42 team person-hours**, for unplanned rework. Maximum allocated effort is **129 / 130 / 130 hours**, or **389 team person-hours** through 30 November.
- This is approximately **25–26 hours per member per seven days**, including contingency. It assumes coursework can accommodate a sustained part-time commitment. If availability is closer to 1 hour/day, the full scope does not fit this schedule without substantially changing staffing, availability or expectations.
- These are planning estimates, not measured durations or guaranteed completion times. If sockets, signals or POSIX process control must be learned from scratch, the learning effort is additional; review capacity on 28 October.

A 1-hour joint session costs **1 hour for each attendee**, hence 3 team person-hours with all three present. Joint design, review and demo time is included in each attendee's daily allocation. Parallel work is not three times the calendar duration: a day's 3 hours each is about 3 hours elapsed when concurrent, but 9 person-hours.

Use a normal 3-hour session as roughly 2 hours of implementation, 40 minutes of local testing/review and 20 minutes of coordination/commit work. On the milestone days, spend more of that allocation testing and demonstrating. Exact clock times should fit class timetables; these are effort budgets, not assumptions that everyone is free at a particular hour.

### 7.2 Milestone budgets and acceptance dates

| Phase | Working dates (inclusive) | Member 1 planned h | Member 2 planned h | Member 3 planned h | Team planned h | Extra contingency per member | Acceptance condition |
|---|---|---:|---:|---:|---:|---:|---|
| Foundation | 26–28 Oct | 9 | 9 | 10 | 28 | 2 h | Agreed headers/state/protocol; all three builds; framed loopback exchange; local worker runner |
| Milestone 1: core | 29 Oct–6 Nov | 29 | 30 | 29 | 88 | 3 h | Discover → login → submit transferred executable → run with limits → output/result; FIFO/PRIORITY correct |
| Milestone 2: resilience | 7–13 Nov | 23 | 23 | 23 | 69 | 3 h | Failure rescue/retry exhaustion, telemetry, cancellation, drain and live logs work together |
| Milestone 3: advanced | 14–21 Nov | 27 | 27 | 27 | 81 | 3 h | Dependencies, quota, preemption, cron and cache pass separate and combined tests |
| Milestone 4: checkpoints | 22–25 Nov | 13 | 13 | 13 | 39 | 1 h | Migrate, preempt and kill worker; checkpointed workload resumes and accounting stays correct |
| Final validation/release | 26–30 Nov | 14 | 14 | 14 | 42 | 2 h | Clean-checkout Linux/macOS/sudo/hotspot tests; complete documentation; reproducible final candidate |
| **Total** | **26 Oct–30 Nov** | **115** | **116** | **116** | **347** | **14 h each** | **Internal completion on 30 Nov** |

Contingency is additional capacity for the named phase, not a second count of scheduled testing. Use it only when required. Unused contingency remains free and can roll forward; it is not an invitation to add features.

The resilience/advanced milestone dates replace the earlier rough calendar. The core gate is 6 November, resilience is 13 November, advanced is 21 November and feature freeze is 25 November.

### 7.3 How much work the sections are likely to take

These are central **team person-hour budgets** for related work packages, including normal tests/fixes. Each row is an allocation of the phase budget above, not extra hours added to it. Shared foundations and final regression are separate rows to avoid hiding their cost inside individual features.

| Work package / assignment coverage | Main owners | Team budget | Why it needs this time |
|---|---|---:|---|
| Shared design, build, framing, buffers, logging skeleton and local runner (foundation) | All; Member 3 leads transport | 28 h | Interface agreement, serialization/partial-I/O checks and independent component skeletons |
| Startup, registration, discovery and fallback (Section 2) | 3 transport; 1 scheduler; 2 worker | 18 h | Broadcast retries, capacity/ID handling, endpoint setup and first network test |
| Basic client, session service, output query and executable-transfer wiring (Section 3/core parts of 6) | 3; 1/2 integrate | 17 h | Parsing/login/events and the initial complete request/transfer path |
| Scheduler concurrency, admission, policies, aging and dispatch (4A–B) | 1 | 24 h | Reservation rollback, FIFO blocking, nonblocking integration and ordering tests |
| Worker limits, execution, output and final cleanup (5A–B) | 2 | 22 h | Privileges, rlimits, wall timer, executable staging and Linux/macOS behavior |
| Core integration acceptance and logging compliance (Section 7 plus cross-component checks) | All | 7 h | Demonstrate the complete pipeline and resolve interface defects |
| Heartbeats, failure rescue, retry budget and reconnect cleanup (4C) | 1/2; 3 reconnect client | 23 h | Timers, stale reports, disconnect races and full-queue recovery |
| Cancellation, diagnosis and graceful draining (5C/5E/9) | 1/2; 3 commands | 16 h | Distinguish intended stops, reap children and release resources once |
| Live telemetry and load-aware matching (4D/5D) | 2 measurements; 1 selection; 3 display | 12 h | Platform-specific counters/units and policy integration |
| Follow-mode streaming, session routing and slow-client handling (Section 6) | 3; 2 output sender | 12 h | Continuous fan-out, Ctrl+C, terminal footer and bounded buffers |
| Resilience milestone integration | All | 6 h | Combined worker-failure/streaming/drain scenarios |
| Dependencies and groups (Section 8) | 1; 3 parser/events | 11 h | Submit-time snapshots, terminal prerequisites and cancellation cascades |
| Conditional fair-share quotas (Section 11) | 1 | 7 h | Exact boundary/denominator rules and override of FIFO order |
| Preemption (Section 10) | 1 planner; 2 stop flow; 3 events | 16 h | Feasibility over all resources, victim order, hold lifecycle and acknowledgements |
| Recurring templates/jobs (Section 12) | 3; 1 admission | 12 h | Independent IDs, overlaps, timer injection, cancellation and capacity policy |
| Content cache, final worker ranking, inventory and LRU (Section 13) | 2/3; 1 ranking | 26 h | Hash/transfer paths, stale misses, pinning, eviction and restart inventory |
| Advanced combined-feature integration | All | 9 h | Quota/preemption/cron/dependency/cache interactions |
| Cooperative worker checkpoint path and sample workload (Section 14) | 2 | 14 h | Atomic state, permissions, polling, restore and final checkpoint |
| Scheduler checkpoint service and transport (Section 14) | 3; 1 dispatch | 12 h | Versioned state metadata and restore-before-exec ordering |
| Migration lifecycle and three restart-path acceptance tests (Section 14) | 1; all integrate | 13 h | Grace exit races, requeue semantics, source preference and restart verification |
| Final regression, cross-platform/hotspot checks, fixes, docs and release | All | 42 h | Validation on real processes/machines and reproducibility from clean checkout |
| **Total planned** | **All three** | **347 h** | **Plus 42 h team contingency** |

Logging is implemented progressively: common logger in foundation, state/decision calls in each feature, and compliance checks during core/final integration. The per-package budgets are approximate allocations; use the daily per-member schedule below for actual assignment of effort.

### 7.4 Daily task assignments

Every day below has a concrete task for each member. Hours in cells are **that member's focused effort**, including ordinary testing. A task continuing over several dates is deliberately broken into implementation, integration and verification rather than assumed to finish in one sitting. Contingency is held at phase level above and is not included in the daily cells.

#### 26–28 October — foundation

| Date | Member 1: scheduler | Member 2: worker | Member 3: transport/client/services |
|---|---|---|---|
| Mon 26 Oct | **3 h:** Agree state model, ownership, resource invariants and queue/retry policies; draft domain headers | **3 h:** Agree worker execution/stop contract; check Linux/macOS access; build hello/timeout fixtures | **3 h:** Agree frame/message contract; create build skeleton, protocol notes and shared headers |
| Tue 27 Oct | **3 h:** Implement job/worker registries and lifecycle skeleton against agreed interfaces | **3 h:** Build local fork/exec runner with stdout/stderr capture and waitpid | **4 h:** Implement serialization and incremental framing; test split/coalesced reads |
| Wed 28 Oct | **3 h:** Create scheduler poll loop, admission/query API stubs and logging hooks | **3 h:** Add job-directory/executable preparation and stub worker loop; compile OS-specific skeleton | **3 h:** Add nonblocking socket queues and common logger/time helpers; loopback test |

#### 29 October–6 November — core pipeline

| Date | Member 1: scheduler | Member 2: worker | Member 3: transport/client/services |
|---|---|---|---|
| Thu 29 Oct | **3 h:** Implement worker registration/IDs, capacity records and submission admission | **3 h:** Add worker flags, capacity clamp, registration and discovery integration | **3 h:** Implement UDP responder/request, timed attempts and explicit-address fallback |
| Fri 30 Oct | **3 h:** Implement dispatch reservations, execution tokens and started/result handlers | **3 h:** Connect worker assignment/start/final-result handlers to local runner | **3 h:** Implement login, username validation, client poll loop and minimal submit command/handler |
| Sat 31 Oct | **4 h:** Connect admission→dispatch→completion; test capacity release with other members | **4 h:** Implement root/non-root paths, chown, setgid/setuid and minimal final-output forwarding | **4 h:** Implement chunked basic binary transfer, PATH mode and minimal output response; run first shared end-to-end job |
| Sun 1 Nov | **4 h:** Implement FIFO and PRIORITY ordering/aging with policy tests | **4 h:** Apply CPU/AS/NPROC limits and wall timer; test sleeping/CPU-bound jobs | **4 h:** Implement nodes/status/submit RPC handlers and owner event delivery |
| Mon 2 Nov | **3 h:** Add bounded admission/QUEUE_FULL and periodic/event-triggered scheduling | **4 h:** Implement macOS resident-memory enforcement; sudo and privilege-failure tests | **3 h:** Implement scheduler output tail and plain logs command; transfer permissions checks |
| Tue 3 Nov | **3 h:** Test no overcommit, FIFO blocking, priority skip and aging boundaries | **3 h:** Implement ~500 ms output reads, final flush, exit reporting and safe cleanup | **3 h:** Test malformed flags/IDs, same-name sessions and idle events; basic multi-process harness |
| Wed 4 Nov | **3 h:** Fix lifecycle/dispatch and queue-edge issues exposed by integration | **3 h:** Fix worker limit/output ordering; compile and run initial tests on both OSes | **3 h:** Test partial/large transfers and query responsiveness; fix transport/client issues |
| Thu 5 Nov | **3 h:** Review service/core integration; run full core policy suite | **3 h:** Run transferred/PATH programs under sudo and non-root; investigate OS discrepancies | **3 h:** Run first real hotspot discovery/fallback test; review core CLI coverage |
| Fri 6 Nov | **3 h:** Core milestone demo: FIFO/PRIORITY, full queue and conserved resources; fix blockers | **3 h:** Core milestone demo: execution, isolation, timeout and clean output; fix blockers | **3 h:** Core milestone demo: discovery/login/submit/status/logs/events; fix blockers |

#### 7–13 November — resilience and telemetry

| Date | Member 1: scheduler | Member 2: worker | Member 3: transport/client/services |
|---|---|---|---|
| Sat 7 Nov | **4 h:** Implement heartbeat deadlines and ALIVE/SUSPECTED/DEAD transitions | **4 h:** Implement 2-second heartbeat production and CPU-load sampling | **4 h:** Implement log subscriptions/live fan-out and client follow rendering |
| Sun 8 Nov | **4 h:** Implement LOST/requeue/3-total-attempt exhaustion and stale-token rejection | **4 h:** Implement Linux/macOS live RAM/CPU/page-fault sampling and peak RAM | **4 h:** Implement follow Ctrl+C/unsubscribe/footer and per-user multi-session routing |
| Mon 9 Nov | **3 h:** Implement cancellation lifecycle and scheduler stop acknowledgements | **3 h:** Implement kill/reap cancellation and intent-based crash/timeout diagnosis | **3 h:** Add cancel RPC, telemetry/status formatting and failure reason display |
| Tue 10 Nov | **3 h:** Integrate drain state and load-aware worker comparison | **3 h:** Implement local drain/SIGINT, continuing heartbeat and graceful final flush/exit | **3 h:** Implement client reconnect/relogin and bounded follower queues |
| Wed 11 Nov | **3 h:** Test requeue preservation at capacity and duplicate/late result handling | **3 h:** Implement disconnect kill/cleanup, rediscovery and new registration | **3 h:** Extend harness for worker kills, dropped connections and slow clients |
| Thu 12 Nov | **3 h:** Run transient suspicion, death and retry-exhaustion integration tests | **3 h:** Run telemetry, drain, zombies/descendants and signal diagnosis tests | **3 h:** Run live follow/reconnect/multiple-user tests during worker failures |
| Fri 13 Nov | **3 h:** Resilience gate and fixes: no job loss/double release or scheduling on excluded workers | **3 h:** Resilience gate and fixes: cleanup, telemetry and drain across both OSes | **3 h:** Resilience gate and fixes: responsive commands/events during transfers/failures |

#### 14–21 November — advanced scheduling and caching

| Date | Member 1: scheduler | Member 2: worker | Member 3: transport/client/services |
|---|---|---|---|
| Sat 14 Nov | **4 h:** Implement dependency IDs, group snapshot expansion and WAITING lifecycle | **4 h:** Implement content-verified cache store, job copy and startup inventory | **4 h:** Implement scheduler hashing, transfer/reference choice and cache inventory service |
| Sun 15 Nov | **4 h:** Implement dependency cascade and quota rule; test boundary cases | **4 h:** Implement cache budget, per-worker dirs, LRU updates and active-use pinning | **4 h:** Implement cache-miss recovery/eviction updates; integrate hash lookup with ranking |
| Mon 16 Nov | **3 h:** Implement feasible preemption planner and priority/newest victim ordering | **3 h:** Implement preemption stop/reap acknowledgement and cache-miss worker response | **3 h:** Implement cron template records, IDs, validation and immediate first copy |
| Tue 17 Nov | **3 h:** Implement beneficiary holds, acknowledgement-driven requeue and hold release | **3 h:** Test LRU A/B/A/C, missing files, restart inventory and pinning; fix cache issues | **3 h:** Implement recurring timer/overlap, crons/cancel-cron and full-queue policy |
| Wed 18 Nov | **3 h:** Integrate idle/cache/CPU/job-count ranking and final scheduling order | **3 h:** Test preemption kill/restart without consuming retry budget; fix stop races | **3 h:** Add advanced flags/errors/events and integrate cron via ordinary admission API |
| Thu 19 Nov | **3 h:** Run quota+FIFO, groups+failure and preemption+cancel interaction tests | **3 h:** Run cache persistence/miss/size-limit tests on Linux/macOS | **3 h:** Run cron overlap/cancellation/queue-full and cache transfer integration tests |
| Fri 20 Nov | **3 h:** Fix advanced scheduling interactions and review invariants | **3 h:** Fix cache/preemption cross-platform defects; prepare checkpoint directory API | **3 h:** Fix client/cache/cron issues; agree checkpoint transport fields and sequence |
| Sat 21 Nov | **4 h:** Advanced gate: dependencies/quotas/preemption/final ranking and fixes | **4 h:** Advanced gate: cache/LRU/pinning/stops; add atomic checkpoint sample | **4 h:** Advanced gate: cron/cache transport/client coverage and fixes |

#### 22–25 November — checkpoints and migration

| Date | Member 1: scheduler | Member 2: worker | Member 3: transport/client/services |
|---|---|---|---|
| Sun 22 Nov | **4 h:** Implement migrate validation/intent, source preference and requeue lifecycle | **4 h:** Implement OSN_CHECKPOINT_DIR, writable state dir and ~500 ms checkpoint detection | **4 h:** Implement scheduler latest-state storage/metadata and checkpoint transfer messages |
| Mon 23 Nov | **3 h:** Integrate restart dispatch with checkpoint-before-binary ordering | **3 h:** Implement checkpoint restore before exec and SIGTERM/grace/SIGKILL sequence | **3 h:** Implement checkpoint-before-binary transport and migrate/status client support |
| Tue 24 Nov | **3 h:** Test migration/preemption/worker-loss accounting and stale report races | **3 h:** Capture final grace checkpoint/output; test early SIGTERM exit and no-state jobs | **3 h:** Run multi-worker resume tests after migrate/preempt/worker death; fix transfer races |
| Wed 25 Nov | **3 h:** Feature freeze gate: complete restart/migration paths and lifecycle fixes | **3 h:** Feature freeze gate: prove resumed progress under sudo on both OSes | **3 h:** Feature freeze gate: complete all commands/flags and checkpoint metadata tests |

#### 26–30 November — regression, fixes and release

| Date | Member 1: scheduler | Member 2: worker | Member 3: transport/client/services |
|---|---|---|---|
| Thu 26 Nov | **3 h:** Run saturation, scheduling/rescue and combined-feature regressions | **3 h:** Run OS limits, cache/cleanup/checkpoint combined regressions | **3 h:** Run malformed/slow/large-transfer and multi-client regressions |
| Fri 27 Nov | **3 h:** Investigate scheduler defects from clean-checkout/multi-machine test | **3 h:** Investigate Linux/macOS/sudo defects from clean-checkout test | **3 h:** Coordinate second hotspot test and clean-build/CLI regression; fix service defects |
| Sat 28 Nov | **4 h:** Fix remaining scheduler issues; write architecture/policy/known-decision docs | **4 h:** Fix remaining worker issues; write OS/limits/cache/checkpoint instructions | **4 h:** Fix remaining protocol/client issues; assemble README and reproducible demo script |
| Sun 29 Nov | **2 h:** Review full 400-mark checklist and another member's module; rehearse demo | **2 h:** Review workload/results and another member's module; rehearse sudo/recovery demo | **2 h:** Review command coverage and another member's module; validate packaging instructions |
| Mon 30 Nov | **2 h:** Final clean regression and candidate review; confirm no unresolved core defects | **2 h:** Final Linux/macOS/sudo smoke runs; verify cleanup and reproducible workloads | **2 h:** Verify README/package from clean checkout; record final candidate and backup |

### 7.5 Dependencies and checkpoints that protect the timeline

1. **26 October: agree interfaces.** Domain types, message structure, ownership, units, stop reasons and execution tokens are decided together. Later fields can extend the contract, but avoid parallel incompatible protocols.
2. **28 October: foundation gate.** Member 1 can test policies using local data; Member 2 can test a standalone runner; Member 3 can test framing in isolation. Nobody should sit idle waiting for the full network path.
3. **31 October: first integrated job.** It need not have every limit or command, but executable transfer, assignment, output and result must traverse all three components. Use this to discover protocol problems before adding policy complexity.
4. **2–5 November: early platform/network check.** Validate macOS memory behavior, root execution, and broadcast on actual available machines. Do not defer these until the final week.
5. **6 November: accept the core before adding recovery.** Admission, dispatch and release invariants must work first, because retries and preemption amplify lifecycle mistakes.
6. **13 November: accept retry/stop lifecycle before preemption.** Reuse reliable stop/reap/result handling; do not invent another uncoordinated state machine.
7. **20–21 November: agree the final checkpoint messages and prepare the sample program.** Actual checkpoint work then has a defined transport and known test workload.
8. **25 November: freeze features.** All requested commands/flags and all four milestones must be integrated. No optional GUI, scheduler replication, containers or arbitrary process snapshots.
9. **26–30 November: validate and fix.** A feature is only complete if it works in the full cluster, on required platforms, with documented reproducible steps.
10. **30 November: final candidate.** Build/test from a clean checkout, verify the actual course submission requirements, retain the final commit/package and a backup. Do not assume a private local build represents a reproducible submission.

The daily work table is the default plan, not a rule to postpone a finished task. If a dependency slips, use a fixture/stub for the agreed interface and keep independent policy/runner/client work moving.

Daily rows describe task order and effort, not compulsory daily attendance. You can move a session within its phase to accommodate classes, exams or a rest day, provided the total availability and shared acceptance date remain realistic. Agree shared integration sessions in advance. Initial 31 October output forwarding may send final captured output; the required periodic live forwarding is completed during the core milestone and follow subscriptions are completed during resilience.

### 7.6 How to handle delays realistically

- Spend 10–15 minutes at the end of a working day on what passed, what is blocked, and remaining effort. This is included in the planned hours, not an extra meeting.
- Re-estimate at the six phase gates. Compare actual hours and passing acceptance tests with the baseline; do not report a feature complete solely because its code exists.
- If a task exceeds its budget by about 25%, or blocks another member for a day, identify the cause and assign specific help. For example, after transport stabilizes Member 3 can help with recovery tests; Member 1 can help with cache ranking while Member 2 resolves OS behavior.
- Consume the current phase's contingency first. If a phase will exceed that reserve, update subsequent allocations immediately; reduce optional polish and transfer a well-defined subtask. Do not silently push required testing into 1 December.
- A high-risk gate still failing after its buffer needs an explicit team availability discussion. The schedule cannot guarantee the full assignment if actual experience/availability differs substantially from the assumption.
- Keep 29–30 November's shorter planned sessions and final contingency available for late fixes. Do not fill spare time with new scope.
- Everyone keeps responsibility for tests/documentation in their owned area. Member 3 assembles the final README/harness but does not inherit all late debugging.

### 7.7 Release acceptance checklist for 30 November

- [ ] Clean Linux and macOS builds and runnable scheduler/worker/client using the exact specified CLI.
- [ ] All requirements in Section 2 mapped to a passing check; full test matrix in Section 9 reviewed.
- [ ] Actual hotspot discovery/fallback and restart tested; multiple clients/workers remain responsive.
- [ ] Sudo limits/privilege drop and macOS RAM enforcement verified.
- [ ] Queue saturation, retry exhaustion, preemption holds, quota ordering and dependency cascades checked.
- [ ] Cache miss/retransfer/LRU/pinning/restart inventory demonstrated.
- [ ] Cooperative progress resumes after migration, preemption and worker death.
- [ ] No zombies, double resource release, stale-execution corruption or premature deletion of final output/checkpoint.
- [ ] README and demo commands work from a clean checkout; all three members can explain their modules and integration.
- [ ] Course packaging/cutoff requirements checked; final candidate and backup prepared; submit early if allowed.

## 8. Open decisions and recommended policies

The brief leaves these details open or in tension. Agree and document them early; seek instructor clarification if available without delaying unrelated implementation.

1. **Bounded queue versus guaranteed rescue:** a fixed 1024-entry queue cannot accept unlimited internal requeues when already full. Recommended: keep the ordinary pending queue bounded and preserve overflow rescues in a scheduler-owned recovery backlog of existing job IDs; merge both when constructing scheduling order. Fresh submission returns QUEUE_FULL at admission capacity. This is an interpretation, not an explicit prescribed structure. Confirm the grader's expected capacity semantics if possible.
2. **WAITING admission and cron overflow:** the brief does not define whether WAITING consumes capacity or what happens to cron instances when full. Recommended: count accepted pending jobs against admission capacity, keep dependency-blocked jobs out of sorting, and retain due cron occurrence metadata until admitted, with a clear event. Do not silently drop work or let cron bypass the queue bound. Define how delayed occurrences are counted.
3. **Aging after requeue:** define whether queued waiting time accumulates across multiple queued intervals. Recommended: accumulate only time actually QUEUED, preserving earned aging; exclude RUNNING and WAITING time. Original submission sequence still breaks ties.
4. **Which priority governs preemption:** the final order says apply aging before testing priority 1, suggesting effective priority. Victim priority basis is not explicit. Recommended: use the same documented effective-priority view for ordering and victim selection; confirm if instructors intend base priority for either case.
5. **Retry counters:** implement three total failure attempts, not three retries after the first. Separate execution IDs from this budget so migration/preemption cannot exhaust it. Define whether preparation/exec failures are terminal failures or consume a recoverable attempt; ordinary program failure is FAILED, not automatic worker-loss rescue.
6. **Multiple same-name sessions and permissions:** recommended owner events fan out to every live session for that username. Cross-user cancellation is illustrated in the brief; do not invent an owner-only cancellation restriction. No complete authorization policy is specified for other commands.
7. **Oversized/pinned cache:** the brief requires eviction until under the limit but forbids evicting the new file and active files, which can make the target impossible. Recommended: never remove protected files; warn about temporary over-budget state and retry eviction when jobs finish. Confirm expected handling of a single binary larger than the cache.
8. **Migration grace exit:** a cooperative job may exit during SIGTERM grace. Preserve the migration intent and requeue with its final checkpoint rather than treating that exit as normal completion/failure. Check once more for new checkpoint/output before cleanup.
9. **Job-directory collisions in local tests:** `/tmp/osn_job_<ID>` is global on one machine. Multiple worker processes can conflict during same-machine restart tests. Preserve the specified path, but use unique internal execution subdirectories or serialize confirmed cleanup before another worker prepares that ID. Document the chosen approach.
10. **Executable compatibility:** byte transfer does not make Linux executables runnable on macOS or vice versa, nor across incompatible CPU architectures. Use compatible workers/programs for a test and portable scripts for suitable mixed-OS tests. Architecture-aware scheduling is not requested.
11. **Scheduler restarts:** scheduler durability, replication, failover and restoring in-memory jobs/checkpoints after a scheduler crash are not specified. Build the required reconnect behavior; do not assume that restarting the scheduler restores the old cluster state.
12. **Large output/checkpoints and reconnect submissions:** choose explicit message/size limits and error handling. Do not automatically replay an unacknowledged submit after reconnect unless a request-deduplication mechanism prevents duplicate jobs.

## 9. Verification matrix

Use C unit tests for policy/lifecycle helpers and a process-driving integration harness for actual binaries. These tests verify consequential OS/network behavior, not just implementation details.

| Area | Required checks |
|---|---|
| Arguments/login | Missing/invalid flags, username boundaries and allowed characters, same-name concurrent sessions, reconnect login |
| Discovery | Broadcast success; three timed attempts; fallback only afterward; failure without fallback; different ports; immediate restart |
| Framing/responsiveness | Fragmented header/payload, multiple frames per read, binary zeros, slow peer, disconnect mid-transfer, status during a large transfer |
| Registration | Capacity clamping/warnings, GPU not clamped, assigned IDs, new ID after reconnect |
| Queue | Full queue rejects immediately; accepted jobs never vanish; worker-loss and preemption requeues survive saturation |
| FIFO/priority | Strict FIFO blocking; priority ordering/stable age ties; 10/20-second aging; PRIORITY skips non-fitting jobs |
| Resources | Cores/RAM/GPUs all checked; no overcommit during dispatch; release once on every terminal/requeue path |
| Limits/isolation | Own cwd; root UID/GID change; denied protected write; exec failure; CPU loop; sleeping timeout; allocation stress; process-limit behavior |
| Output | Both streams captured, no worker-terminal leakage, live updates near 500 ms, final bytes before terminal result, retained 64 KB tail |
| Signals/cleanup | SIGSEGV, SIGFPE, exit nonzero/127, cancellation race, no zombies/descendants left behind, temp cleanup after send |
| Heartbeats/recovery | SUSPECTED after one miss, recovery before death, DEAD after three, no new work while suspected, three total attempts, stale token ignored |
| Telemetry | Growing RAM sample, CPU time/page faults, finished peak RAM, platform-specific units verified |
| Follow/events | Idle prompt events; owner routing; multiple subscribers; Ctrl+C only stops follow; finish footer; slow follower cannot freeze cluster |
| Drain | stdin and SIGINT equivalent; no new assignments; heartbeat while draining; exit only after output/results; no false rescue |
| Dependencies | All-success release; failed/cancelled cascading; unknown references rejected; group submit-time snapshot; WAITING excluded from ordering |
| Quota | 3-of-4 cores with competing user; exactly 50%; no competitor; SUSPECTED/DRAINING denominator; skip precedes FIFO no-fit |
| Preemption | Insufficient resources; non-preemptible protection; victim priority/newest tie; only enough victims; hold release; no retry charge |
| Cron | Independent IDs; immediate first copy; overlapping runs; inherited flags/user; dependency incompatibility; cancel future only; full-queue policy |
| Cache | First transfer/second hit; one-byte change; deleted cache retransfer; startup inventory; idle beats busy cached; cached idle wins; pinning |
| LRU | With 2 MB budget and ~0.7 MB programs: A, B, A, C evicts B; resubmitting B transfers again |
| Checkpoint/migration | Atomic state publication; status sequence/size/age; final grace checkpoint; restore before exec; different-worker preference; invalid migrate errors |
| Combined behavior | Checkpointed preemption and worker loss; cron plus quota; dependency failure after retry exhaustion; cache eviction during execution; drain while followed |

Suggested workload programs: hello/stdout-stderr, sleeping timeout, CPU spinner, memory growth, nonzero exit, signal crash, gradual flushed output, modest child-process tree, and cooperative progress counter with SIGTERM checkpointing. Use explicit flushes in streaming fixtures so workload buffering does not hide transport behavior.

## 10. Completion checklist

- [ ] All specified flags and commands supported by the exact three executable names.
- [ ] All 14 assignment sections mapped to implemented modules and tests.
- [ ] Core operation, fault recovery and advanced policy interactions demonstrated.
- [ ] Linux and macOS exercised, including sudo privilege dropping and macOS memory enforcement.
- [ ] Real multi-machine discovery/fallback tested on a suitable LAN/hotspot.
- [ ] No job loss on admission/requeue edge cases; no stale-session corruption or double resource release.
- [ ] Checkpoint resumes verified for migration, preemption and worker failure.
- [ ] README includes build/run examples, environment variables, known policy decisions and test/demo instructions.
- [ ] Each member's own descriptive commits show a roughly even contribution.

The screenshots do not specify a submission packaging format, a mandatory report, or presentation rules. Check the course portal for those administrative requirements separately.

## Source coverage

Every file in `Questions/` was visually inspected: 39 screenshots, from `Screenshot 2026-10-25 162338.png` through `Screenshot 2026-10-25 163446.png`. They cover the contents, introduction, four milestones, state machines, Sections 2–14, consolidated scheduling order, epilogue and 400-mark summary. Several screenshots overlap; overlapping content was treated as the same requirement rather than additional work.
