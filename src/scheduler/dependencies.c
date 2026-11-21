#include <stdlib.h>
#include <stdio.h>
#include "scheduler.h"

static int bad(enum job_state state)
{
    return state==FAILED||state==FAILED_PERMANENT||state==CANCELLED;
}

int job_add_deps(struct scheduler *s,const struct job_req *req,
                 const struct sched_dep *deps,size_t count,uint64_t now,int *id)
{
    struct job *j,*ptr;
    int *ids=NULL,found,match,err;
    size_t i,n=0;
    if(!s||!req||!id||(!deps&&count)||count>QUEUE_CAP) return S_BAD_INPUT;
    // Sirf pehle se accepted jobs refer kar sakte hain, toh cycle ban hi nahi sakti.
    for(i=0;i<count;i++){
        if(deps[i].id<=0||(deps[i].group!=0&&deps[i].group!=1)) return S_BAD_INPUT;
        found=0;
        for(j=s->jobs;j;j=j->next)
            if(deps[i].group?j->req.group==deps[i].id:j->id==deps[i].id) found=1;
        if(!found) return S_NOT_FOUND;
    }
    if(count&&s->job_count){
        if(s->job_count>SIZE_MAX/sizeof(*ids)) return S_NO_MEMORY;
        ids=malloc(s->job_count*sizeof(*ids));
        if(!ids) return S_NO_MEMORY;
        for(j=s->jobs;j;j=j->next){
            match=0;
            for(i=0;i<count;i++)
                if(deps[i].group?j->req.group==deps[i].id:j->id==deps[i].id) match=1;
            if(match) ids[n++]=j->id;
        }
    }
    err=job_add(s,req,now,id);
    if(err!=S_OK){ free(ids); return err; }
    ptr=job_find(s,*id);
    ptr->deps=ids;
    ptr->dep_count=n;
    if(n){
        ptr->state=WAITING;
        sched_note(s,"Job %d -> WAITING: %zu dependencies",ptr->id,n);
        sched_dependencies(s,now);
    }
    return S_OK;
}

void sched_dependencies(struct scheduler *s,uint64_t now)
{
    struct job *j,*dep;
    size_t i;
    int ready,failed,changed;
    if(!s) return;
    // Iteration rakhi hai, recursive chain se stack overflow nahi hoga.
    do {
        changed=0;
        for(j=s->jobs;j;j=j->next){
            if(j->state!=WAITING) continue;
            ready=1; failed=0;
            for(i=0;i<j->dep_count;i++){
                dep=job_find(s,j->deps[i]);
                if(!dep||bad(dep->state)) failed=1;
                if(!dep||dep->state!=COMPLETED) ready=0;
            }
            if(failed){
                j->state=CANCELLED; j->end_ms=now; s->pending--;
                snprintf(j->reason,sizeof(j->reason),"dependency failed or cancelled");
                sched_release_holds(s,j->id);
                sched_note(s,"Job %d CANCELLED: %s",j->id,j->reason);
                changed=1;
            }else if(ready){
                j->state=QUEUED; j->queue_ms=now;
                sched_note(s,"Job %d WAITING -> QUEUED: dependencies completed",j->id);
                changed=1;
            }
        }
    }while(changed);
}
