#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scheduler.h"

void sched_note(struct scheduler *s,const char *fmt,...)
{
    char buf[320];
    va_list args;
    if(!s||!s->event) return;
    va_start(args,fmt); vsnprintf(buf,sizeof(buf),fmt,args); va_end(args);
    s->event(buf,s->event_arg);
}

void sched_release_holds(struct scheduler *s,int id)
{
    struct job *j;
    struct worker *w;
    for(j=s->jobs;j;j=j->next){
        if(j->hold_for==id) j->hold_for=0;
        if(j->id==id) j->preempt_worker=0;
    }
    for(w=s->workers;w;w=w->next) if(w->preempt_for==id) w->preempt_for=0;
}

int sched_heartbeat(struct scheduler *s,int worker_id,double load,uint64_t now)
{
    struct worker *w=worker_find(s,worker_id);
    if(!w) return S_NOT_FOUND;
    if(!isfinite(load)||load<0||load>100||now<w->heartbeat_ms) return S_BAD_INPUT;
    if(w->state==DEAD) return S_BAD_STATE;
    w->heartbeat_ms=now; w->load=load;
    if(w->state==SUSPECTED){
        w->state=ALIVE;
        sched_note(s,"Worker %d SUSPECTED -> ALIVE",worker_id);
    }
    return S_OK;
}

static void release(struct worker *w,struct job *j)
{
    w->used.cores-=j->req.need.cores;
    w->used.ram-=j->req.need.ram;
    w->used.gpu-=j->req.need.gpu;
    if(j->dispatch_pending) w->reserved_jobs--;
    else w->jobs--;
    j->worker_id=0; j->dispatch_pending=0; j->dispatch_ms=0;
}

static void requeue(struct scheduler *s,struct job *j,uint64_t now)
{
    j->state=QUEUED; j->queue_ms=now; j->end_ms=0; j->stop_pending=S_STOP_NONE;
    memset(&j->usage,0,sizeof(j->usage));
    sched_note(s,"Job %d -> QUEUED: %s",j->id,j->reason);
}

int sched_worker_dead(struct scheduler *s,int worker_id,uint64_t now)
{
    struct worker *w=worker_find(s,worker_id);
    struct job *j;
    int pending,cancel;
    if(!w) return S_NOT_FOUND;
    if(w->state==DEAD) return S_BAD_STATE;
    if(now<w->heartbeat_ms) return S_BAD_INPUT;
    for(j=s->jobs;j;j=j->next)
        if(j->worker_id==worker_id&&now<(j->dispatch_pending?j->dispatch_ms:j->start_ms))
            return S_BAD_INPUT;
    w->state=DEAD;
    sched_note(s,"Worker %d -> DEAD",worker_id);
    if(w->preempt_for) sched_release_holds(s,w->preempt_for);
    for(j=s->jobs;j;j=j->next){
        if(j->worker_id!=worker_id||(!j->dispatch_pending&&j->state!=RUNNING)) continue;
        pending=j->dispatch_pending; cancel=j->stop_pending==S_STOP_CANCEL;
        if(pending) j->wait_ms+=now-j->queue_ms;
        release(w,j);
        if(cancel){
            if(pending) s->pending--;
            j->state=CANCELLED; j->end_ms=now; j->stop_pending=S_STOP_NONE;
            snprintf(j->reason,sizeof(j->reason),"cancelled; worker session lost");
            sched_release_holds(s,j->id);
            sched_note(s,"Job %d -> CANCELLED",j->id);
            continue;
        }
        j->state=LOST; j->failures++;
        sched_note(s,"Job %d -> LOST (failure %u/%d)",j->id,j->failures,MAX_ATTEMPTS);
        snprintf(j->reason,sizeof(j->reason),"worker session lost");
        if(j->failures>=MAX_ATTEMPTS){
            if(pending) s->pending--;
            j->state=FAILED_PERMANENT; j->end_ms=now; j->stop_pending=S_STOP_NONE;
            sched_release_holds(s,j->id);
            sched_note(s,"Job %d LOST -> FAILED_PERMANENT",j->id);
        }else{
            // Internal requeue QUEUE_CAP se reject nahi hoti. Fresh submit tab tak reject hoga.
            if(!pending) s->pending++;
            requeue(s,j,now);
        }
    }
    sched_dependencies(s,now);
    return S_OK;
}

int sched_tick(struct scheduler *s,uint64_t now)
{
    struct worker *w;
    uint64_t elapsed;
    int count=0;
    if(!s) return S_BAD_INPUT;
    for(w=s->workers;w;w=w->next){
        if(w->state==DEAD||now<w->heartbeat_ms) continue;
        elapsed=now-w->heartbeat_ms;
        if(elapsed>=6000){ sched_worker_dead(s,w->id,now); count++; }
        else if(elapsed>=2000&&w->state==ALIVE){
            w->state=SUSPECTED; count++;
            sched_note(s,"Worker %d ALIVE -> SUSPECTED",w->id);
        }
    }
    return count;
}

int sched_cancel(struct scheduler *s,int id,uint64_t now)
{
    struct job *j=job_find(s,id);
    if(!j) return S_NOT_FOUND;
    if(now<j->submit_ms) return S_BAD_INPUT;
    if((j->state==WAITING||j->state==QUEUED)&&!j->dispatch_pending)
        return job_cancel_pending(s,id,"cancelled by owner",now);
    if(j->state!=RUNNING&&!j->dispatch_pending) return S_BAD_STATE;
    if(j->stop_pending==S_STOP_CANCEL) return S_OK;
    // Reap confirmation se pehle resources busy hi rahenge.
    j->stop_pending=S_STOP_CANCEL;
    j->stop_ms=now;
    sched_release_holds(s,id);
    sched_note(s,"Job %d cancellation requested by %s",id,j->req.user);
    return S_OK;
}

int sched_migrate(struct scheduler *s,int id,uint64_t now)
{
    struct job *j=job_find(s,id);
    if(!j) return S_NOT_FOUND;
    if(j->state!=RUNNING||j->stop_pending) return S_BAD_STATE;
    if(now<j->start_ms) return S_BAD_INPUT;
    j->stop_pending=S_STOP_MIGRATE;
    j->stop_ms=now;
    sched_note(s,"Job %d migration requested by %s",id,j->req.user);
    return S_OK;
}

int sched_report(struct scheduler *s,int worker_id,int id,uint64_t run_id,
                  enum job_state state,const struct telemetry *usage,
                  const char *reason,uint64_t now)
{
    struct job *j=job_find(s,id);
    struct worker *w;
    int stop,pending,err;
    if(!j) return S_NOT_FOUND;
    if(!run_id||j->run_id!=run_id||j->worker_id!=worker_id) return S_STALE_RUN;
    if(j->state!=RUNNING&&!j->dispatch_pending) return S_BAD_STATE;
    if(state!=COMPLETED&&state!=FAILED&&state!=CANCELLED) return S_BAD_INPUT;
    if(now<(j->dispatch_pending?j->dispatch_ms:j->start_ms)) return S_BAD_INPUT;
    if(usage&&(usage->ram<0||usage->peak_ram<0||!isfinite(usage->cpu_time)||usage->cpu_time<0))
        return S_BAD_INPUT;
    stop=j->stop_pending; pending=j->dispatch_pending;
    if(!stop&&pending&&state==COMPLETED) return S_BAD_STATE;
    if(usage) j->usage=*usage;
    if(!stop){
        if(pending){
            // Exec start fail ka terminal report; RUNNING fake nahi karna.
            return job_fail_dispatch(s,id,worker_id,run_id,reason,now);
        }
        return job_finish(s,id,run_id,state,reason,now);
    }
    w=worker_find(s,worker_id);
    if(!w) return S_NOT_FOUND;
    if(stop==S_STOP_CANCEL){
        if(!pending) return job_finish(s,id,run_id,CANCELLED,"cancelled by owner",now);
        err=job_abort_dispatch(s,id,worker_id,run_id,"cancelled before start",now);
        if(err!=S_OK) return err;
        j->stop_pending=S_STOP_NONE;
        return job_cancel_pending(s,id,"cancelled by owner",now);
    }
    release(w,j);
    if(!pending) s->pending++;
    else j->wait_ms+=now-j->queue_ms;
    if(stop==S_STOP_MIGRATE) j->migrated_from=worker_id;
    snprintf(j->reason,sizeof(j->reason),"%s; restarting without failure charge",
             stop==S_STOP_MIGRATE?"migration stop confirmed":"preemption stop confirmed");
    requeue(s,j,now);
    return S_OK;
}

int sched_checkpoint(struct scheduler *s,int worker_id,int id,uint64_t run_id,
                      uint64_t seq,const void *data,size_t size,uint64_t now)
{
    struct job *j=job_find(s,id);
    unsigned char *ptr=NULL;
    if(!j) return S_NOT_FOUND;
    if(j->worker_id!=worker_id||!run_id||j->run_id!=run_id) return S_STALE_RUN;
    if(j->state!=RUNNING) return S_BAD_STATE;
    if(!seq||seq<=j->checkpoint_seq||j->checkpoint_no==UINT64_MAX||size>CHECKPOINT_CAP||(!data&&size)||now<j->start_ms)
        return S_BAD_INPUT;
    if(size){ ptr=malloc(size); if(!ptr) return S_NO_MEMORY; memcpy(ptr,data,size); }
    free(j->checkpoint); j->checkpoint=ptr; j->checkpoint_size=size;
    j->checkpoint_no++; j->checkpoint_seq=seq; j->checkpoint_ms=now;
    sched_note(s,"Job %d checkpoint %llu stored (%zu bytes)",id,(unsigned long long)j->checkpoint_no,size);
    return S_OK;
}
