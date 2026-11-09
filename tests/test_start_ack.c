#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "worker_loop.h"
#include "scheduler.h"

static int checks,base;
static const char *program;
#define CHECK(expr) do { checks++; if(!(expr)){ \
    printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); ok=0; goto done; \
} } while(0)

struct context {
    int block;
    int tries;
    int starts;
    int results;
    int phase;
    int error;
    size_t bytes;
    struct worker_start start;
    struct worker_result result;
    struct scheduler *s;
    int wid;
};

static struct job_req request(void)
{
    struct job_req req;
    memset(&req,0,sizeof(req));
    strcpy(req.user,"local");
    strcpy(req.path,program);
    req.need=(struct resource){1,64,0};
    req.time_limit=5;
    req.prio=2;
    return req;
}

static int started(const struct worker_start *start,void *arg)
{
    struct context *c=arg;
    c->tries++;
    if(c->block) return 1;
    if(c->phase!=0) c->error=1;
    c->phase=1;
    c->starts++;
    c->start=*start;
    if(c->s){
        if(start->exec_state!=RUN_START_OK||
           job_confirm_start(c->s,start->id,c->wid,start->run_id,runner_now_ms())!=S_OK)
            c->error=1;
    }
    return 0;
}

static int output(int id,uint64_t run,const void *buf,size_t len,void *arg)
{
    struct context *c=arg;
    struct job *j;
    (void)run; (void)buf;
    if(c->phase!=1&&c->phase!=2) c->error=1;
    if(c->s){
        j=job_find(c->s,id);
        if(!j||j->state!=RUNNING) c->error=1;
    }
    c->phase=2;
    c->bytes+=len;
    return 0;
}

static int result(const struct worker_result *r,void *arg)
{
    struct context *c=arg;
    if(c->phase!=1&&c->phase!=2) c->error=1;
    c->phase=3;
    c->results++;
    c->result=*r;
    if(c->s&&job_finish(c->s,r->id,r->run_id,r->state,r->reason,runner_now_ms())!=S_OK)
        c->error=1;
    return 0;
}

static const struct worker_hooks hooks={.output=output,.result=result,.start=started};

static int wait_empty(struct worker_loop *loop)
{
    uint64_t start=runner_now_ms();
    while(loop->runner.head&&runner_now_ms()-start<10000)
        if(worker_loop_step(loop,50)<0) return 0;
    return loop->runner.head==NULL;
}

static int ordering(void)
{
    struct worker_loop loop;
    struct context c={0};
    struct job_req req=request();
    struct run_job *j;
    char *args[]={(char *)program,"hello",NULL};
    char dir[128];
    uint64_t deadline;
    int ok=1;
    CHECK(worker_loop_init(&loop,&hooks,&c)==RUN_OK);
    c.block=1;
    CHECK(worker_loop_assign(&loop,base,17,&req,args)==RUN_OK);
    j=runner_find(&loop.runner,base,17);
    CHECK(j&&j->exec_state==RUN_START_PENDING&&!j->start_reported);
    strcpy(dir,j->dir);
    deadline=runner_now_ms()+5000;
    while((!j->reaped||!c.tries)&&runner_now_ms()<deadline) CHECK(worker_loop_step(&loop,50)==0);
    CHECK(j->reaped&&c.tries>0&&c.starts==0&&c.bytes==0&&c.results==0);
    CHECK(!j->start_reported&&j->offset==0&&access(dir,F_OK)==0);
    c.block=0;
    CHECK(wait_empty(&loop));
    CHECK(c.starts==1&&c.results==1&&c.bytes>0&&c.phase==3&&!c.error);
    CHECK(c.start.id==base&&c.start.run_id==17&&c.start.exec_state==RUN_START_OK);
    CHECK(c.result.exec_state==RUN_START_OK&&c.result.state==COMPLETED);
    CHECK(access(dir,F_OK)<0&&errno==ENOENT);
done:
    worker_loop_free(&loop);
    return ok;
}

static int failure(void)
{
    struct worker_loop loop;
    struct context c={0};
    struct job_req req=request();
    char *args[]={"osn_missing_exec_for_start_test_9372",NULL};
    int ok=1;
    CHECK(worker_loop_init(&loop,&hooks,&c)==RUN_OK);
    strcpy(req.path,args[0]);
    CHECK(worker_loop_assign(&loop,base,1,&req,args)==RUN_OK);
    CHECK(wait_empty(&loop));
    CHECK(c.starts==1&&c.results==1&&c.bytes==0&&!c.error);
    CHECK(c.start.exec_state==RUN_START_FAILED&&c.start.stage==6&&c.start.error==ENOENT);
    CHECK(c.result.exec_state==RUN_START_FAILED&&c.result.state==FAILED);
    CHECK(strstr(c.result.reason,"code 127")&&strstr(c.result.reason,"execvp"));
done:
    worker_loop_free(&loop);
    return ok;
}

static int exit127(void)
{
    struct worker_loop loop;
    struct context c={0};
    struct job_req req=request();
    char *args[]={(char *)program,"exit127",NULL};
    int ok=1;
    CHECK(worker_loop_init(&loop,&hooks,&c)==RUN_OK);
    CHECK(worker_loop_assign(&loop,base,1,&req,args)==RUN_OK);
    CHECK(wait_empty(&loop));
    CHECK(c.starts==1&&c.results==1&&!c.error);
    // Program khud 127 return kare toh exec failure nahi hai. Error pipe
    // packet se hi pata chalega ki execvp fail hua tha ya program chala tha.
    CHECK(c.start.exec_state==RUN_START_OK&&c.start.stage==0);
    CHECK(c.result.exec_state==RUN_START_OK&&c.result.state==FAILED);
    CHECK(strstr(c.result.reason,"code 127")&&!strstr(c.result.reason,"execvp"));
done:
    worker_loop_free(&loop);
    return ok;
}

static int cancel_early(void)
{
    struct worker_loop loop;
    struct context c={0};
    struct job_req req=request();
    char *args[]={(char *)program,"sleep","5000",NULL};
    int ok=1;
    CHECK(worker_loop_init(&loop,&hooks,&c)==RUN_OK);
    CHECK(worker_loop_assign(&loop,base,1,&req,args)==RUN_OK);
    CHECK(worker_loop_cancel(&loop,base,1)==RUN_OK);
    CHECK(wait_empty(&loop));
    CHECK(c.starts==1&&c.results==1&&!c.error);
    CHECK(c.start.exec_state==RUN_START_UNCONFIRMED);
    CHECK(c.result.exec_state==RUN_START_UNCONFIRMED&&c.result.state==CANCELLED);
done:
    worker_loop_free(&loop);
    return ok;
}

static int scheduler_bridge(void)
{
    struct scheduler s;
    struct worker_loop loop;
    struct context c={0};
    struct job_req req=request();
    struct worker *w;
    uint64_t run;
    char *args[]={(char *)program,"hello",NULL};
    int id,ok=1;
    sched_init(&s);
    CHECK(worker_loop_init(&loop,&hooks,&c)==RUN_OK);
    s.next_job=base;
    c.s=&s;
    CHECK(worker_add(&s,(struct resource){1,64,0},runner_now_ms(),&c.wid)==S_OK);
    CHECK(job_add(&s,&req,runner_now_ms(),&id)==S_OK);
    CHECK(job_reserve(&s,id,c.wid,runner_now_ms(),&run)==S_OK);
    CHECK(worker_loop_assign(&loop,id,run,&req,args)==RUN_OK);
    CHECK(job_find(&s,id)->state==QUEUED&&job_find(&s,id)->dispatch_pending);
    CHECK(wait_empty(&loop));
    w=worker_find(&s,c.wid);
    CHECK(c.starts==1&&c.results==1&&c.bytes>0&&!c.error);
    CHECK(job_find(&s,id)->state==COMPLETED&&!job_find(&s,id)->dispatch_pending);
    CHECK(w->used.cores==0&&w->used.ram==0&&w->used.gpu==0);
    CHECK(w->jobs==0&&w->reserved_jobs==0&&s.pending==0);
done:
    worker_loop_free(&loop);
    sched_free(&s);
    return ok;
}

int main(int argc,char **argv)
{
    char path[PATH_MAX];
    size_t i;
    int ran=0,failed=0;
    struct { const char *name; int (*run)(void); } tests[]={
        {"ordering",ordering},{"failure",failure},{"exit127",exit127},
        {"cancel_early",cancel_early},{"scheduler_bridge",scheduler_bridge}
    };
    if(argc<2||argc>3||!realpath(argv[1],path)||strlen(path)>=PATH_LEN){
        puts("usage: test_start_ack <fixture-path> [test-name]"); return 1;
    }
    program=path;
    base=600000+(int)(getpid()%1000000)*10;
    for(i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        if(argc==3&&strcmp(argv[2],tests[i].name)) continue;
        ran++;
        if(tests[i].run()) printf("PASS start ack %s\n",tests[i].name);
        else failed++;
    }
    printf("%d start-ack groups, %d checks, %d failed\n",ran,checks,failed);
    return !ran||failed?1:0;
}
