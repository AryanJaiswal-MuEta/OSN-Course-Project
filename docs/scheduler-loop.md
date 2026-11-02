# Scheduler loop and service boundary — 28 October

Today's Member 1 scope is the poll-loop foundation, admission/query boundary and logging hooks. This is a library addition, not yet a listening TCP scheduler or a scheduling-policy implementation.

## Integration with the current repository

Work started by fetching and fast-forwarding to `e1d4704`, which contains the team's protocol definitions/networking skeleton. The existing `include/protocol.h` and all common transport/discovery/logging files are unchanged by this work.

The common implementations are still empty, so this change does not invent another wire format or implement their functions. Member 3 should decode requests using the shared protocol, call the service functions, and serialize the returned values. The protocol currently has no nodes/status/login message types; those still need agreement before connecting a real client.

## New project files

```text
include/scheduler_loop.h
include/scheduler_service.h
src/scheduler/loop.c
src/scheduler/service.c
```

`scheduler.h` also adds the `S_BUFFER_SMALL` error, `lifecycle.c` supplies its text, and the Makefile adds the modules to `libscheduler.a`. Merge those small edits as well when moving work to another repo. Existing model layouts and runner files are unchanged.

## Poll loop

`sched_loop_init()` attaches a caller-owned scheduler and callbacks. `sched_loop_add()` watches a descriptor for POLLIN/POLLOUT and sets O_NONBLOCK/FD_CLOEXEC. Add a TCP listener, accepted sockets, a discovery socket or an internal wakeup descriptor when their owner modules are ready.

The callbacks are:

- `io(loop,fd,events,arg)`: handle readiness, including POLLERR/POLLHUP/POLLNVAL.
- `tick(loop,now,arg)`: periodic work, roughly once per second.
- `schedule(loop,now,arg)`: one coalesced scheduling opportunity after I/O and periodic callbacks. The actual FIFO/PRIORITY selection is still pending.

`sched_loop_request()` marks scheduling needed. Requests coalesce into one scheduling callback per step; the loop also requests scheduling on the one-second timer. Initialization requests an initial pass. Call `sched_loop_step()` to embed this loop or `sched_loop_run()` to run until stopped. A step returns 0 on normal completion or -1 with errno on error; its return value is not a ready-fd count.

Hooks must do bounded, nonblocking work. For a listener, accept until EAGAIN or a per-step budget; for connections, feed the decoder and buffer outgoing data. Enable POLLOUT only while bytes are queued, then disable it after flushing; otherwise writable sockets will make the loop spin. Remove a descriptor before closing it. Callbacks may add/remove/update watches, and registrations carry generation tags so a stale readiness event cannot be applied to a replacement with the same fd number.

The loop neither closes descriptors nor frees the scheduler. Stop it, unregister/close caller-owned sockets, call `sched_loop_free()`, and finally `sched_free()`. Do not initialize/free the loop from a callback. Reentrant stepping is rejected. Stop called inside a callback takes effect before later callbacks; a normal signal-interrupted poll returns control as well. This is a single-owner loop, not a thread-safe executor; cross-thread work will need a pipe/event notification. Callbacks that block can still delay the whole loop.

Timers use monotonic milliseconds and are best-effort at the next step. There is no background thread or hard real-time guarantee. Calls from another thread are unsupported; register a dedicated wakeup fd and process the notification on the owner thread. A signal handler may set the stop flag; the interrupted poll or the next timer wakeup lets the loop observe it.

## Admission/query service

Initialize a `sched_service` with the registry and optional changed/log hooks:

```c
static void changed(void *arg)
{
    sched_loop_request(arg);
}

/* log hook ko Member 3 ke shared logger wrapper se connect karna hai */
sched_service_init(&svc,&s,changed,log_hook,&loop);
```

Both hooks receive the same arg; use a connection/service context containing the loop/logger if they need more than one value.

`sched_submit()` takes the session's validated username separately from the request. It ignores the request's user field, copies the correct owner, and uses the existing bounded `job_add()` admission. Success requests scheduling and logs the ID/owner. Admission failure logs the acting user and reason without triggering scheduling. No asynchronous event format or authentication scheme is introduced here.

`sched_status_get()` copies a pointer-free status snapshot: owner/request, state, assignment, execution ID, telemetry, checkpoint metadata and reason. Data for unfinished features remains at the current initialized values.

`sched_nodes_get()` copies worker ID/state, free/total cores/RAM/GPUs and running-job count. Call with NULL/0 to discover the required count. If workers exist it returns S_BUFFER_SMALL; allocate that many entries and retry. An undersized buffer receives no partial list. An empty registry permits NULL/0 and returns S_OK. Allocation and serialization belong to the caller.

Queries do not call scheduling, wait on queue capacity, or return registry-owned linked-list pointers. All calls still belong on the scheduler owner thread, including both sizing and filling the nodes buffer. Do not use the service as a thread-safe API from arbitrary client threads.

Log-hook strings contain the `[SCHEDULER]` component tag but no timestamp. A transport/logging wrapper should add the assignment's `[HH:MM:SS]` timestamp once; do not add the tag twice. For now hooks cover submission outcomes, not all future worker/job transitions. Full lifecycle event/log integration remains pending.

## Verification

`make test` now also runs selectable `test_loop` and `test_service` groups. They use actual nonblocking socket pairs for readiness/backpressure and an injected timer callback for observing periodic scheduling; they do not claim to test the unfinished wire protocol.

Coverage includes watch growth, registration flags, same-number remove/re-add, hangup removal, reentrant-step rejection, timer execution, coalesced submit wakeup, query snapshot isolation, session-owner selection and queries under full admission capacity. The slow-peer test fills one connection's send buffer and verifies another connection can invoke the nodes query.

Verified on WSL Ubuntu: five loop groups (119 checks), two service groups (1061 checks), all existing scheduler/worker groups and both driver suites pass. The complete suite also passes with AddressSanitizer and UndefinedBehaviorSanitizer. No new production network protocol is assumed by these tests.

Still pending: listener/CLI integration, UDP discovery, worker registration handlers, reservation/start acknowledgement split, FIFO/PRIORITY/aging, heartbeat recovery and production client services. macOS execution is not verified here.
