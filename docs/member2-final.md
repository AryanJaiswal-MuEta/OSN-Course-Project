# Member 2 completion check — 22 November 2026

Based on main e586111. The worker's independent execution, cache, telemetry, discovery/reconnect, drain, stop and checkpoint receive/restore implementation is complete against the current scheduler contract. Member 3's production artifact/client services and joint platform validation remain separate release gates.

## Final worker fixes

**Per-execution flush barriers:** completed jobs previously waited for the entire worker send queue to empty. Another continuously streaming job could keep that queue nonempty on every polling tick, starving the completed job's final report and keeping scheduler resources occupied. The worker now counts bytes actually sent on its connection and records each execution's last output, checkpoint and final-result positions in that stream.

Only that job's output/checkpoint prefix must flush before its directory is cleaned. Its final report follows cleanup, preserving immediate same-host migration/retry safety. Once its result prefix is sent, its metadata/cache pin can retire even if unrelated output was queued later. Partial socket writes advance the count only by bytes actually sent; a reconnect starts a new count after killing/reaping old jobs. Backpressure still retries unsent data without advancing per-job output offsets.

**Control traffic first:** due heartbeat and drain messages are queued before bulk inventory/checkpoint updates. Existing queue bounds and connection-failure handling remain in force; this does not claim heartbeats can bypass bytes already sent into a TCP stream or tolerate an indefinitely stalled peer.

**LRU starts at confirmed exec:** cached content is touched after the successful-exec start message is accepted into the outgoing queue. Fork success or a failed interpreter/exec no longer makes a program appear recently executed. The existing pin protects staging/active use until that execution is retired.

**Exact duplicate-byte checks:** retransmitting an existing cache identity compares complete bounded chunks with pread, handling short reads/EINTR and checking both files' declared sizes. A mismatch beyond the first 16 KiB is rejected without replacing the cached program. No hash function/table was added. First-download cryptographic identity verification is still Member 3's artifact/identity responsibility; exact byte comparison and length checks do not substitute for SHA verification.

## Regression checks

* A short real job completes while a second job continuously writes output; the producer is still running when the short job's report arrives. Cleanup and unrelated-stream handling are checked independently.
* A cached program with a nonexistent interpreter fails exec without a start acknowledgement or LRU timestamp change.
* A job ignoring SIGTERM is killed/reaped after the two-second migration grace period and reports JOB_STOPPED after directory cleanup.
* Local drain during a running checkpointed job sends DRAINING, continues heartbeats, waits for successful job completion and then exits.
* A restarted cache reports inventory after its due heartbeat, with the cache lock and per-worker directories preserved.
* Cache tests now include a 16,421-byte duplicate transfer crossing the chunk boundary and a mismatch after byte 16,384, alongside LRU/pinning/restart/missing-reference cases.

Run `make all test test-worker test-worker-discovery` on Linux/macOS. The workload fixtures (including the new stream fixture) build without sanitizers so address-space limits remain meaningful. Focused supervisor/cache sanitizer builds use AddressSanitizer and UndefinedBehaviorSanitizer; root worker tests exercise privilege dropping to nobody. Source support for macOS remains present but must be executed on a Mac; loopback tests do not replace separate-machine/hotspot tests.

Verified on WSL Ubuntu with normal and root execution: the full scheduler/worker regression suites, one/two-worker migration, the new output/grace/drain/control cases, and 49 cache checks. Focused worker/cache AddressSanitizer/UndefinedBehaviorSanitizer runs pass. In the final normal load run, 64 actual CLI processes completed 256 jobs across four workers in 3.56 seconds; cancellation, four simultaneous 1 MiB output jobs and checkpoint migration also completed. This is one bounded local measurement, not a hardware-independent performance guarantee. The short-job-versus-continuous-output regression reported the short job in approximately 0.10 seconds while the producer was still running.

## Final repo handoff

Production changes are confined to `src/worker/main.c`, `network.c`, `node.h`, and `cache.c`. Merge the Makefile additions and include `tests/programs/stream.c`, `tests/test_worker_cache.c`, and `tests/test_worker_network.py` for reproducible verification. Existing worker/config/runner/platform files and shared interfaces remain required, as described in worker-integration.md.

Member 3 must still connect full executable/cache-reference selection, cache-miss retransmission, cryptographic identity and client services. This patch changes no client commands, cron service or scheduler policy, and cannot mark those features complete.
