#ifndef OSN_SCHEDULER_H
#define OSN_SCHEDULER_H

#include "model.h"

enum sched_err {
    S_OK=0,S_BAD_INPUT,S_NO_MEMORY,S_NOT_FOUND,S_BAD_STATE,
    S_NO_RESOURCE,S_QUEUE_FULL,S_STALE_RUN,S_BUSY,S_ID_LIMIT
};

void sched_init(struct scheduler *s);
void sched_free(struct scheduler *s);
const char *sched_error(int err);
const char *job_state_name(enum job_state state);
const char *worker_state_name(enum worker_state state);
int user_valid(const char *name);
int job_can_move(enum job_state from,enum job_state to);
int worker_can_move(enum worker_state from,enum worker_state to);

int worker_add(struct scheduler *s,struct resource total,uint64_t now,int *id);
struct worker *worker_find(struct scheduler *s,int id);
int worker_set_state(struct scheduler *s,int id,enum worker_state state);
int worker_fits(const struct worker *w,struct resource need);
struct resource worker_free(const struct worker *w);

int job_add(struct scheduler *s,const struct job_req *req,uint64_t now,int *id);
struct job *job_find(struct scheduler *s,int id);

// Yeh policy selector nahi hai. Abhi test driver worker choose karega;
// FIFO/PRIORITY module baad mein isi lifecycle ko call karega.
int job_start(struct scheduler *s,int id,int worker_id,uint64_t now,uint64_t *run_id);

// Worker ka stop/exit confirm hone ke baad hi result apply karna.
// Purane run ka result aaya toh reservation dobara release nahi honi chahiye.
int job_finish(struct scheduler *s,int id,uint64_t run_id,
               enum job_state state,const char *reason,uint64_t now);
int job_cancel_pending(struct scheduler *s,int id,const char *reason,uint64_t now);

#endif
