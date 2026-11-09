# Revised implementation plan — 9 November 2026

This replaces the old calendar as a forecast. It does not mark missed dates as completed work. Deadline: **2 December**. Internal completion target: **30 November**. All estimates are focused effort, including routine testing and review; commits use the actual dates work is done.

## Capacity and assumptions

Aryan owns Members 1 and 2 and has confirmed **5–6 focused hours per day combined**, not per role. There are 22 calendar days from 9 through 30 November: a theoretical 110–132 hours. The plan budgets about **95–115 hours of remaining role-1/role-2 work plus 12–17 hours of contingency**. Class/exam/rest commitments that reduce this availability require adjusting scope distribution or targets, rather than dropping tests.

This is a tight target, not a completion guarantee. It assumes Member 3 independently owns working framing, nonblocking transport, discovery, client/login/services, cron, scheduler executable/checkpoint storage and the related serialization. If Aryan takes those tasks too, this forecast no longer fits the stated capacity.

## Actual state after repository inspection

| Area | Current evidence |
|---|---|
| Member 1 foundation | Registry, lifecycle/accounting, poll-loop hooks, query snapshots and registration service exist; network handler wiring is not complete |
| Member 1 today | Dispatch reservation/confirmation/abort and focused tests, described in `dispatch.md` |
| Member 1 remaining | Actual policies, listener/session/assignment integration, fault recovery and advanced scheduling |
| Member 2 foundation | Local fork/exec runner, job-directory/staging, limits, output, telemetry, cancellation, cleanup, loop skeleton and startup configuration helpers exist |
| Member 2 remaining | Actual exec/start acknowledgement, production worker startup/registration, connection lifecycle/heartbeats/drain, cache and checkpoints |
| Shared networking | Protocol implementation has arrived; strict compilation currently fails. Transport has declarations but no function implementations; discovery/logging are empty and there is no production client |
| Build integration | Recent syncs removed worker-loop/config build/test rules even though those source files remain. Integrate those modules deliberately at the next worker/integration step |
| Cross-platform | Previous WSL normal/root and sanitizer checks exist for local components. macOS execution and real multi-machine discovery remain unverified |

The strict check used for the new protocol implementation is `cc -Iinclude -D_POSIX_C_SOURCE=200809L -std=c11 -Wall -Wextra -Wpedantic -Werror -fsyntax-only src/common/protocol.c`. It reports pointer-signedness errors and an always-true magic comparison caused by the current get32 result range. Member 3 should correct and test framing before transport integration. This inspection did not edit their files.

## Revised sequence and time budgets

Hours below are **Aryan's combined effort across Members 1 and 2**. Member 3's concurrent work is additional; do not count it as free time in Aryan's budget.

| Target dates | Aryan's task | Estimated hours | Member 3 dependency | Passing gate |
|---|---|---:|---|---|
| **9 Nov** | Member 1: reserve resources, confirm start, abort safely, expose counters and test | 3–4 | No transport rewrite today | Preparation cannot overcommit or lose a job; late/duplicate messages cannot corrupt resources |
| **10 Nov** | Member 2: distinguish fork from actual exec confirmation; expose start outcome, connect to loop callbacks; restore relevant worker build rules | 3–4 | Agree JOB_ACCEPTED/start semantics | Successful exec produces start outcome; setup/exec failure cannot masquerade as a running program |
| **11 Nov** | Member 1: FIFO, PRIORITY, aging and policy tests, excluding reserved jobs from new selection | 4–5 | Stable job requirement fields | Strict FIFO blocks; priority skips nonfits; aging reaches priority 1 correctly |
| **12–14 Nov** | Member 1/2: real scheduler/worker startup and session adapters, registration/replies, dispatch/result/output wiring, then core integration | 14–17 | Working protocol/transport, discovery and minimal client; agree payloads first | A client submits a compatible program, a real worker runs it over TCP, output/result return, reservations release; basic Linux/macOS/hotspot checks start here |
| **15–18 Nov** | Member 1/2: heartbeat/loss state machine, retry budget, cancel/stop races, drain/reconnect, telemetry and load selection; integrate live-follow path | 18–22 | Output subscriptions/client events/logging and stable transport | Worker kill/partition triggers recovery; retry exhaustion works; cancel/drain/follow do not leak resources or zombies |
| **19–24 Nov** | Member 1/2: dependencies/groups, conditional quotas, preemption/holds, worker executable cache/LRU and final worker ranking; combined tests | 26–30 | Cron/templates, executable content IDs/transfers/inventory and advanced flags | Dependency failure cascades; quota overrides order correctly; urgent jobs preempt only enough victims; cache hit/miss/pinning/LRU work together |
| **25–27 Nov** | Member 1/2: cooperative workload, checkpoint capture/restore, migration stop/grace/requeue/source preference and shared restart path | 13–16 | Scheduler checkpoint retention/transfer and client migrate/status support | Progress resumes after migrate, preemption and worker failure; no checkpoint means a clean restart |
| **28–30 Nov** | Full integration/regression, clean builds, Linux/macOS and sudo validation, hotspot rerun, docs/demo/package and fixes | 14–16 | All three members participate and fix their modules | Reproducible final candidate and backup; exact submission requirements checked |

This totals approximately **95–114 planned hours**, plus contingency. Use lighter days for review and recovery rather than adding optional features. The 1 December buffer is for emergencies after the planned completion date; the final submission time must be checked on the course portal.

## How to pace the advanced block

- **19 Nov:** Member 1 dependencies/group snapshots and failure cascade.
- **20 Nov:** Member 1 quota checks/boundaries; start Member 2 cache data/storage interface if time remains.
- **21 Nov:** Member 1 preemption feasibility, victim order and hold rules; keep worker stop/result behavior explicit.
- **22 Nov:** Member 2 cache/LRU/pinning and cache-miss recovery using Member 3's content identifiers, with no new hash implementation in our modules.
- **23 Nov:** Finish cache placement/inventory integration; integrate cron through ordinary admission and test preemption acknowledgements.
- **24 Nov:** Combined policy/cache/cron/dependency regressions and fixes. Any unfinished work consumes contingency; it is not automatically called complete.

These daily emphases are flexible within the block. Do not edit scheduler and worker internals indiscriminately in one large commit; keep one functional slice reviewable and passing its focused tests.

## Gates and delay policy

1. **12 Nov integration checkpoint:** Member 3's framing/transport must be usable. Fix/agree interfaces now rather than creating a second incompatible protocol.
2. **14 Nov core gate:** if a real networked job still cannot run, redistribute integration work immediately and re-estimate the advanced block. Standalone drivers do not satisfy this gate.
3. **18 Nov resilience gate:** stable stop/reap/acknowledgement and resource conservation come before preemption/migration.
4. **24 Nov advanced gate:** re-estimate remaining checkpoints against actual hours and remaining contingency. Do not hide missing features in documentation.
5. **27 Nov feature freeze:** required integrations should be complete. Final days verify and fix; they are not spare feature-development days.
6. At each session, fetch/check teammates' changes first, implement a coherent slice, test relevant behavior, and fetch again before a non-forced push. Never backdate or invent work history.

Early platform testing and incremental integration are required throughout these blocks; the last three days are the final rerun, not the first time the system is exercised on another OS/machine.

## Today's explicit boundary

Only the Member 1 dispatch slice is implemented in today's change. This plan does not implement tomorrow's worker acknowledgement, scheduling algorithms, network transport, resilience or advanced features. The rest remains scheduled work.
