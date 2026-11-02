#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scheduler.h"

static int checks;

// Test fail hua toh cleanup pe jao; production logic mein assert/abort nahi hai.
#define CHECK(expr) do { \
    checks++; \
    if(!(expr)){ \
        printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); \
        ok=0; goto done; \
    } \
} while(0)

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

static int registry(void)
{
    struct scheduler s;
    struct job_req req=request();
    struct job *j;
    int wid,id,other,ok=1;
    sched_init(&s);
    CHECK(!s.jobs&&!s.workers&&s.pending==0);
    CHECK(worker_find(&s,1001)==NULL&&job_find(&s,1)==NULL);
    CHECK(worker_add(&s,(struct resource){4,2048,1},100,&wid)==S_OK);
    CHECK(wid==1001&&s.worker_count==1);
    CHECK(worker_add(&s,(struct resource){2,1024,0},101,&other)==S_OK);
    CHECK(other!=wid&&s.worker_tail->id==other);
    CHECK(job_add(&s,&req,200,&id)==S_OK);
    j=job_find(&s,id);
    CHECK(j&&j->state==QUEUED&&s.pending==1);
    CHECK(j->submit_ms==200&&j->worker_id==0&&j->run_id==0);
    CHECK(j->failures==0&&j->deps==NULL&&j->checkpoint==NULL);
    req.user[0]='x';
    CHECK(!strcmp(j->req.user,"aryan"));
    CHECK(job_add(&s,&req,201,&other)==S_OK);
    CHECK(other>id&&s.job_count==2&&s.job_tail->id==other);

    // Cleanup future fields bhi own karta hai; allocated data leak nahi hona chahiye.
    j->deps=malloc(2*sizeof(int));
    j->checkpoint=malloc(8);
    CHECK(j->deps&&j->checkpoint);
done:
    sched_free(&s);
    if(s.jobs||s.workers||s.pending||s.job_count||s.worker_count) ok=0;
    sched_free(&s);
    return ok;
}

static int validation(void)
{
    struct scheduler s;
    struct job_req req=request();
    char name[33];
    int id,ok=1;
    sched_init(&s);
    CHECK(user_valid("Aryan_01-a.b"));
    CHECK(!user_valid("")&&!user_valid(NULL)&&!user_valid("has space"));
    CHECK(!user_valid("bad/name")&&!user_valid("a@b"));
    memset(name,'a',sizeof(name));
    name[31]='\0';
    CHECK(user_valid(name));
    name[31]='a'; name[32]='\0';
    CHECK(!user_valid(name));
    CHECK(worker_add(&s,(struct resource){0,100,0},0,&id)==S_BAD_INPUT);
    CHECK(worker_add(&s,(struct resource){1,-1,0},0,&id)==S_BAD_INPUT);
    CHECK(worker_add(&s,(struct resource){1,100,-1},0,&id)==S_BAD_INPUT);
    CHECK(worker_add(NULL,(struct resource){1,100,0},0,&id)==S_BAD_INPUT);
    CHECK(job_add(&s,NULL,0,&id)==S_BAD_INPUT);
    req.prio=0;
    CHECK(job_add(&s,&req,0,&id)==S_BAD_INPUT);
    req.prio=4;
    CHECK(job_add(&s,&req,0,&id)==S_BAD_INPUT);
    req=request(); req.time_limit=0;
    CHECK(job_add(&s,&req,0,&id)==S_BAD_INPUT);
    req=request(); req.need.gpu=-1;
    CHECK(job_add(&s,&req,0,&id)==S_BAD_INPUT);
    req=request(); req.path[0]='\0';
    CHECK(job_add(&s,&req,0,&id)==S_BAD_INPUT);
    req=request(); memset(req.user,'a',sizeof(req.user));
    CHECK(job_add(&s,&req,0,&id)==S_BAD_INPUT);
    req=request(); memset(req.path,'a',sizeof(req.path));
    CHECK(job_add(&s,&req,0,&id)==S_BAD_INPUT);
    req=request(); req.group=-1;
    CHECK(job_add(&s,&req,0,&id)==S_BAD_INPUT);
    req=request(); req.preemptible=2;
    CHECK(job_add(&s,&req,0,&id)==S_BAD_INPUT);
    CHECK(s.job_count==0&&s.worker_count==0&&s.next_job==1);
done:
    sched_free(&s);
    return ok;
}

static int resources(void)
{
    struct scheduler s;
    struct job_req req=request();
    struct worker *w;
    uint64_t run=0;
    int id,id2,wid,ok=1;
    sched_init(&s);
    CHECK(worker_add(&s,(struct resource){4,1024,1},0,&wid)==S_OK);
    CHECK(job_add(&s,&req,100,&id)==S_OK);
    CHECK(job_start(&s,id,wid,99,&run)==S_BAD_INPUT);
    CHECK(job_start(&s,id,wid,200,&run)==S_OK);
    w=worker_find(&s,wid);
    CHECK(w->used.cores==2&&w->used.ram==512&&w->used.gpu==1&&w->jobs==1);
    CHECK(s.pending==0&&job_find(&s,id)->wait_ms==100);
    CHECK(job_add(&s,&req,200,&id2)==S_OK);
    CHECK(job_start(&s,id2,wid,201,&run)==S_NO_RESOURCE);
    req.need=(struct resource){3,10,0};
    CHECK(!worker_fits(w,req.need));
    req.need=(struct resource){1,513,0};
    CHECK(!worker_fits(w,req.need));
    req.need=(struct resource){2,512,0};
    CHECK(worker_fits(w,req.need));
    CHECK(w->jobs==1&&s.pending==1&&job_find(&s,id2)->state==QUEUED);
    CHECK(job_finish(&s,id,run,COMPLETED,"exit 0",300)==S_OK);
    CHECK(w->used.cores==0&&w->used.ram==0&&w->used.gpu==0&&w->jobs==0);
    CHECK(job_start(&s,id2,wid,301,&run)==S_OK);
    CHECK(run==2&&s.pending==0);
done:
    sched_free(&s);
    return ok;
}

static int results(void)
{
    struct scheduler s;
    struct job_req req=request();
    struct job *j;
    struct worker *w;
    uint64_t run;
    int id,wid,ok=1;
    sched_init(&s);
    CHECK(worker_add(&s,(struct resource){2,512,1},0,&wid)==S_OK);
    CHECK(job_add(&s,&req,10,&id)==S_OK);
    CHECK(job_start(&s,id,wid,20,&run)==S_OK);
    j=job_find(&s,id); w=worker_find(&s,wid);
    CHECK(job_finish(&s,id,run+1,COMPLETED,NULL,30)==S_STALE_RUN);
    CHECK(job_finish(&s,id,0,COMPLETED,NULL,30)==S_STALE_RUN);
    CHECK(job_finish(&s,id,run,LOST,NULL,30)==S_BAD_STATE);
    CHECK(job_finish(&s,id,run,COMPLETED,NULL,19)==S_BAD_INPUT);
    CHECK(job_cancel_pending(&s,id,"cancel",30)==S_BAD_STATE);
    CHECK(w->jobs==1&&w->used.gpu==1&&j->state==RUNNING);
    CHECK(job_finish(&s,id,run,FAILED,"exit 127",30)==S_OK);
    CHECK(j->state==FAILED&&j->worker_id==0&&j->last_worker==wid);
    CHECK(j->end_ms==30&&!strcmp(j->reason,"exit 127"));
    CHECK(job_finish(&s,id,run,FAILED,"duplicate",31)==S_BAD_STATE);
    CHECK(w->jobs==0&&w->used.cores==0&&w->used.ram==0&&w->used.gpu==0);
    CHECK(job_start(&s,id,wid,40,&run)==S_BAD_STATE);
done:
    sched_free(&s);
    return ok;
}

static int workers(void)
{
    struct scheduler s;
    struct job_req req=request();
    uint64_t run;
    int id,wid,next,ok=1;
    sched_init(&s);
    CHECK(worker_add(&s,(struct resource){4,1024,1},0,&wid)==S_OK);
    CHECK(job_add(&s,&req,10,&id)==S_OK);
    CHECK(worker_set_state(&s,wid,SUSPECTED)==S_OK);
    CHECK(!worker_fits(worker_find(&s,wid),req.need));
    CHECK(job_start(&s,id,wid,20,&run)==S_BAD_STATE);
    CHECK(worker_set_state(&s,wid,ALIVE)==S_OK);
    CHECK(job_start(&s,id,wid,20,&run)==S_OK);
    CHECK(worker_set_state(&s,wid,DEAD)==S_BUSY);
    CHECK(worker_find(&s,wid)->state==ALIVE);
    CHECK(worker_set_state(&s,wid,DRAINING)==S_OK);
    CHECK(!worker_fits(worker_find(&s,wid),req.need));
    CHECK(job_finish(&s,id,run,CANCELLED,"stop confirmed",30)==S_OK);
    CHECK(worker_set_state(&s,wid,DEAD)==S_OK);
    CHECK(worker_set_state(&s,wid,ALIVE)==S_BAD_STATE);
    CHECK(worker_add(&s,(struct resource){4,1024,1},40,&next)==S_OK);
    CHECK(next!=wid);
done:
    sched_free(&s);
    return ok;
}

static int capacity(void)
{
    struct scheduler s;
    struct job_req req=request();
    int id=0,i,ok=1;
    sched_init(&s);
    for(i=0;i<QUEUE_CAP;i++) CHECK(job_add(&s,&req,(uint64_t)i,&id)==S_OK);
    CHECK(s.pending==QUEUE_CAP&&s.job_count==QUEUE_CAP);
    CHECK(job_add(&s,&req,QUEUE_CAP,&id)==S_QUEUE_FULL);
    CHECK(s.next_job==QUEUE_CAP+1&&s.job_count==QUEUE_CAP);
    CHECK(job_cancel_pending(&s,1,"user request",QUEUE_CAP)==S_OK);
    CHECK(job_cancel_pending(&s,1,"again",QUEUE_CAP)==S_BAD_STATE);
    CHECK(s.pending==QUEUE_CAP-1);
    CHECK(job_add(&s,&req,QUEUE_CAP,&id)==S_OK);
    CHECK(id==QUEUE_CAP+1&&s.pending==QUEUE_CAP);
    CHECK(job_find(&s,1)->state==CANCELLED);
done:
    sched_free(&s);
    return ok;
}

static int limits(void)
{
    struct scheduler s;
    struct job_req req=request();
    struct worker *w;
    uint64_t run;
    int id,wid,ok=1;
    sched_init(&s);
    CHECK(worker_add(&s,(struct resource){INT_MAX,LLONG_MAX,INT_MAX},0,&wid)==S_OK);
    req.need=(struct resource){INT_MAX,LLONG_MAX,INT_MAX};
    CHECK(job_add(&s,&req,0,&id)==S_OK);
    CHECK(job_start(&s,id,wid,0,&run)==S_OK);
    w=worker_find(&s,wid);
    CHECK(w->used.ram==LLONG_MAX&&worker_free(w).ram==0);
    CHECK(!worker_fits(w,(struct resource){1,1,0}));
    CHECK(job_finish(&s,id,run,COMPLETED,NULL,1)==S_OK);
    CHECK(worker_free(w).ram==LLONG_MAX);
    s.next_job=INT_MAX;
    CHECK(job_add(&s,&req,2,&id)==S_ID_LIMIT);
    s.next_worker=INT_MAX;
    CHECK(worker_add(&s,(struct resource){1,1,0},2,&wid)==S_ID_LIMIT);
    CHECK(job_add(&s,NULL,2,&id)==S_BAD_INPUT);
    s.next_job=2;
    CHECK(job_add(&s,&req,2,&id)==S_OK);
    s.next_run=UINT64_MAX;
    CHECK(job_start(&s,id,w->id,3,&run)==S_ID_LIMIT);
    CHECK(s.pending==1&&w->jobs==0);
done:
    sched_free(&s);
    return ok;
}

static int transitions(void)
{
    struct scheduler s;
    int from,to,expected,ok=1;
    const int edges[8][8]={
        [WAITING]={[QUEUED]=1,[CANCELLED]=1},
        [QUEUED]={[RUNNING]=1,[CANCELLED]=1},
        [RUNNING]={[COMPLETED]=1,[FAILED]=1,[CANCELLED]=1,[LOST]=1,[QUEUED]=1},
        [LOST]={[QUEUED]=1,[FAILED_PERMANENT]=1}
    };
    sched_init(&s);
    for(from=0;from<8;from++){
        for(to=0;to<8;to++){
            expected=edges[from][to];
            CHECK(job_can_move((enum job_state)from,(enum job_state)to)==expected);
        }
    }
    CHECK(!worker_can_move(DEAD,ALIVE));
    CHECK(worker_can_move(ALIVE,SUSPECTED)&&worker_can_move(SUSPECTED,ALIVE));
    CHECK(worker_can_move(ALIVE,DRAINING)&&!worker_can_move(DRAINING,ALIVE));
    CHECK(!job_can_move((enum job_state)99,QUEUED));
    CHECK(!strcmp(job_state_name(FAILED_PERMANENT),"FAILED_PERMANENT"));
    CHECK(!strcmp(worker_state_name(DRAINING),"DRAINING"));
    CHECK(!strcmp(job_state_name((enum job_state)99),"UNKNOWN"));
done:
    sched_free(&s);
    return ok;
}

int main(int argc,char **argv)
{
    struct { const char *name; int (*run)(void); } tests[]={
        {"registry",registry},{"validation",validation},{"resources",resources},
        {"results",results},{"workers",workers},{"capacity",capacity},
        {"limits",limits},{"transitions",transitions}
    };
    size_t i;
    int ran=0,failed=0;
    if(argc>2){ puts("usage: test_scheduler [test-name]"); return 1; }
    for(i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        if(argc==2&&strcmp(argv[1],tests[i].name)) continue;
        ran++;
        if(tests[i].run()) printf("PASS %s\n",tests[i].name);
        else failed++;
    }
    if(!ran){ puts("unknown test name"); return 1; }
    printf("%d groups, %d checks, %d failed\n",ran,checks,failed);
    return failed?1:0;
}
