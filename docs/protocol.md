### This markdown file is the documentation for the protocol used in the given codebase.It will include the bytes breakdown of protocol messages, their structure, and any relevant details for other team members to interact with the protocol effectively.


## 1. Design Rules

- **No raw C structs**: Do not use `sizeof(struct ...)` or `send(&struct, ...)`. Different compilers, OS (macOS vs Linux), and architectures add padding or alignment differences. All packets use explicit big-endian byte serialization.
- **Big Endian byte order**: All multi-byte fields (16-bit, 32-bit, 64-bit) are packed in network byte order (most significant byte first).
- **Non-blocking TCP streaming**: Sockets can read partial frames or multiple concatenated frames in one `recv()`. The `transport` decoder handles buffering until a full message is available.
- **Maximum accepted payload**: 1 MiB (`1024 * 1024` bytes).

---

## 2. TCP Frame Structure

Every message sent across TCP has a **16-byte fixed header** followed by variable payload bytes.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Magic (4 bytes)                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Version (2 bytes)       |      Message Type (2 bytes)   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Payload Length (4 bytes)                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Request ID (4 bytes)                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Payload bytes ... (N bytes)                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### Header Fields Breakdown:

| Offset | Size (bytes) | Field Name | Type | Value / Description |
|---|---|---|---|---|
| `0` | 4 | **Magic** | `uint32_t` | `0x47505436` (`PROTOCOL_MAGIC_NUMBER`) |
| `4` | 2 | **Version** | `uint16_t` | `1` (`PROTOCOL_VERSION`) |
| `6` | 2 | **Message Type** | `uint16_t` | Identifies the message operation (1 to 7) |
| `8` | 4 | **Payload Length** | `uint32_t` | Exact number of payload bytes following the header |
| `12` | 4 | **Request ID** | `uint32_t` | Nonzero ID correlating requests and replies |

---

## 3. Message Types (`enum protocol_type`)

| Value | Name | Direction | Description |
|---|---|---|---|
| `1` | `WORKER_REGISTER` | Worker $\rightarrow$ Scheduler | Advertises worker capacity (cores, RAM, GPU) |
| `2` | `JOB_SUBMIT` | Client $\rightarrow$ Scheduler | Submits a new job request |
| `3` | `JOB_ASSIGN` | Scheduler $\rightarrow$ Worker | Commands worker to stage and run a job |
| `4` | `JOB_ACCEPTED` | Worker $\rightarrow$ Scheduler | Acknowledges receipt of job assignment |
| `5` | `JOB_COMPLETE` | Worker $\rightarrow$ Scheduler | Reports successful job completion with telemetry |
| `6` | `JOB_FAILED` | Worker $\rightarrow$ Scheduler | Reports job execution failure or termination |
| `7` | `ERROR` | Either $\rightarrow$ Other | Reports protocol or validation errors |

---

## 4. Payload Byte Breakdown per Message

### A. `WORKER_REGISTER` (`type = 1`)
Sent by the worker immediately after TCP connection opens.

| Field | Size (bytes) | Type | Description |
|---|---|---|---|
| `cores` | 4 | `uint32_t` | Clamped available CPU cores |
| `ram_mb` | 8 | `uint64_t` | Clamped available RAM in megabytes |
| `gpu` | 4 | `uint32_t` | Available GPU units |
| `name_len` | 2 | `uint16_t` | Length of optional worker name string (can be 0) |
| `worker_name` | `name_len` | `char[]` | Optional worker name bytes (no null terminator) |

Minimum payload size: **18 bytes** (when `name_len = 0`).

---

### B. `JOB_SUBMIT` (`type = 2`)
Sent by the client to submit a job.

| Field | Size (bytes) | Type | Description |
|---|---|---|---|
| `user_len` | 2 | `uint16_t` | Length of username string |
| `user` | `user_len` | `char[]` | Username characters |
| `path_len` | 2 | `uint16_t` | Length of executable path |
| `path` | `path_len` | `char[]` | Executable path string |
| `cores` | 4 | `uint32_t` | Required cores |
| `ram_mb` | 8 | `uint64_t` | Required RAM limit in MB |
| `gpu` | 4 | `uint32_t` | Required GPUs |
| `time_limit` | 4 | `uint32_t` | Maximum runtime in seconds |
| `priority` | 1 | `uint8_t` | Priority tier (1 = highest, 3 = lowest) |
| `group` | 4 | `uint32_t` | Optional group ID (0 if none) |
| `preemptible` | 1 | `uint8_t` | Flag (`1` = yes, `0` = no) |

---

### C. `JOB_ASSIGN` (`type = 3`)
Sent by the scheduler to dispatch a job to a worker.

| Field | Size (bytes) | Type | Description |
|---|---|---|---|
| `job_id` | 4 | `uint32_t` | Unique scheduler job ID |
| `run_id` | 4 | `uint32_t` | Execution token (changes per dispatch) |
| `cores` | 4 | `uint32_t` | Cores allocated |
| `ram_mb` | 8 | `uint64_t` | RAM limit in MB |
| `gpu` | 4 | `uint32_t` | GPUs allocated |
| `time_limit` | 4 | `uint32_t` | Runtime deadline in seconds |
| `path_len` | 2 | `uint16_t` | Length of binary path |
| `path` | `path_len` | `char[]` | Path to executable |
| `checkpoint_size` | 4 | `uint32_t` | Size of checkpoint state (0 if none) |
| `checkpoint` | `checkpoint_size` | `unsigned char[]` | Checkpoint state bytes to restore |

---

### D. `JOB_ACCEPTED` (`type = 4`)
Sent by worker back to scheduler confirming the assignment was accepted.

| Field | Size (bytes) | Type | Description |
|---|---|---|---|
| `job_id` | 4 | `uint32_t` | Matches assigned job ID |
| `run_id` | 4 | `uint32_t` | Matches assigned execution token |

Total payload size: **8 bytes**.

---

### E. `JOB_COMPLETE` (`type = 5`) & `JOB_FAILED` (`type = 6`)
Sent by worker when a job finishes.

| Field | Size (bytes) | Type | Description |
|---|---|---|---|
| `job_id` | 4 | `uint32_t` | Finished job ID |
| `run_id` | 4 | `uint32_t` | Execution token |
| `peak_ram_mb` | 8 | `uint64_t` | Peak resident RAM used during execution |
| `faults` | 8 | `uint64_t` | Total major + minor page faults |
| `cpu_time_ms` | 8 | `uint64_t` | Total CPU time consumed in milliseconds |
| `reason_len` | 2 | `uint16_t` | Diagnostic message length (can be 0) |
| `reason` | `reason_len` | `char[]` | Diagnostic reason (e.g. exit code or signal) |

---

### F. `ERROR` (`type = 7`)
Sent when a request is rejected or frame validation fails.

| Field | Size (bytes) | Type | Description |
|---|---|---|---|
| `error_code` | 4 | `uint32_t` | Value from `enum protocol_error` |
| `msg_len` | 2 | `uint16_t` | Length of error description string |
| `message` | `msg_len` | `char[]` | Human readable error string |

---

## 5. UDP Discovery Packet Format

Discovery uses compact **16-byte fixed UDP packets** sent over broadcast to port `9999` (or custom `-discovery-port`).

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      Magic: 0x4f53444e                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Version (1)  |   Kind (1)    |          Port (2)             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Nonce (8 bytes)                        |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Offset | Size | Field | Description |
|---|---|---|---|
| `0` | 4 | `magic` | `0x4f53444e` (`OSDN` in ASCII) |
| `4` | 1 | `version` | `1` |
| `5` | 1 | `kind` | `1` = `DISCOVERY_REQUEST`, `2` = `DISCOVERY_RESPONSE` |
| `6` | 2 | `port` | In request: `0`. In response: scheduler's TCP listening port |
| `8` | 8 | `nonce` | Random integer so the requester matches its own response |

### Discovery Flow:
1. Worker sends UDP broadcast: `DISCOVERY_REQUEST(port=0, nonce=X)`.
2. Scheduler receives and replies: `DISCOVERY_RESPONSE(port=8080, nonce=X)`.
3. Worker connects via TCP to `<scheduler_ip>:8080`.

---

## 6. Standard Error Codes (`enum protocol_error`)

Carried in `ERROR` messages:

| Code | Name | Description |
|---|---|---|
| `0` | `SUCCESS` | Operation succeeded |
| `1` | `INVALID_MESSAGE` | Malformed header, bad length, or payload truncated |
| `2` | `UNSUPPORTED_VERSION`| Packet version is not `1` |
| `3` | `UNKNOWN_MESSAGE_TYPE`| Message type not in range `1..7` |
| `4` | `INVALID_JOB_ID` | Referenced job does not exist in scheduler registry |
| `5` | `WORKER_UNAVAILABLE` | Worker is DEAD or capacity exhausted |
| `6` | `PERMISSION_DENIED` | Action not permitted for this user |
