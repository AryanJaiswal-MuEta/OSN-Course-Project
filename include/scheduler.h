#ifndef OSN_SCHEDULER_H
#define OSN_SCHEDULER_H

#include "model.h"

enum sched_err {
    S_OK=0,S_BAD_INPUT,S_NO_MEMORY,S_NOT_FOUND,S_BAD_STATE,
    S_NO_RESOURCE,S_QUEUE_FULL,S_STALE_RUN,S_BUSY,S_ID_LIMIT,S_BUFFER_SMALL
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

// Network dispatch mein pehle reserve, phir actual start ki confirmation.
// Sirf assignment bhejne se job RUNNING nahi banegi.
int job_reserve(struct scheduler *s,int id,int worker_id,uint64_t now,uint64_t *run_id);
int job_confirm_start(struct scheduler *s,int id,int worker_id,uint64_t run_id,uint64_t now);
int job_abort_dispatch(struct scheduler *s,int id,int worker_id,uint64_t run_id,
                       const char *reason,uint64_t now);
// Worker ke terminal launch failure ke baad hi call karo.
int job_fail_dispatch(struct scheduler *s,int id,int worker_id,uint64_t run_id,
                      const char *reason,uint64_t now);

// Local driver ka shortcut: same call mein reserve aur confirm. Network
// send ke baad yeh shortcut mat use karna; confirmation abhi aani baaki hai.
int job_start(struct scheduler *s,int id,int worker_id,uint64_t now,uint64_t *run_id);

// Worker ka stop/exit confirm hone ke baad hi result apply karna.
// Purane run ka result aaya toh reservation dobara release nahi honi chahiye.
int job_finish(struct scheduler *s,int id,uint64_t run_id,
               enum job_state state,const char *reason,uint64_t now);
int job_cancel_pending(struct scheduler *s,int id,const char *reason,uint64_t now);

enum sched_policy { SCHED_FIFO_ORDER,SCHED_PRIORITY_ORDER };
struct sched_pick {
    int job_id;
    int worker_id;
};

// Sirf next candidate dhoondo. Caller reserve karega, phir dobara pick karega.
int sched_pick_next(const struct scheduler *s,enum sched_policy policy,
                    uint64_t now,struct sched_pick *out);

struct sched_dep { int id; int group; };
void sched_note(struct scheduler *s,const char *fmt,...);
int sched_priority(const struct job *j,uint64_t now);
int sched_quota_skip(const struct scheduler *s,const struct job *j);
const struct worker *sched_fit(const struct scheduler *s,const struct job *j);
int sched_job_eligible(const struct scheduler *s,const struct job *j);
int job_add_deps(struct scheduler *s,const struct job_req *req,
                 const struct sched_dep *deps,size_t count,uint64_t now,int *id);
void sched_dependencies(struct scheduler *s,uint64_t now);
void sched_release_holds(struct scheduler *s,int id);
int sched_heartbeat(struct scheduler *s,int worker_id,double load,uint64_t now);
int sched_worker_dead(struct scheduler *s,int worker_id,uint64_t now);
int sched_tick(struct scheduler *s,uint64_t now);
int sched_cancel(struct scheduler *s,int id,uint64_t now);
int sched_migrate(struct scheduler *s,int id,uint64_t now);
int sched_preempt(struct scheduler *s,int id,uint64_t now,int *worker_id);
int sched_preempt_next(struct scheduler *s,enum sched_policy policy,uint64_t now);
int sched_report(struct scheduler *s,int worker_id,int id,uint64_t run_id,
                  enum job_state state,const struct telemetry *usage,
                  const char *reason,uint64_t now);
int sched_checkpoint(struct scheduler *s,int worker_id,int id,uint64_t run_id,
                      uint64_t seq,const void *data,size_t size,uint64_t now);
int sched_cache_set(struct scheduler *s,int worker_id,const char *key,int present);
int sched_content_set(struct scheduler *s,int id,const char *key);
int sched_cached(const struct worker *w,const char *key);

#endif
