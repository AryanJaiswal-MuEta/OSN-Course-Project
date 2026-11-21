#include <stdlib.h>
#include "scheduler.h"

static int enough(struct resource free_res,struct resource need)
{
    return free_res.cores>=need.cores&&free_res.ram>=need.ram&&free_res.gpu>=need.gpu;
}

static int before(const struct job *a,const struct job *b,uint64_t now)
{
    int pa=sched_priority(a,now),pb=sched_priority(b,now);
    if(pa!=pb) return pa>pb;
    if(a->submit_ms!=b->submit_ms) return a->submit_ms>b->submit_ms;
    return a->id>b->id;
}

int sched_preempt(struct scheduler *s,int id,uint64_t now,int *worker_id)
{
    struct job *j=job_find(s,id),*curr,**victims,*temp;
    struct worker *w;
    struct resource free_res;
    size_t n,i,k,take;
    if(!s||!worker_id) return S_BAD_INPUT;
    if(!j) return S_NOT_FOUND;
    if(!sched_job_eligible(s,j)||sched_quota_skip(s,j)||sched_priority(j,now)!=1||j->preempt_worker)
        return S_BAD_STATE;
    if(sched_fit(s,j)) return S_BAD_STATE;
    if(s->job_count>SIZE_MAX/sizeof(*victims)) return S_NO_MEMORY;
    victims=malloc(s->job_count*sizeof(*victims));
    if(!victims) return S_NO_MEMORY;
    for(w=s->workers;w;w=w->next){
        if(w->state!=ALIVE||w->preempt_for||!enough(w->total,j->req.need)) continue;
        n=0;
        for(curr=s->jobs;curr;curr=curr->next){
            if(curr->worker_id!=w->id||curr->state!=RUNNING||!curr->req.preemptible||curr->stop_pending)
                continue;
            victims[n++]=curr;
        }
        // List chhoti hogi; insertion sort mein global comparator context nahi chahiye.
        for(i=1;i<n;i++){
            temp=victims[i]; k=i;
            while(k&&before(temp,victims[k-1],now)){ victims[k]=victims[k-1]; k--; }
            victims[k]=temp;
        }
        free_res=worker_free(w);
        for(take=0;take<n&&!enough(free_res,j->req.need);take++){
            free_res.cores+=victims[take]->req.need.cores;
            free_res.ram+=victims[take]->req.need.ram;
            free_res.gpu+=victims[take]->req.need.gpu;
        }
        if(!take||!enough(free_res,j->req.need)) continue;
        // Puri feasibility prove hone ke baad hi stop intents publish karo.
        // Worker lock se freed resources koi third job nahi chura sakti.
        w->preempt_for=id; j->preempt_worker=w->id;
        for(i=0;i<take;i++){
            victims[i]->stop_pending=S_STOP_PREEMPT; victims[i]->hold_for=id;
            victims[i]->stop_ms=now;
            sched_note(s,"Job %d preempt requested for priority-1 job %d",victims[i]->id,id);
        }
        *worker_id=w->id;
        free(victims);
        return S_OK;
    }
    free(victims);
    return S_NO_RESOURCE;
}

int sched_preempt_next(struct scheduler *s,enum sched_policy policy,uint64_t now)
{
    struct job *j,*best=NULL;
    int worker_id;
    if(!s||(policy!=SCHED_FIFO_ORDER&&policy!=SCHED_PRIORITY_ORDER)) return S_BAD_INPUT;
    for(j=s->jobs;j;j=j->next){
        if(!sched_job_eligible(s,j)||sched_quota_skip(s,j)) continue;
        if(policy==SCHED_FIFO_ORDER){ best=j; break; }
        if(sched_priority(j,now)==1&&!j->preempt_worker&&!sched_fit(s,j)){ best=j; break; }
    }
    if(!best||sched_priority(best,now)!=1||best->preempt_worker) return S_NOT_FOUND;
    return sched_preempt(s,best->id,now,&worker_id);
}
