# Remaining work — 14 November 2026

**21 November update:** Member 1's scheduler implementation and adapter are now covered by the [scheduler handoff](scheduler-integration.md). The table below is historical; worker execution message handling, Member 3 services and joint release checks are still pending and are not marked complete by scheduler fixture tests.

This supersedes the dates in remaining-plan.md. Target: a tested package on 30 November; official deadline 2 December. Estimates are focused hours across Members 1 and 2 combined, separate from Member 3's effort.

Today: restored the dispatch and worker start-confirmation builds after the sync, and added FIFO and PRIORITY selection with 10-second aging. The selector returns one job/worker pair without taking a reservation. The scheduler adapter should reserve that pair before choosing another; pending reservations are skipped by selection. The selector picks the first eligible worker with enough resources; load and cache preferences come later. Production network wiring is not included.

| Target | Member 1 work | Member 2 work | Combined effort |
|---|---|---|---:|
| 15–18 Nov | Wire policy and dispatch to scheduler session handlers; handle failed launch without marking RUNNING | Wire worker startup, registration, start, output and final-result callbacks | 16–20 h |
| 19–22 Nov | Heartbeat deadlines, worker-loss recovery/retries, accounting and load-aware placement | Heartbeats, telemetry integration, reconnect cleanup, cancellation and drain | 17–21 h |
| 23–26 Nov | Dependencies, quotas, preemption and cache-aware placement | Preemption stop/reap and executable cache/LRU | 20–25 h |
| 27–28 Nov | Migration/requeue coordination | Checkpoint capture, restore and migration grace | 10–13 h |
| 29–30 Nov | Clean builds, cross-platform and network regressions, docs and packaging | Joint Linux/macOS, sudo and hotspot verification | 10–12 h |

Remaining: **73–91 focused hours** before contingency. At 5–6 hours per day from 15–30 November, there are 80–96 theoretical hours. The high end leaves essentially no buffer and assumes independent progress from Member 3. These are estimates, not promised dates.

Member 3's first gate is usable framing/transport and a jointly verified TCP job by 18 November. Agree on payloads for registration, exec confirmation, failed launch, output and result. Later dependencies include client commands/events, executable transfer, log following, cron, cache inventory and checkpoint storage/transport. Do not count local demos as end-to-end tests.

If the 18 November gate slips, redistribute integration immediately. Keep 29–30 November reserved for verification. Treat migration and advanced features as at risk rather than compressing testing; record incomplete requirements honestly. Keep 1 December as emergency buffer.
