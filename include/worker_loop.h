#ifndef OSN_WORKER_LOOP_H
#define OSN_WORKER_LOOP_H

#include <poll.h>
#include <signal.h>
#include "runner.h"

#define WORKER_CONN 0
#define WORKER_ADMIN 1

struct worker_loop;

struct worker_result {
    int id;
    uint64_t run_id;
    enum job_state state;
    struct telemetry usage;
    char reason[160];
};

struct worker_hooks {
    void (*io)(struct worker_loop *loop,int fd,short events,void *arg);
    void (*tick)(struct worker_loop *loop,uint64_t now,void *arg);
    output_fn output;
    int (*result)(const struct worker_result *result,void *arg);
};

struct worker_watch {
    int fd;
    short events;
    uint64_t tag;
};

struct worker_loop {
    struct runner runner;
    struct worker_watch watch[2];
    struct worker_hooks hooks;
    void *arg;
    uint64_t next_poll;
    uint64_t next_tag;
    volatile sig_atomic_t stop;
    int stepping;
};

int worker_loop_init(struct worker_loop *loop,const struct worker_hooks *hooks,void *arg);
int worker_loop_watch(struct worker_loop *loop,int slot,int fd,short events);
int worker_loop_assign(struct worker_loop *loop,int id,uint64_t run_id,
                       const struct job_req *req,char *const args[]);
int worker_loop_cancel(struct worker_loop *loop,int id,uint64_t run_id);
int worker_loop_step(struct worker_loop *loop,int wait_ms);
int worker_loop_run(struct worker_loop *loop);
void worker_loop_stop(struct worker_loop *loop);
void worker_loop_free(struct worker_loop *loop);

#endif
