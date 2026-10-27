# Actual progress — 27 October 2026

## Current scope

Current coding focus is **Member 1 only**, covering the tasks originally allocated to 26 and 27 October. No work from the baseline is assumed completed solely because its date has passed.

- Foundation code, public APIs, interactive driver and tests: implemented.
- Original 28 October scheduler event loop and admission/query service boundaries: next.
- FIFO/PRIORITY and network assignment/recovery: pending.
- Worker and production shared networking/client: pending.

The local folder is initialized as a Git repository with origin `https://github.com/AryanJaiswal-MuEta/OSN-Course-Project.git`. The remote had no refs when checked. Current local branch: `member1/foundation`. Foundation code and tests are committed locally; no push has been made. The original question screenshots remain local and are not included in these implementation commits.

## Immediate pacing

| Working session | Focus | Suggested focused time |
|---|---|---:|
| 27 Oct: this implementation | Domain records, registries, lifecycle foundation, driver and tests | Review/understand the delivered slice; original two-day task budget was 6 h |
| Next Member 1 session | Read and exercise the API, agree it with Member 3, then scheduler loop/service contract | 1 h review + 2–3 h implementation |
| Following session | Worker registration/admission handlers and split dispatch reservation from confirmed start | 3–4 h |
| After transport integration | End-to-end one-job test, then FIFO/PRIORITY and aging | Re-estimate after loop/transport are working |

These are remaining learning/implementation estimates, not a claim about measured time spent generating this code. Understanding the functions and tests is part of the project work.

If the earlier plan to personally cover both Members 1 and 2 still applies, the old equal-person timeline is not an individual commitment of 3 h/day: those roles contain 231 planned hours plus 28 contingency hours. From 27 October through 30 November there are 35 calendar days, averaging about 7.4 h/day for that combined allocation. Reallocate work or actual availability before treating the old dates as achievable; do not remove cross-platform/integration tests to hide the workload.

## Verification

- WSL Ubuntu: `make test` passes, eight C groups / 1198 checks plus interactive-driver scenarios.
- Compiler warnings enabled: C11, `-Wall -Wextra -Wpedantic -Werror`.
- AddressSanitizer and UndefinedBehaviorSanitizer: `make sanitize` passes the same eight C groups / 1198 checks and driver scenarios, with no reported sanitizer errors.
- macOS: not run here.

## Not implemented in this change

No scheduler TCP executable, UDP discovery, production client, worker execution, scheduling policy, asynchronous events, automatic retries, dependencies, quotas, preemption, cron or checkpoint transport. Future-state fields are preparation only. The standalone driver simulates state updates without opening files/sockets or starting processes.
