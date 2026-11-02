# Scheduler worker registration boundary

Added on 2 November as a small Member 1 catch-up step from the 29 October plan. No network protocol or worker runtime behavior is changed.

`sched_register_worker(svc,total,now,&id)` is the service entry point for a decoded worker registration. It reuses the registry's validation and unique ID allocation, records capacity and the initial heartbeat timestamp, logs the outcome and calls the existing changed hook after success. The changed hook can request a scheduling pass. A rejected registration does not consume an ID or request scheduling.

The transport handler must send the returned ID back to the worker and later associate it with that connection. That response/session wiring is still pending. Repeated calls create new worker registrations; request replay/deduplication belongs to the connection layer and is not implemented by this API.

Capacity is cores, RAM in MB and GPU count. Physical capacity clamping belongs on the worker; the scheduler validates positive cores/RAM and nonnegative GPUs. Nodes snapshots expose the registered capacity immediately, with all resources free and zero running jobs.

Changed project files: `include/scheduler_service.h`, `src/scheduler/service.c`. Focused checks are in `tests/test_service.c` under the `registration` group. The function uses the service's existing logger callback; timestamp formatting belongs to the shared logger adapter.

Deferred to later sessions: connection registration replies, dispatch reservations/start acknowledgements, worker CLI/capacity reporting and scheduling policies. This is not a completed network-registration feature.
