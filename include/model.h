#ifndef OSN_MODEL_H
#define OSN_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define USER_LEN 32
#define PATH_LEN 512
#define QUEUE_CAP 1024
#define MAX_ATTEMPTS 3

enum job_state {
    WAITING,QUEUED,RUNNING,COMPLETED,FAILED,CANCELLED,LOST,FAILED_PERMANENT
};

enum worker_state { ALIVE,SUSPECTED,DEAD,DRAINING };

struct resource {
    int cores;
    long long ram; // MB mein rakho; bytes wala conversion worker karega
    int gpu;
};

struct job_req {
    char user[USER_LEN];
    char path[PATH_LEN];
    struct resource need;
    int time_limit;
    int prio;
    int group;
    int preemptible;
};

struct telemetry {
    long long ram;
    long long peak_ram;
    uint64_t faults;
    double cpu_time;
};

struct job {
    int id;
    struct job_req req;
    enum job_state state;
    int worker_id;
    int last_worker;
    uint64_t run_id;
    uint64_t submit_ms;
    uint64_t queue_ms;
    uint64_t wait_ms;
    uint64_t start_ms;
    uint64_t end_ms;

    // run_id har dispatch pe badlega; failure budget usse alag rakhna hai.
    unsigned int failures;
    int hold_for;
    int migrated_from;
    int stop_pending;

    // Abhi dependencies/checkpoints ka behaviour nahi hai, bas lifecycle
    // ke fields pehle se hain taaki baad mein har file rewrite na karni pade.
    int *deps;
    size_t dep_count;
    struct telemetry usage;
    unsigned char *checkpoint;
    size_t checkpoint_size;
    uint64_t checkpoint_no;
    uint64_t checkpoint_ms;
    char reason[160];
    struct job *next;
};

struct worker {
    int id;
    enum worker_state state;
    struct resource total;
    struct resource used;
    size_t jobs;
    uint64_t heartbeat_ms;
    double load;
    struct worker *next;
};

struct scheduler {
    struct job *jobs;
    struct job *job_tail;
    struct worker *workers;
    struct worker *worker_tail;
    size_t job_count;
    size_t worker_count;
    size_t pending;
    int next_job;
    int next_worker;
    uint64_t next_run;
};

#endif
