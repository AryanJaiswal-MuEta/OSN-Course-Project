#include <stdio.h>
#include "scheduler.h"

int job_reserve(struct scheduler *s,int id,int worker_id,uint64_t now,uint64_t *run_id)
{
    struct job *j=job_find(s,id);
    struct worker *w=worker_find(s,worker_id);
    if(!s||!run_id) return S_BAD_INPUT;
    if(!j||!w) return S_NOT_FOUND;
    if(j->state!=QUEUED||j->dispatch_pending||w->state!=ALIVE) return S_BAD_STATE;
    if(!worker_fits(w,j->req.need)) return S_NO_RESOURCE;
    if(now<j->queue_ms) return S_BAD_INPUT;
    if(s->next_run==UINT64_MAX) return S_ID_LIMIT;

    // Transfer/prepare chal raha ho tab bhi cores, RAM aur GPU busy hain.
    // Doosri assignment ko yehi resources dobara nahi milne chahiye.
    w->used.cores+=j->req.need.cores;
    w->used.ram+=j->req.need.ram;
    w->used.gpu+=j->req.need.gpu;
    w->reserved_jobs++;
    j->worker_id=w->id;
    j->run_id=s->next_run++;
    j->dispatch_pending=1;
    j->dispatch_ms=now;
    j->reason[0]='\0';
    *run_id=j->run_id;
    // Pending queue ka slot abhi occupied rakho. Prepare fail hua toh job
    // ko full queue mein reinsert karne ki problem hi nahi aayegi.
    return S_OK;
}

static int pending_run(struct scheduler *s,int id,int worker_id,uint64_t run_id,
                        struct job **job,struct worker **worker)
{
    struct job *j=job_find(s,id);
    struct worker *w;
    if(!s) return S_BAD_INPUT;
    if(!j) return S_NOT_FOUND;
    if(!run_id||run_id!=j->run_id||worker_id!=j->worker_id) return S_STALE_RUN;
    if(j->state!=QUEUED||!j->dispatch_pending) return S_BAD_STATE;
    w=worker_find(s,worker_id);
    if(!w) return S_NOT_FOUND;
    *job=j;
    *worker=w;
    return S_OK;
}

int job_confirm_start(struct scheduler *s,int id,int worker_id,uint64_t run_id,uint64_t now)
{
    struct job *j;
    struct worker *w;
    int err=pending_run(s,id,worker_id,run_id,&j,&w);
    if(err!=S_OK) return err;
    if(w->state==DEAD) return S_BAD_STATE;
    if(now<j->dispatch_ms) return S_BAD_INPUT;
    // Worker ka actual start confirm ho gaya. Reserved resources already
    // used mein hain, isliye yahan unko add nahi karna hai.
    w->reserved_jobs--;
    w->jobs++;
    s->pending--;
    j->wait_ms+=now-j->queue_ms;
    j->start_ms=now;
    j->last_worker=worker_id;
    j->dispatch_pending=0;
    j->state=RUNNING;
    return S_OK;
}

int job_abort_dispatch(struct scheduler *s,int id,int worker_id,uint64_t run_id,
                       const char *reason,uint64_t now)
{
    struct job *j;
    struct worker *w;
    int err=pending_run(s,id,worker_id,run_id,&j,&w);
    if(err!=S_OK) return err;
    if(now<j->dispatch_ms) return S_BAD_INPUT;
    // Caller ne confirm kiya ho ki remote execution start nahi hui, ya
    // stop ho chuki hai. Sirf socket send error pe resources release mat karo.
    w->used.cores-=j->req.need.cores;
    w->used.ram-=j->req.need.ram;
    w->used.gpu-=j->req.need.gpu;
    w->reserved_jobs--;
    j->worker_id=0;
    j->dispatch_pending=0;
    j->dispatch_ms=0;
    snprintf(j->reason,sizeof(j->reason),"%s",reason?reason:"");
    return S_OK;
}
