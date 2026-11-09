#include <stdio.h>
#include <string.h>
#include "scheduler_service.h"

static int checks;
#define CHECK(expr) do { checks++; if(!(expr)){ \
    printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); ok=0; goto done; \
} } while(0)

static struct job_req request(void)
{
    struct job_req req;
    memset(&req,0,sizeof(req));
    strcpy(req.user,"aryan");
    strcpy(req.path,"./hello");
    req.need=(struct resource){2,512,1};
    req.time_limit=10;
    req.prio=2;
    return req;
}

static int reserve_accept(void)
{
    struct scheduler s;
    struct sched_service svc;
    struct sched_status status;
    struct sched_node node;
    struct job_req req=request();
    struct job *j;
    struct worker *w;
    uint64_t run,other_run=999;
    size_t count;
    int wid,id,other,ok=1;
    sched_init(&s);
    sched_service_init(&svc,&s,NULL,NULL,NULL);
    CHECK(worker_add(&s,(struct resource){2,512,1},0,&wid)==S_OK);
    CHECK(job_add(&s,&req,10,&id)==S_OK);
    CHECK(job_add(&s,&req,20,&other)==S_OK);
    CHECK(job_reserve(&s,id,wid,100,&run)==S_OK);
    j=job_find(&s,id); w=worker_find(&s,wid);
    CHECK(j->state==QUEUED&&j->dispatch_pending&&j->dispatch_ms==100);
    CHECK(j->start_ms==0&&j->worker_id==wid&&j->last_worker==0);
    CHECK(w->jobs==0&&w->reserved_jobs==1&&s.pending==2);
    CHECK(w->used.cores==2&&w->used.ram==512&&w->used.gpu==1);
    CHECK(sched_status_get(&svc,id,&status)==S_OK&&status.dispatch_pending);
    CHECK(status.state==QUEUED&&status.run_id==run&&status.dispatch_ms==100);
    CHECK(sched_nodes_get(&svc,&node,1,&count)==S_OK&&node.reserved_jobs==1);
    CHECK(node.jobs==0&&node.free.cores==0&&node.free.ram==0&&node.free.gpu==0);
    CHECK(job_reserve(&s,other,wid,101,&other_run)==S_NO_RESOURCE);
    CHECK(other_run==999&&job_find(&s,other)->worker_id==0);
    CHECK(job_reserve(&s,id,wid,101,&other_run)==S_BAD_STATE);
    CHECK(job_confirm_start(&s,id,wid,run+1,110)==S_STALE_RUN);
    CHECK(job_confirm_start(&s,id,wid+1,run,110)==S_STALE_RUN);
    CHECK(job_confirm_start(&s,id,wid,run,99)==S_BAD_INPUT);
    CHECK(w->reserved_jobs==1&&w->jobs==0&&s.pending==2);
    CHECK(worker_set_state(&s,wid,SUSPECTED)==S_OK);
    CHECK(job_confirm_start(&s,id,wid,run,120)==S_OK);
    CHECK(j->state==RUNNING&&!j->dispatch_pending&&j->start_ms==120&&j->wait_ms==110);
    CHECK(w->reserved_jobs==0&&w->jobs==1&&s.pending==1);
    CHECK(w->used.cores==2&&w->used.ram==512&&w->used.gpu==1);
    CHECK(job_confirm_start(&s,id,wid,run,121)==S_BAD_STATE);
    CHECK(job_finish(&s,id,run,COMPLETED,"exit 0",130)==S_OK);
    CHECK(w->used.cores==0&&w->used.ram==0&&w->used.gpu==0&&w->jobs==0);
done:
    sched_free(&s);
    return ok;
}

static int abort_reassign(void)
{
    struct scheduler s;
    struct job_req req=request();
    struct worker *a,*b;
    struct job *j;
    uint64_t run,next;
    int first,second,id,ok=1;
    sched_init(&s);
    CHECK(worker_add(&s,(struct resource){2,512,1},0,&first)==S_OK);
    CHECK(worker_add(&s,(struct resource){2,512,1},0,&second)==S_OK);
    CHECK(job_add(&s,&req,10,&id)==S_OK);
    CHECK(job_reserve(&s,id,first,20,&run)==S_OK);
    CHECK(job_abort_dispatch(&s,id,first,run,"not started",19)==S_BAD_INPUT);
    CHECK(job_abort_dispatch(&s,id,first,run,"not started",30)==S_OK);
    a=worker_find(&s,first); b=worker_find(&s,second); j=job_find(&s,id);
    CHECK(a->reserved_jobs==0&&a->used.cores==0&&a->used.ram==0&&a->used.gpu==0);
    CHECK(j->state==QUEUED&&!j->dispatch_pending&&j->worker_id==0&&s.pending==1);
    CHECK(j->queue_ms==10&&j->failures==0&&!strcmp(j->reason,"not started"));
    CHECK(job_abort_dispatch(&s,id,first,run,"duplicate",31)==S_STALE_RUN);
    CHECK(job_reserve(&s,id,second,40,&next)==S_OK&&next!=run);
    CHECK(job_confirm_start(&s,id,first,run,50)==S_STALE_RUN);
    CHECK(job_confirm_start(&s,id,second,run,50)==S_STALE_RUN);
    CHECK(job_confirm_start(&s,id,second,next,50)==S_OK);
    CHECK(job_finish(&s,id,run,COMPLETED,"late result",60)==S_STALE_RUN);
    CHECK(a->jobs==0&&b->jobs==1&&b->reserved_jobs==0);
    CHECK(job_finish(&s,id,next,FAILED,"exit 7",60)==S_OK);
    CHECK(b->used.cores==0&&b->used.ram==0&&b->used.gpu==0&&b->jobs==0);
    CHECK(s.pending==0&&j->failures==0&&j->last_worker==second);
done:
    sched_free(&s);
    return ok;
}

static int cancel_reserved(void)
{
    struct scheduler s;
    struct job_req req=request();
    struct worker *w;
    uint64_t run;
    int id,wid,ok=1;
    sched_init(&s);
    CHECK(worker_add(&s,(struct resource){2,512,1},0,&wid)==S_OK);
    CHECK(job_add(&s,&req,10,&id)==S_OK);
    CHECK(job_reserve(&s,id,wid,20,&run)==S_OK);
    w=worker_find(&s,wid);
    CHECK(job_cancel_pending(&s,id,"cancel",30)==S_BUSY);
    CHECK(worker_set_state(&s,wid,DEAD)==S_BUSY);
    CHECK(job_finish(&s,id,run,FAILED,"not started",30)==S_BAD_STATE);
    CHECK(w->reserved_jobs==1&&s.pending==1);
    CHECK(job_abort_dispatch(&s,id,wid,run,"worker confirmed stop",40)==S_OK);
    CHECK(worker_set_state(&s,wid,DEAD)==S_OK);
    CHECK(job_cancel_pending(&s,id,"cancel",50)==S_OK);
    CHECK(s.pending==0&&job_find(&s,id)->state==CANCELLED);
    CHECK(w->used.cores==0&&w->used.ram==0&&w->used.gpu==0&&w->reserved_jobs==0);
    CHECK(job_confirm_start(NULL,id,wid,run,60)==S_BAD_INPUT);
done:
    sched_free(&s);
    return ok;
}

static int full_queue(void)
{
    struct scheduler s;
    struct job_req req=request();
    uint64_t run;
    int i,id=0,wid,ok=1;
    sched_init(&s);
    CHECK(worker_add(&s,(struct resource){2,512,1},0,&wid)==S_OK);
    for(i=0;i<QUEUE_CAP;i++) CHECK(job_add(&s,&req,(uint64_t)i,&id)==S_OK);
    CHECK(job_reserve(&s,1,wid,2000,&run)==S_OK);
    CHECK(s.pending==QUEUE_CAP);
    CHECK(job_add(&s,&req,2001,&id)==S_QUEUE_FULL);
    CHECK(job_abort_dispatch(&s,1,wid,run,"preparation aborted",2002)==S_OK);
    CHECK(s.pending==QUEUE_CAP&&job_find(&s,1)->state==QUEUED);
    CHECK(worker_find(&s,wid)->reserved_jobs==0);
    CHECK(job_reserve(&s,1,wid,2003,&run)==S_OK);
    CHECK(job_confirm_start(&s,1,wid,run,2004)==S_OK);
    CHECK(s.pending==QUEUE_CAP-1);
    CHECK(job_add(&s,&req,2005,&id)==S_OK&&id==QUEUE_CAP+1);
    CHECK(job_finish(&s,1,run,COMPLETED,"exit 0",2006)==S_OK);
    CHECK(s.pending==QUEUE_CAP&&s.job_count==QUEUE_CAP+1);
done:
    sched_free(&s);
    return ok;
}

int main(int argc,char **argv)
{
    struct { const char *name; int (*run)(void); } tests[]={
        {"reserve_accept",reserve_accept},{"abort_reassign",abort_reassign},
        {"cancel_reserved",cancel_reserved},{"full_queue",full_queue}
    };
    size_t i;
    int ran=0,failed=0;
    if(argc>2){ puts("usage: test_dispatch [test-name]"); return 1; }
    for(i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        if(argc==2&&strcmp(argv[1],tests[i].name)) continue;
        ran++;
        if(tests[i].run()) printf("PASS dispatch %s\n",tests[i].name);
        else failed++;
    }
    printf("%d dispatch groups, %d checks, %d failed\n",ran,checks,failed);
    return !ran||failed?1:0;
}
