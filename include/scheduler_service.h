#ifndef OSN_SCHEDULER_SERVICE_H
#define OSN_SCHEDULER_SERVICE_H

#include "scheduler.h"

struct sched_service {
    struct scheduler *s;
    void (*changed)(void *arg);
    void (*log)(const char *msg,void *arg);
    void *arg;
};

struct sched_node {
    int id;
    enum worker_state state;
    struct resource total;
    struct resource free;
    size_t jobs;
};

struct sched_status {
    int id;
    struct job_req req;
    enum job_state state;
    int worker_id;
    int last_worker;
    uint64_t run_id;
    unsigned int failures;
    struct telemetry usage;
    uint64_t checkpoint_no;
    size_t checkpoint_size;
    uint64_t checkpoint_ms;
    char reason[160];
};

int sched_service_init(struct sched_service *svc,struct scheduler *s,
                       void (*changed)(void *),void (*log)(const char *,void *),void *arg);
int sched_submit(struct sched_service *svc,const char *user,
                 const struct job_req *req,uint64_t now,int *id);
int sched_status_get(const struct sched_service *svc,int id,struct sched_status *out);
int sched_nodes_get(const struct sched_service *svc,struct sched_node *out,
                    size_t cap,size_t *count);

#endif
