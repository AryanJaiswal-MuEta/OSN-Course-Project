#include <stdio.h>
#include <string.h>
#include "scheduler_service.h"

int sched_service_init(struct sched_service *svc,struct scheduler *s,
                       void (*changed)(void *),void (*log)(const char *,void *),void *arg)
{
    if(!svc||!s) return S_BAD_INPUT;
    svc->s=s;
    svc->changed=changed;
    svc->log=log;
    svc->arg=arg;
    return S_OK;
}

int sched_submit(struct sched_service *svc,const char *user,
                 const struct job_req *req,uint64_t now,int *id)
{
    struct job_req copy;
    char msg[160];
    int err;
    if(!svc||!svc->s||!req||!id||!user_valid(user)) return S_BAD_INPUT;
    copy=*req;
    // Owner session se aayega, payload ke user field pe trust mat karo.
    memset(copy.user,0,sizeof(copy.user));
    memcpy(copy.user,user,strlen(user));
    err=job_add(svc->s,&copy,now,id);
    if(svc->log){
        if(err==S_OK) snprintf(msg,sizeof(msg),"[SCHEDULER] Job %d SUBMITTED by %s",*id,user);
        else snprintf(msg,sizeof(msg),"[SCHEDULER] Submit rejected for %s: %s",user,sched_error(err));
        svc->log(msg,svc->arg);
    }
    if(err==S_OK&&svc->changed) svc->changed(svc->arg);
    return err;
}

int sched_status_get(const struct sched_service *svc,int id,struct sched_status *out)
{
    const struct job *j;
    if(!svc||!svc->s||!out||id<=0) return S_BAD_INPUT;
    for(j=svc->s->jobs;j&&j->id!=id;j=j->next){}
    if(!j) return S_NOT_FOUND;
    memset(out,0,sizeof(*out));
    out->id=j->id;
    out->req=j->req;
    out->state=j->state;
    out->worker_id=j->worker_id;
    out->last_worker=j->last_worker;
    out->run_id=j->run_id;
    out->failures=j->failures;
    out->usage=j->usage;
    out->checkpoint_no=j->checkpoint_no;
    out->checkpoint_size=j->checkpoint_size;
    out->checkpoint_ms=j->checkpoint_ms;
    memcpy(out->reason,j->reason,sizeof(out->reason));
    return S_OK;
}

int sched_nodes_get(const struct sched_service *svc,struct sched_node *out,
                    size_t cap,size_t *count)
{
    const struct worker *curr;
    size_t i=0;
    if(!svc||!svc->s||!count||(!out&&cap)) return S_BAD_INPUT;
    *count=svc->s->worker_count;
    // Pehle required size batao; chhota buffer ho toh half-list mat likho.
    if(cap<*count) return S_BUFFER_SMALL;
    for(curr=svc->s->workers;curr;curr=curr->next){
        out[i].id=curr->id;
        out[i].state=curr->state;
        out[i].total=curr->total;
        out[i].free=worker_free(curr);
        out[i].jobs=curr->jobs;
        i++;
    }
    return S_OK;
}
