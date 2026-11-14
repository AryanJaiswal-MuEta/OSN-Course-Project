#include <stdio.h>
#include <string.h>
#include "scheduler.h"

#define CHECK(expr) do { if(!(expr)){ \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expr); goto done; \
} } while(0)

static int add(struct scheduler *s,int cores,int prio,int *id)
{
    struct job_req req={0};
    strcpy(req.user,"test");
    strcpy(req.path,"printf");
    req.need=(struct resource){cores,64,1};
    req.time_limit=10;
    req.prio=prio;
    return job_add(s,&req,100,id);
}

int main(void)
{
    struct scheduler s;
    struct sched_pick pick;
    uint64_t run;
    int small,large,first,second,third,ok=0;
    sched_init(&s);
    CHECK(sched_pick_next(&s,SCHED_FIFO_ORDER,100,&pick)==S_NOT_FOUND);
    CHECK(worker_add(&s,(struct resource){2,128,1},100,&small)==S_OK);
    CHECK(add(&s,4,3,&first)==S_OK);
    CHECK(sched_pick_next(&s,SCHED_PRIORITY_ORDER,100,&pick)==S_NO_RESOURCE);
    CHECK(add(&s,2,2,&second)==S_OK);
    CHECK(add(&s,1,1,&third)==S_OK);
    CHECK(sched_pick_next(&s,SCHED_FIFO_ORDER,100,&pick)==S_NO_RESOURCE);
    CHECK(sched_pick_next(&s,SCHED_PRIORITY_ORDER,100,&pick)==S_OK);
    CHECK(pick.job_id==third&&pick.worker_id==small);
    CHECK(worker_add(&s,(struct resource){4,128,1},100,&large)==S_OK);
    CHECK(sched_pick_next(&s,SCHED_FIFO_ORDER,100,&pick)==S_OK);
    CHECK(pick.job_id==first&&pick.worker_id==large);
    CHECK(sched_pick_next(&s,SCHED_PRIORITY_ORDER,10099,&pick)==S_OK&&pick.job_id==third);
    CHECK(sched_pick_next(&s,SCHED_PRIORITY_ORDER,10100,&pick)==S_OK&&pick.job_id==second);
    CHECK(sched_pick_next(&s,SCHED_PRIORITY_ORDER,20099,&pick)==S_OK&&pick.job_id==second);
    CHECK(sched_pick_next(&s,SCHED_PRIORITY_ORDER,20100,&pick)==S_OK&&pick.job_id==first);
    CHECK(job_reserve(&s,first,large,20100,&run)==S_OK);
    CHECK(sched_pick_next(&s,SCHED_FIFO_ORDER,20100,&pick)==S_OK&&pick.job_id==second);
    CHECK(job_reserve(&s,second,small,20100,&run)==S_OK);
    CHECK(sched_pick_next(&s,SCHED_FIFO_ORDER,20100,&pick)==S_NO_RESOURCE);
    CHECK(job_abort_dispatch(&s,second,small,run,NULL,20100)==S_OK);
    CHECK(worker_set_state(&s,small,DRAINING)==S_OK);
    CHECK(sched_pick_next(&s,SCHED_PRIORITY_ORDER,20100,&pick)==S_NO_RESOURCE);
    ok=1;
done:
    sched_free(&s);
    if(ok) puts("PASS FIFO blocking, priority skipping/aging, reservation and eligibility");
    return ok?0:1;
}
