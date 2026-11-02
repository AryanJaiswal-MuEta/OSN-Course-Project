# Worker startup configuration — 2 November catch-up

This small Member 2 slice adds argument parsing and capacity detection/clamping from the missed 29 October plan. It does not implement discovery or connect to the scheduler.

Project files: `include/worker_config.h` and `src/worker/config.c`. Merge their Makefile additions to compile the module into `librunner.a`. Tests live in `tests/test_worker_config.c`.

`worker_config_parse()` accepts the assignment's startup argument layout:

```text
worker [scheduler_ip scheduler_port] -cores N -ram MB [-gpu N] [-discovery-port PORT]
```

Fallback IPv4 address/port, when present, precede flags. Required cores/RAM must be positive; GPUs default to zero and discovery port defaults to 9999. Flags can occur in any order. Unknown/duplicate flags, malformed values, missing values and invalid ports are rejected. The caller's configuration is unchanged on failure. An optional error buffer receives a short explanation.

`worker_capacity_detect()` reads online cores and physical RAM, returning RAM in whole MB. Linux uses sysconf; macOS uses sysconf for cores and sysctl for RAM. This is physical capacity, not currently available/free RAM or a container quota.

`worker_capacity_limit()` reduces requested cores/RAM only when they exceed physical capacity. It returns a bitmask (WORKER_CLAMP_CORES and/or WORKER_CLAMP_RAM), with zero for no changes, or -1 for invalid inputs. GPU is deliberately not clamped. The optional warning buffer describes reductions. The future worker startup must print that warning whenever clamping occurs.

The later startup path should parse, detect, clamp/report, attempt discovery first and use the stored address only after discovery fails. This change supplies helpers only; it does not change the existing development drivers or introduce a fake production worker executable. Registration, discovery and final CLI wiring remain separate work.

Verified on WSL Ubuntu: all four configuration test groups (63 checks), the currently configured regression suite, and a focused AddressSanitizer/UndefinedBehaviorSanitizer build of the configuration tests pass. macOS detection code is present but has not been run here.
