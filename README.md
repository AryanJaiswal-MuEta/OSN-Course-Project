# courseproject

Member 1 scheduler build: `make build/scheduler` on Linux/macOS.
Run: `./build/scheduler 8080 PRIORITY` (or `FIFO`).
Checks: `make test-scheduler` and `make test`.

See [scheduler handoff and worker protocol](docs/scheduler-integration.md) for completed scheduler features, integration APIs and remaining worker/client service connections.

Member 2 worker build: `make build/worker`; checks: `make test-worker`.
See [worker handoff and transfer protocol](docs/worker-integration.md) for execution, cache, checkpoints, tests and the remaining Member 3 service connections.

