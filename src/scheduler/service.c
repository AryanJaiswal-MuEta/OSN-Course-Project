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

int sched_register_worker(struct sched_service *svc,struct resource total,
                          uint64_t now,int *id)
{
    char msg[160];
    int err;
    if(!svc||!svc->s||!id) return S_BAD_INPUT;
    // Registry hi ID aur capacity record own karegi. Network handler ko
    // sirf returned ID reply mein bhejna hai; khud se IDs mat banana.
    err=worker_add(svc->s,total,now,id);
    if(svc->log){
        if(err==S_OK)
            snprintf(msg,sizeof(msg),"[SCHEDULER] Worker %d registered. Cores:%d RAM:%lldMB GPU:%d",
                     *id,total.cores,total.ram,total.gpu);
        else snprintf(msg,sizeof(msg),"[SCHEDULER] Worker registration rejected: %s",sched_error(err));
        svc->log(msg,svc->arg);
    }
    // Invalid registration pe scheduling wakeup nahi chahiye. Successful
    // add pe existing changed hook next loop pass ko request kar dega.
    if(err==S_OK&&svc->changed) svc->changed(svc->arg);
    return err;
}

int sched_submit(struct sched_service *svc,const char *user,
                 const struct job_req *req,uint64_t now,int *id)
{
    return sched_submit_deps(svc,user,req,NULL,0,now,id);
}

int sched_submit_deps(struct sched_service *svc,const char *user,
                      const struct job_req *req,const struct sched_dep *deps,
                      size_t count,uint64_t now,int *id)
{
    struct job_req copy;
    char msg[160];
    int err;
    if(!svc||!svc->s||!req||!id||!user_valid(user)) return S_BAD_INPUT;
    copy=*req;
    // Owner session se aayega, payload ke user field pe trust mat karo.
    memset(copy.user,0,sizeof(copy.user));
    memcpy(copy.user,user,strlen(user));
    err=job_add_deps(svc->s,&copy,deps,count,now,id);
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
    out->dispatch_pending=j->dispatch_pending;
    out->dispatch_ms=j->dispatch_ms;
    out->failures=j->failures;
    out->stop_pending=j->stop_pending;
    out->hold_for=j->hold_for;
    out->preempt_worker=j->preempt_worker;
    out->migrated_from=j->migrated_from;
    out->usage=j->usage;
    out->checkpoint_no=j->checkpoint_no;
    out->checkpoint_size=j->checkpoint_size;
    out->checkpoint_ms=j->checkpoint_ms;
    out->submit_ms=j->submit_ms;
    out->start_ms=j->start_ms;
    out->end_ms=j->end_ms;
    out->wait_ms=j->wait_ms;
    out->dep_count=j->dep_count;
    out->output_size=j->output_size;
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
        out[i].reserved_jobs=curr->reserved_jobs;
        out[i].load=curr->load;
        out[i].heartbeat_ms=curr->heartbeat_ms;
        out[i].dispatch_blocked=curr->dispatch_blocked;
        i++;
    }
    return S_OK;
}

static int owned(const struct sched_service *svc,const char *user,int id,struct job **out)
{
    struct job *j;
    if(!svc||!svc->s||!user_valid(user)||id<=0) return S_BAD_INPUT;
    j=job_find(svc->s,id);
    if(!j) return S_NOT_FOUND;
    if(strcmp(user,j->req.user)) return S_FORBIDDEN;
    *out=j;
    return S_OK;
}

int sched_status_owned(const struct sched_service *svc,const char *user,int id,struct sched_status *out)
{
    struct job *j;
    int err;
    if(!out) return S_BAD_INPUT;
    memset(out,0,sizeof(*out));
    err=owned(svc,user,id,&j);
    return err==S_OK?sched_status_get(svc,id,out):err;
}

int sched_cancel_owned(struct sched_service *svc,const char *user,int id,uint64_t now)
{
    struct job *j;
    int err=owned(svc,user,id,&j);
    if(err!=S_OK) return err;
    err=sched_cancel(svc->s,id,now);
    if(err==S_OK&&svc->changed) svc->changed(svc->arg);
    return err;
}

int sched_migrate_owned(struct sched_service *svc,const char *user,int id,uint64_t now)
{
    struct job *j;
    int err=owned(svc,user,id,&j);
    if(err!=S_OK) return err;
    err=sched_migrate(svc->s,id,now);
    if(err==S_OK&&svc->changed) svc->changed(svc->arg);
    return err;
}

int sched_output_get(const struct sched_service *svc,const char *user,int id,
                     void *buf,size_t cap,struct sched_output_info *out)
{
    struct job *j;
    int err;
    if(!out||(!buf&&cap)) return S_BAD_INPUT;
    memset(out,0,sizeof(*out));
    err=owned(svc,user,id,&j);
    if(err!=S_OK) return err;
    *out=(struct sched_output_info){j->output_size,j->run_id,j->output_received,j->state};
    // Size query allowed hai; chhote buffer pe half output likh ke caller ko
    // confuse nahi karna. Binary bytes hain, NUL/string assume mat karna.
    if(cap<j->output_size) return S_BUFFER_SMALL;
    if(j->output_size) memcpy(buf,j->output,j->output_size);
    return S_OK;
}

int sched_checkpoint_get(const struct sched_service *svc,const char *user,int id,
                         void *buf,size_t cap,struct sched_checkpoint_info *out)
{
    struct job *j;
    int err;
    if(!out||(!buf&&cap)) return S_BAD_INPUT;
    memset(out,0,sizeof(*out));
    err=owned(svc,user,id,&j);
    if(err!=S_OK) return err;
    *out=(struct sched_checkpoint_info){j->checkpoint_size,j->checkpoint_no,j->checkpoint_ms,j->run_id};
    if(cap<j->checkpoint_size) return S_BUFFER_SMALL;
    if(j->checkpoint_size) memcpy(buf,j->checkpoint,j->checkpoint_size);
    return S_OK;
}
