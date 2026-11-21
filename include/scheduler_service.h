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
    size_t reserved_jobs;
    double load;
    uint64_t heartbeat_ms;
    int dispatch_blocked;
};

struct sched_status {
    int id;
    struct job_req req;
    enum job_state state;
    int worker_id;
    int last_worker;
    uint64_t run_id;
    int dispatch_pending;
    uint64_t dispatch_ms;
    unsigned int failures;
    int stop_pending;
    int hold_for;
    int preempt_worker;
    int migrated_from;
    struct telemetry usage;
    uint64_t checkpoint_no;
    size_t checkpoint_size;
    uint64_t checkpoint_ms;
    uint64_t submit_ms,start_ms,end_ms,wait_ms;
    size_t dep_count,output_size;
    char reason[160];
};

int sched_service_init(struct sched_service *svc,struct scheduler *s,
                       void (*changed)(void *),void (*log)(const char *,void *),void *arg);
int sched_register_worker(struct sched_service *svc,struct resource total,
                          uint64_t now,int *id);
int sched_submit(struct sched_service *svc,const char *user,
                 const struct job_req *req,uint64_t now,int *id);
int sched_submit_deps(struct sched_service *svc,const char *user,
                      const struct job_req *req,const struct sched_dep *deps,
                      size_t count,uint64_t now,int *id);
int sched_status_get(const struct sched_service *svc,int id,struct sched_status *out);
int sched_nodes_get(const struct sched_service *svc,struct sched_node *out,
                    size_t cap,size_t *count);

struct sched_output_info {
    size_t size;
    uint64_t run_id,current_run_received;
    enum job_state state;
};
struct sched_checkpoint_info {
    size_t size;
    uint64_t number,received_ms,run_id;
};
int sched_status_owned(const struct sched_service *svc,const char *user,int id,struct sched_status *out);
int sched_cancel_owned(struct sched_service *svc,const char *user,int id,uint64_t now);
int sched_migrate_owned(struct sched_service *svc,const char *user,int id,uint64_t now);
int sched_output_get(const struct sched_service *svc,const char *user,int id,
                     void *buf,size_t cap,struct sched_output_info *out);
int sched_checkpoint_get(const struct sched_service *svc,const char *user,int id,
                         void *buf,size_t cap,struct sched_checkpoint_info *out);

#endif
