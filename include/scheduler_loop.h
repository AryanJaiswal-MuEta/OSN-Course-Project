#ifndef OSN_SCHEDULER_LOOP_H
#define OSN_SCHEDULER_LOOP_H

#include <poll.h>
#include <signal.h>
#include "scheduler.h"

struct sched_loop;

struct sched_hooks {
    void (*io)(struct sched_loop *loop,int fd,short events,void *arg);
    void (*tick)(struct sched_loop *loop,uint64_t now,void *arg);
    void (*schedule)(struct sched_loop *loop,uint64_t now,void *arg);
};

struct sched_watch {
    int fd;
    short events;
    uint64_t tag;
};

struct sched_loop {
    struct scheduler *s;
    struct sched_watch *watch;
    size_t count;
    size_t cap;
    uint64_t next_tag;
    uint64_t next_tick;
    struct sched_hooks hooks;
    void *arg;
    volatile sig_atomic_t stop;
    int dirty;
    int stepping;
};

int sched_loop_init(struct sched_loop *loop,struct scheduler *s,
                    const struct sched_hooks *hooks,void *arg);
int sched_loop_add(struct sched_loop *loop,int fd,short events);
int sched_loop_update(struct sched_loop *loop,int fd,short events);
int sched_loop_remove(struct sched_loop *loop,int fd);
void sched_loop_request(struct sched_loop *loop);
void sched_loop_stop(struct sched_loop *loop);
int sched_loop_step(struct sched_loop *loop,int wait_ms);
int sched_loop_run(struct sched_loop *loop);
void sched_loop_free(struct sched_loop *loop);
uint64_t sched_now_ms(void);

#endif
