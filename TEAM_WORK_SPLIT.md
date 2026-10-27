# Three-member implementation split

Updated 26 October 2026. The complete member split is now also included in `IMPLEMENTATION_PLAN.md`, Section 6. Its Section 7 is the authoritative daily schedule and effort budget: 26 October–30 November, with no planned work on 1 December.

Use Member 1/2/3 as placeholders for your names. This expands the A/B/C split in IMPLEMENTATION_PLAN.md: Member 1 = A, Member 2 = B, Member 3 = C.

The split is by ownership of modules, with responsibilities in every milestone. Marks attach to assignment features that cross modules, so they are not a reliable measure of each member's workload. Every member implements, tests and documents their own modules and commits their own work.

## Ownership at a glance

| Member | Owns | Main paths |
|---|---|---|
| 1 | Scheduler core, job lifecycle, scheduling policies and resource accounting | `src/scheduler/main.c`, `loop.c`, `registry.c`, `admission.c`, `lifecycle.c`, `policy.c`, `recovery.c`, `dependencies.c`, `quotas.c`, `preemption.c`, `dispatch.c` |
| 2 | Worker, process execution, OS limits, telemetry, local cache and checkpoint handling | `src/worker/` and `tests/programs/` |
| 3 | Common transport/discovery, client, and scheduler-facing service modules | `src/common/`, `src/client/`, `src/scheduler/client_service.c`, `output_service.c`, `artifact_service.c`, `cron.c`, plus initial build configuration |

Suggested filenames are design choices, not required names from the assignment. Member 3 owns the initial Makefile; each member updates build entries for their own new files.

## Member 1 — Scheduler core and scheduling policy

### Milestone 1: core pipeline

- Scheduler command-line validation and TCP listening/event loop, using Member 3's transport and discovery helpers.
- Worker registration, ID allocation, worker table and job registry.
- Bounded submission admission and explicit QUEUE_FULL responses through the service interface.
- All public job/worker state transitions and common lifecycle functions.
- Requested resource reservations and release, including GPUs and dispatch-in-progress reservations.
- FIFO ordering/head-of-line blocking; PRIORITY ordering and aging.
- Resource matching and assignment orchestration, including worker start acknowledgement.
- Read-only query APIs for Member 3's nodes/status handlers.
- Scheduler-specific transition/assignment logs using the shared logger.

### Milestone 2: resilience

- Scheduler-side heartbeat deadlines, SUSPECTED recovery and DEAD transitions.
- LOST/requeue/FAILED_PERMANENT behavior and three-total-attempt failure accounting.
- Execution tokens and rejection of stale worker reports.
- Load-aware worker choice from heartbeat data.
- Scheduler cancellation lifecycle, resource release and drain-state handling.
- Guaranteed preservation of internally requeued jobs under queue saturation.

### Milestone 3: advanced policy

- Dependency graph, group snapshot resolution, WAITING release and cascading cancellation.
- Per-user running-core accounting and conditional fair-share quota rule.
- Preemption feasibility, victim selection/order, holds and beneficiary cleanup.
- Final worker ranking: idle, cache hit, CPU load, running-job count.
- Integrate cron-created jobs through the same admission and lifecycle APIs.

### Milestone 4: migration scheduling

- Validate RUNNING state, initiate migration and track migration intent.
- Requeue after stop confirmation without consuming failure budget.
- Prefer another qualifying worker, and reuse the normal dispatch path for restoration.
- Ensure preemption, failure recovery and migration all share correct restart accounting.

### Tests and review responsibility

Own policy/lifecycle tests: FIFO blocking, priority aging, quota ordering, dependencies, resource conservation, retries and preemption holds. Review Member 3's scheduler service integrations for state ownership and accounting correctness.

## Member 2 — Worker execution and OS behavior

### Milestone 1: core pipeline

- Worker flags, capacity measurement/clamping and registration client.
- Worker event loop and assignment handling using Member 3's transport.
- Receive/stage executable content; prepare the job directory and executable permissions.
- Fork/execvp, child descriptor setup and process cleanup.
- Required chown and setgid/setuid behavior; checked failures and non-root warning.
- CPU/address-space/process limits and independent wall-clock deadlines.
- Linux/macOS differences, including macOS resident-memory enforcement.
- stdout/stderr redirection, incremental output reads and final output flush.
- Report start/exit results and clean up temporary directories at the correct time.

### Milestone 2: resilience

- Heartbeat production every two seconds with load and running-job telemetry.
- Live RAM, CPU time, page faults and final peak memory.
- SIGKILL/reap cancellation and readable exit/signal diagnosis.
- Local drain command and SIGINT behavior; complete output/results before graceful exit.
- Kill local executions on scheduler connection loss, then rediscover/re-register.
- Keep intentional cancellation/preemption/migration reasons distinct from crashes.

### Milestone 3: worker cache and preemption execution

- Cache directory and OSN_CACHE_DIR support.
- Content-verified cache storage, startup scan and inventory reporting.
- Cache hit/miss handling and executable copy into the per-job directory.
- OSN_CACHE_SIZE_MB, LRU timestamps updated on job start, active-use pinning and eviction notifications.
- Handle oversized/pinned-cache cases according to the team's documented policy.
- Execute preemption stop requests and acknowledge only after reaping.

### Milestone 4: cooperative checkpoints

- Writable checkpoint directory and OSN_CHECKPOINT_DIR environment setup.
- Roughly 500 ms change detection and checkpoint updates to scheduler.
- Restore checkpoint state before exec.
- Migration SIGTERM, nonblocking two-second grace and SIGKILL if necessary.
- Capture final output/checkpoint even if the process exits during grace.
- Build the cooperative progress-counter sample workload.

### Tests and review responsibility

Own controlled workload programs and worker integration tests: privileges, cwd, limits, CPU/sleep timeouts, crash signals, output, cancellation, child cleanup, cache eviction and checkpoint resumption. Review Member 1's stop/retry interactions against actual worker behavior.

## Member 3 — Networking, client and scheduler services

This member also owns concrete scheduler modules so the workload includes more than the interactive client.

### Milestone 1: shared foundation and client

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

### Milestone 2: streaming and sessions

- At least 64 KB retained output per job, follower subscriptions and final footer.
- Client follow mode and Ctrl+C unsubscribe without exiting.
- Owner-to-session routing, including multiple same-username sessions.
- Client cancellation request handling and display of telemetry/diagnoses.
- Reconnect handling, explicit disconnection errors and no accidental submit replay.
- Bounded slow-client output queues so followers cannot freeze the cluster.
- Multi-client/multi-worker integration harness.

### Milestone 3: cron and executable transfer services

- Scheduler cron template storage, independent IDs, due timer and immediate first injection.
- crons/cancel-cron handlers and -every validation/inherited specification.
- Integrate injected jobs through Member 1's admission API; implement the agreed full-queue policy.
- Scheduler-side content hashing and per-worker cache inventory.
- Full transfer versus cache reference, stale-cache miss recovery and inventory updates.
- All advanced client flags and response/event formatting.

### Milestone 4: checkpoint transport and client integration

- Scheduler storage of the latest checkpoint bytes, sequence, size and receipt time.
- Transfer saved checkpoint before binary/reference during dispatch.
- Client migrate command and checkpoint metadata in status.
- End-to-end migration/recovery/preemption test orchestration.
- Assemble README build/run/demo instructions, with technical content supplied by Members 1 and 2.

### Tests and review responsibility

Own framing, partial transfer, discovery/fallback, client event/follow, cache protocol and cron tests. Review Member 2's socket/file-transfer handling and ensure worker activity cannot block networking.

## Interfaces to agree before independent implementation

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

## Collaboration rules

1. Each module has one primary editor. Cross-module changes go through the owner or a small reviewed patch.
2. Member 1 owns the scheduler loop and calls Member 3's services through APIs. Member 3 does not independently mutate job states or resource counters.
3. Decide queue/recovery capacity, cron overflow, aging after requeue, priority basis and oversized-cache behavior together; record decisions.
4. Integrate a minimal end-to-end slice early. Do not wait for all components to be complete.
5. Each member writes relevant tests and documentation; Member 3 coordinates integration rather than becoming the sole tester.
6. Commit your own work with descriptive messages. Review in a cycle: 1 reviews 3, 3 reviews 2, 2 reviews 1.
7. All three participate in Linux/macOS, sudo and real hotspot testing. Assign actual machines based on availability.

## Joint acceptance gates and target dates

| Target | Member 1 demonstrates | Member 2 demonstrates | Member 3 demonstrates |
|---|---|---|---|
| 28 Oct: foundation | State model and scheduling/admission APIs | Local runner and worker protocol contract | Three builds, agreed frames and basic communication |
| 6 Nov: milestone 1 | Correct FIFO/PRIORITY and conserved resources | Transferred program runs with limits and captured output | Discover, log in, submit and inspect from client |
| 13 Nov: milestone 2 | Worker death and retry exhaustion | Telemetry, cancel, drain and reconnect cleanup | Live logs, idle events and concurrent clients |
| 21 Nov: milestone 3 | Dependencies, quota, preemption and worker ranking | Cache/LRU/pinning and stop acknowledgements | Cron, advanced flags and cache hit/miss protocol |
| 25 Nov: milestone 4 / feature freeze | Correct migration/restart lifecycle | Checkpoint capture/restore and grace handling | Checkpoint transport/status and end-to-end demo |
| 30 Nov: final validation / internal completion | Scheduling/accounting interaction tests | Linux/macOS and sudo execution checks | Network/client tests, documentation and verified final candidate |

These are proposed internal targets. Final assignment deadline: 2 December 2026. Planned effort is 115 / 116 / 116 hours for Members 1 / 2 / 3, plus 14 contingency hours each (129 / 130 / 130 hours allocated in total). Feature work ends on 25 November; validation and release preparation run through 30 November, two calendar days before the deadline.

## First tasks for each member

- **Member 1:** define Job/Worker states and records, implement registries/admission and a pure FIFO/PRIORITY selection function with tests.
- **Member 2:** build a local runner that forks/execs a sample job, redirects output, applies limits, enforces timeout and reaps it; then wire it to assignments.
- **Member 3:** establish the three build targets, shared framing and a loopback exchange, then discovery and the client login/submit parser.

After the interfaces are agreed, these first tasks can proceed concurrently. The first shared demo should be one client submitting one transferred program through the scheduler to one worker, with status and output returned.
