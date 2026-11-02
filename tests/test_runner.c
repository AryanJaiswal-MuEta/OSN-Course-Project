#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "runner.h"

static int checks,base;
static const char *program;

#define CHECK(expr) do { checks++; if(!(expr)){ \
    printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); ok=0; goto done; \
} } while(0)

struct capture {
    size_t bytes;
    int reject;
    uint64_t first_ms;
};

static int capture(int id,uint64_t run_id,const void *buf,size_t len,void *arg)
{
    struct capture *c=arg;
    (void)id; (void)run_id; (void)buf;
    if(c->reject) return 1;
    if(!c->first_ms) c->first_ms=runner_now_ms();
    c->bytes+=len;
    return 0;
}

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

static int wait_all(struct runner *r,struct capture *c)
{
    struct run_job *j;
    struct timespec delay={0,20000000};
    uint64_t start=runner_now_ms();
    int pending;
    do {
        if(runner_poll(r,capture,c)<0) return 0;
        pending=0;
        for(j=r->head;j;j=j->next) if(!j->finished) pending++;
        if(!pending) return 1;
        nanosleep(&delay,NULL);
    } while(runner_now_ms()-start<10000);
    return 0;
}

static int concurrent(void)
{
    struct runner r;
    struct job_req req=request();
    struct run_job *j;
    struct capture c={0,0,0};
    char dir[128];
    char *hello[]={(char *)program,"hello",NULL};
    char *spam[]={(char *)program,"spam",NULL};
    char *fail[]={(char *)program,"exit",NULL};
    pid_t pid;
    int ok=1;
    CHECK(runner_init(&r)==RUN_OK);
    CHECK(runner_start(&r,base,1,&req,hello)==RUN_OK);
    CHECK(runner_start(&r,base+1,8,&req,spam)==RUN_OK);
    CHECK(runner_start(&r,base+2,9,&req,fail)==RUN_OK);
    CHECK(runner_start(&r,base,2,&req,hello)==RUN_BUSY);
    CHECK(runner_remove(&r,base,1)==RUN_BUSY);
    CHECK(runner_cancel(&r,base,99)==RUN_NOT_FOUND);
    CHECK(wait_all(&r,&c));
    j=runner_find(&r,base,1);
    CHECK(j&&j->state==COMPLETED&&j->reaped);
    strcpy(dir,j->dir); pid=j->pid;
    CHECK(access(dir,F_OK)==0);
    CHECK(c.bytes>40960);
    CHECK(runner_find(&r,base+1,8)->state==COMPLETED);
    CHECK(runner_find(&r,base+2,9)->state==FAILED);
    CHECK(strstr(runner_find(&r,base+2,9)->reason,"code 7")!=NULL);
    CHECK(waitpid(pid,NULL,WNOHANG)==-1&&errno==ECHILD);
    CHECK(runner_remove(&r,base,1)==RUN_OK);
    CHECK(access(dir,F_OK)<0&&errno==ENOENT);
done:
    runner_free(&r);
    return ok;
}

static int backpressure(void)
{
    struct runner r;
    struct job_req req=request();
    struct run_job *j;
    struct capture c={0,1,0};
    struct timespec delay={0,20000000};
    char *args[]={(char *)program,"hello",NULL};
    uint64_t start;
    int ok=1;
    CHECK(runner_init(&r)==RUN_OK);
    CHECK(runner_start(&r,base,1,&req,args)==RUN_OK);
    j=runner_find(&r,base,1);
    start=runner_now_ms();
    while(!j->reaped&&runner_now_ms()-start<5000){
        CHECK(runner_poll(&r,capture,&c)>=0);
        nanosleep(&delay,NULL);
    }
    CHECK(j->reaped&&!j->finished&&j->offset==0&&c.bytes==0);
    CHECK(runner_remove(&r,base,1)==RUN_BUSY);
    c.reject=0;
    CHECK(wait_all(&r,&c));
    CHECK(c.bytes>0&&j->finished);
done:
    runner_free(&r);
    return ok;
}

static int cancel(void)
{
    struct runner r;
    struct job_req req=request();
    struct capture c={0,0,0};
    char *args[]={(char *)program,"sleep","5000",NULL};
    int ok=1;
    CHECK(runner_init(&r)==RUN_OK);
    CHECK(runner_start(&r,base,1,&req,args)==RUN_OK);
    CHECK(runner_cancel(&r,base,1)==RUN_OK);
    CHECK(runner_cancel(&r,base,1)==RUN_BUSY);
    CHECK(wait_all(&r,&c));
    CHECK(runner_find(&r,base,1)->state==CANCELLED);
    CHECK(strstr(runner_find(&r,base,1)->reason,"Cancelled")!=NULL);
done:
    runner_free(&r);
    return ok;
}

static int live_output(void)
{
    struct runner r;
    struct job_req req=request();
    struct capture c={0,0,0};
    char *args[]={(char *)program,"stream",NULL};
    uint64_t end;
    int ok=1;
    CHECK(runner_init(&r)==RUN_OK);
    CHECK(runner_start(&r,base,1,&req,args)==RUN_OK);
    CHECK(wait_all(&r,&c));
    end=runner_now_ms();
    CHECK(c.bytes==28&&c.first_ms>0);
    CHECK(end-c.first_ms>=500);
    CHECK(runner_find(&r,base,1)->state==COMPLETED);
done:
    runner_free(&r);
    return ok;
}

static int invalid(void)
{
    struct runner r;
    struct job_req req=request();
    char *args[]={(char *)program,"hello",NULL};
    char dir[128];
    int ok=1;
    CHECK(runner_init(&r)==RUN_OK);
    CHECK(runner_start(&r,0,1,&req,args)==RUN_BAD_INPUT);
    CHECK(runner_start(&r,base,0,&req,args)==RUN_BAD_INPUT);
    CHECK(runner_start(&r,base,1,&req,NULL)==RUN_BAD_INPUT);
    req.need.ram=LLONG_MAX;
    CHECK(runner_start(&r,base,1,&req,args)==RUN_BAD_INPUT);
    req=request(); req.time_limit=0;
    CHECK(runner_start(&r,base,1,&req,args)==RUN_BAD_INPUT);
    req=request(); memset(req.path,'a',sizeof(req.path));
    CHECK(runner_start(&r,base,1,&req,args)==RUN_BAD_INPUT);
    req=request();
    strcpy(req.path,"/osn_missing_program_for_runner_test");
    CHECK(runner_start(&r,base,1,&req,args)==RUN_IO);
    snprintf(dir,sizeof(dir),"/tmp/osn_job_%d",base);
    CHECK(access(dir,F_OK)<0&&errno==ENOENT);
    CHECK(r.head==NULL);
done:
    runner_free(&r);
    return ok;
}

int main(int argc,char **argv)
{
    char path[PATH_MAX];
    size_t i;
    int ran=0,failed=0;
    struct { const char *name; int (*run)(void); } tests[]={
        {"concurrent",concurrent},{"backpressure",backpressure},
        {"cancel",cancel},{"live_output",live_output},{"invalid",invalid}
    };
    if(argc<2||argc>3||!realpath(argv[1],path)||strlen(path)>=PATH_LEN){
        puts("usage: test_runner <fixture-path> [test-name]");
        return 1;
    }
    program=path;
    base=200000+(int)(getpid()%1000000)*10;
    for(i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        if(argc==3&&strcmp(argv[2],tests[i].name)) continue;
        ran++;
        if(tests[i].run()) printf("PASS worker %s\n",tests[i].name);
        else failed++;
    }
    if(!ran){ puts("unknown test name"); return 1; }
    printf("%d worker groups, %d checks, %d failed\n",ran,checks,failed);
    return failed?1:0;
}
