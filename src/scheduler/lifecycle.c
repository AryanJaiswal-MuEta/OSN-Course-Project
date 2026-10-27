#include <stdio.h>
#include "scheduler.h"

const char *sched_error(int err)
{
    switch(err){
    case S_OK: return "OK";
    case S_BAD_INPUT: return "invalid input";
    case S_NO_MEMORY: return "memory allocation failed";
    case S_NOT_FOUND: return "job or worker not found";
    case S_BAD_STATE: return "operation not allowed in this state";
    case S_NO_RESOURCE: return "worker does not have enough free resources";
    case S_QUEUE_FULL: return "QUEUE_FULL";
    case S_STALE_RUN: return "result belongs to an older execution";
    case S_BUSY: return "worker still has active jobs";
    case S_ID_LIMIT: return "ID range exhausted";
    default: return "unknown error";
    }
}

const char *job_state_name(enum job_state state)
{
    switch(state){
    case WAITING: return "WAITING";
    case QUEUED: return "QUEUED";
    case RUNNING: return "RUNNING";
    case COMPLETED: return "COMPLETED";
    case FAILED: return "FAILED";
    case CANCELLED: return "CANCELLED";
    case LOST: return "LOST";
    case FAILED_PERMANENT: return "FAILED_PERMANENT";
    default: return "UNKNOWN";
    }
}

const char *worker_state_name(enum worker_state state)
{
    switch(state){
    case ALIVE: return "ALIVE";
    case SUSPECTED: return "SUSPECTED";
    case DEAD: return "DEAD";
    case DRAINING: return "DRAINING";
    default: return "UNKNOWN";
    }
}

int job_can_move(enum job_state from,enum job_state to)
{
    // Yeh allowed edges ka reference hai, direct state-write API nahi.
    // RUNNING se QUEUED sirf confirmed preemption/migration pe allowed hoga.
    switch(from){
    case WAITING: return to==QUEUED||to==CANCELLED;
    case QUEUED: return to==RUNNING||to==CANCELLED;
    case RUNNING:
        return to==COMPLETED||to==FAILED||to==CANCELLED||to==LOST||to==QUEUED;
    case LOST: return to==QUEUED||to==FAILED_PERMANENT;
    default: return 0;
    }
}

int worker_can_move(enum worker_state from,enum worker_state to)
{
    switch(from){
    case ALIVE: return to==SUSPECTED||to==DEAD||to==DRAINING;
    case SUSPECTED: return to==ALIVE||to==DEAD||to==DRAINING;
    case DRAINING: return to==DEAD;
    default: return 0;
    }
}

struct resource worker_free(const struct worker *w)
{
    struct resource free_res={0,0,0};
    if(!w) return free_res;
    free_res.cores=w->total.cores-w->used.cores;
    free_res.ram=w->total.ram-w->used.ram;
    free_res.gpu=w->total.gpu-w->used.gpu;
    return free_res;
}

int worker_fits(const struct worker *w,struct resource need)
{
    struct resource free_res;
    if(!w||w->state!=ALIVE||need.cores<=0||need.ram<=0||need.gpu<0) return 0;
    free_res=worker_free(w);
    return need.cores<=free_res.cores&&need.ram<=free_res.ram&&need.gpu<=free_res.gpu;
}

int worker_set_state(struct scheduler *s,int id,enum worker_state state)
{
    struct worker *w=worker_find(s,id);
    if(!w) return S_NOT_FOUND;
    if(!worker_can_move(w->state,state)) return S_BAD_STATE;

    // Active worker ko DEAD mark karke jobs orphan nahi kar sakte.
    // Recovery module mein LOST/requeue ke saath yeh atomic flow add hoga.
    if(state==DEAD&&w->jobs) return S_BUSY;
    w->state=state;
    return S_OK;
}

int job_start(struct scheduler *s,int id,int worker_id,uint64_t now,uint64_t *run_id)
{
    struct job *j=job_find(s,id);
    struct worker *w=worker_find(s,worker_id);
    if(!s||!run_id) return S_BAD_INPUT;
    if(!j||!w) return S_NOT_FOUND;
    if(j->state!=QUEUED||w->state!=ALIVE) return S_BAD_STATE;
    if(!worker_fits(w,j->req.need)) return S_NO_RESOURCE;
    if(now<j->queue_ms) return S_BAD_INPUT;
    if(s->next_run==UINT64_MAX) return S_ID_LIMIT;

    // Resource fit ke baad hi add karo. Isse signed addition bhi total
    // capacity ke andar rahegi aur failure pe half-updated record nahi bachega.
    w->used.cores+=j->req.need.cores;
    w->used.ram+=j->req.need.ram;
    w->used.gpu+=j->req.need.gpu;
    w->jobs++;
    s->pending--;
    j->wait_ms+=now-j->queue_ms;
    j->start_ms=now;
    j->worker_id=w->id;
    j->last_worker=w->id;
    j->run_id=s->next_run++;
    j->state=RUNNING;
    *run_id=j->run_id;
    return S_OK;
}

int job_finish(struct scheduler *s,int id,uint64_t run_id,
               enum job_state state,const char *reason,uint64_t now)
{
    struct job *j=job_find(s,id);
    struct worker *w;
    if(!j) return S_NOT_FOUND;
    if(!run_id||run_id!=j->run_id) return S_STALE_RUN;
    if(j->state!=RUNNING) return S_BAD_STATE;
    if(state!=COMPLETED&&state!=FAILED&&state!=CANCELLED) return S_BAD_STATE;
    if(now<j->start_ms) return S_BAD_INPUT;
    w=worker_find(s,j->worker_id);
    if(!w) return S_NOT_FOUND;

    // Result dobara aaya toh upar state check rok dega. RAM/GPU/cores
    // ek hi baar release honge; worker ka result aane se pehle call mat karna.
    w->used.cores-=j->req.need.cores;
    w->used.ram-=j->req.need.ram;
    w->used.gpu-=j->req.need.gpu;
    w->jobs--;
    j->state=state;
    j->worker_id=0;
    j->end_ms=now;
    snprintf(j->reason,sizeof(j->reason),"%s",reason?reason:"");
    return S_OK;
}

int job_cancel_pending(struct scheduler *s,int id,const char *reason,uint64_t now)
{
    struct job *j=job_find(s,id);
    if(!j) return S_NOT_FOUND;
    if(j->state!=QUEUED&&j->state!=WAITING) return S_BAD_STATE;
    if(now<j->submit_ms) return S_BAD_INPUT;
    s->pending--;
    j->state=CANCELLED;
    j->end_ms=now;
    snprintf(j->reason,sizeof(j->reason),"%s",reason?reason:"");
    return S_OK;
}
