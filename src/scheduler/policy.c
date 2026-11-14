#include "scheduler.h"

static int priority(const struct job *j,uint64_t now)
{
    uint64_t extra=now>=j->queue_ms?now-j->queue_ms:0;
    uint64_t age=j->wait_ms/10000+extra/10000;
    age+=(j->wait_ms%10000+extra%10000)/10000;
    return age>=(uint64_t)(j->req.prio-1)?1:j->req.prio-(int)age;
}

static const struct worker *fit(const struct scheduler *s,const struct job *j)
{
    const struct worker *w;
    // Core milestone mein first fitting worker; load/cache ranking baad mein.
    for(w=s->workers;w;w=w->next)
        if(worker_fits(w,j->req.need)) return w;
    return NULL;
}

int sched_pick_next(const struct scheduler *s,enum sched_policy policy,
                    uint64_t now,struct sched_pick *out)
{
    const struct job *j,*best=NULL;
    const struct worker *w,*target=NULL;
    int prio,best_prio=4,queued=0;
    if(!s||!out||(policy!=SCHED_FIFO_ORDER&&policy!=SCHED_PRIORITY_ORDER))
        return S_BAD_INPUT;
    *out=(struct sched_pick){0,0};
    for(j=s->jobs;j;j=j->next){
        if(j->state!=QUEUED||j->dispatch_pending) continue;
        queued=1;
        if(policy==SCHED_FIFO_ORDER){
            // List submission order mein hai; head fit nahi hua toh wait.
            best=j;
            target=fit(s,j);
            break;
        }
        prio=priority(j,now);
        // Equal priority mein pehle wala hi rakho. Non-fitting job ko
        // skip karo, taaki uske peeche wali job abhi chal sake.
        if(prio>=best_prio) continue;
        w=fit(s,j);
        if(!w) continue;
        best=j;
        target=w;
        best_prio=prio;
        if(prio==1) break;
    }
    if(!queued) return S_NOT_FOUND;
    if(!target) return S_NO_RESOURCE;
    out->job_id=best->id;
    out->worker_id=target->id;
    return S_OK;
}
