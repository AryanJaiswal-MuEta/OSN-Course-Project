#include <string.h>
#include "scheduler.h"

static long long held(const struct scheduler *s,const char *user)
{
    const struct job *j;
    long long cores=0;
    for(j=s->jobs;j;j=j->next)
        if(j->state==RUNNING&&!strcmp(j->req.user,user)) cores+=j->req.need.cores;
    return cores;
}

int sched_job_eligible(const struct scheduler *s,const struct job *j)
{
    const struct job *b;
    if(j->state!=QUEUED||j->dispatch_pending) return 0;
    if(!j->hold_for) return 1;
    for(b=s->jobs;b&&b->id!=j->hold_for;b=b->next){}
    // Beneficiary start/terminate hote hi hold ka purpose khatam hai.
    return !b||b->state==RUNNING||b->state==COMPLETED||b->state==FAILED||
           b->state==CANCELLED||b->state==FAILED_PERMANENT;
}

int sched_quota_skip(const struct scheduler *s,const struct job *j)
{
    const struct worker *w;
    const struct job *other;
    long long total=0;
    for(w=s->workers;w;w=w->next) if(w->state!=DEAD) total+=w->total.cores;
    if(held(s,j->req.user)<=total/2) return 0;
    for(other=s->jobs;other;other=other->next)
        if(sched_job_eligible(s,other)&&strcmp(j->req.user,other->req.user)&&
           held(s,other->req.user)<=total/2) return 1;
    return 0;
}
