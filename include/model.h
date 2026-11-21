#ifndef OSN_MODEL_H
#define OSN_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define USER_LEN 32
#define PATH_LEN 512
#define QUEUE_CAP 1024
#define MAX_ATTEMPTS 3
#define CONTENT_LEN 129
#define CHECKPOINT_CAP (512u*1024u)

enum sched_stop { S_STOP_NONE,S_STOP_CANCEL,S_STOP_PREEMPT,S_STOP_MIGRATE };
struct cache_entry {
    char key[CONTENT_LEN];
    struct cache_entry *next;
};

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
    int dispatch_pending;
    uint64_t dispatch_ms;
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
    int stop_sent;
    uint64_t stop_ms;
    int preempt_worker;
    char content[CONTENT_LEN];

    // Dependency IDs submit time pe snapshot hain; baad ke group members nahi judenge.
    int *deps;
    size_t dep_count;
    struct telemetry usage;
    unsigned char *checkpoint;
    size_t checkpoint_size;
    uint64_t checkpoint_no;
    uint64_t checkpoint_seq;
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
    size_t reserved_jobs;
    uint64_t heartbeat_ms;
    double load;
    int preempt_for;
    struct cache_entry *cache;
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
    // Callback sirf log/event publish kare; iske andar scheduler mutate mat karo.
    void (*event)(const char *msg,void *arg);
    void *event_arg;
};

#endif
