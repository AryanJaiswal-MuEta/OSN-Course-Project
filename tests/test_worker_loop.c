#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "worker_loop.h"

static int checks,base;
static const char *program;

#define CHECK(expr) do { checks++; if(!(expr)){ \
    printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); ok=0; goto done; \
} } while(0)

struct context {
    size_t bytes;
    int block_output;
    int block_result;
    int result_tries;
    int results;
    int ticks;
    int error;
    int fd;
    int cancel_id;
    int seen_conn,seen_admin;
    int replace;
    struct worker_result last;
};

static struct job_req request(void)
{
    struct job_req req;
    memset(&req,0,sizeof(req));
    strcpy(req.user,"local");
    strcpy(req.path,program);
    req.need=(struct resource){1,64,0};
    req.prio=2;
    req.time_limit=5;
    return req;
}

static int output(int id,uint64_t run,const void *buf,size_t len,void *arg)
{
    struct context *c=arg;
    (void)id; (void)run; (void)buf;
    if(c->block_output) return 1;
    c->bytes+=len;
    return 0;
}

static int result(const struct worker_result *r,void *arg)
{
    struct context *c=arg;
    c->result_tries++;
    if(c->block_result) return 1;
    c->last=*r;
    c->results++;
    return 0;
}

static void tick(struct worker_loop *loop,uint64_t now,void *arg)
{
    struct context *c=arg;
    (void)now;
    c->ticks++;
    if(worker_loop_step(loop,0)!=-1||errno!=EBUSY) c->error=1;
}

static void io(struct worker_loop *loop,int fd,short events,void *arg)
{
    struct context *c=arg;
    char buf[64];
    ssize_t n;
    if(fd==loop->watch[WORKER_CONN].fd) c->seen_conn++;
    if(fd==loop->watch[WORKER_ADMIN].fd) c->seen_admin++;
    if(events&POLLIN){
        n=read(fd,buf,sizeof(buf));
        if(n<=0) c->error=1;
        if(c->cancel_id){
            if(worker_loop_cancel(loop,c->cancel_id,1)!=RUN_OK) c->error=1;
            c->cancel_id=0;
        }
    }
    if(c->replace&&fd==loop->watch[WORKER_CONN].fd){
        c->replace=0;
        if(worker_loop_watch(loop,WORKER_ADMIN,c->fd,POLLIN)<0) c->error=1;
    }
    if(events&(POLLHUP|POLLERR|POLLNVAL)){
        if(worker_loop_watch(loop,fd==c->fd?WORKER_ADMIN:WORKER_CONN,-1,0)<0) c->error=1;
    }
}

static const struct worker_hooks hooks={io,tick,output,result};

static int wait_empty(struct worker_loop *loop)
{
    uint64_t start=runner_now_ms();
    while(loop->runner.head&&runner_now_ms()-start<10000)
        if(worker_loop_step(loop,50)<0) return 0;
    return loop->runner.head==NULL;
}

static int delivery(void)
{
    struct worker_loop loop;
    struct context c={0};
    struct job_req req=request();
    struct run_job *j;
    struct stat st;
    uint64_t start;
    char dir[128];
    char *args[]={(char *)program,"hello",NULL};
    int ok=1;
    CHECK(worker_loop_init(&loop,&hooks,&c)==RUN_OK);
    c.block_output=c.block_result=1;
    CHECK(worker_loop_assign(&loop,base,7,&req,args)==RUN_OK);
    j=runner_find(&loop.runner,base,7);
    CHECK(j!=NULL);
    strcpy(dir,j->dir);
    CHECK(fstatat(j->dir_fd,"program",&st,0)==0&&(st.st_mode&S_IXUSR));
    start=runner_now_ms();
    while(!j->reaped&&runner_now_ms()-start<5000) CHECK(worker_loop_step(&loop,50)==0);
    CHECK(j->reaped&&!j->finished&&j->offset==0);
    CHECK(c.results==0&&c.result_tries==0);
    c.block_output=0;
    while(!j->finished&&runner_now_ms()-start<7000) CHECK(worker_loop_step(&loop,50)==0);
    CHECK(j->finished&&c.bytes>0&&c.result_tries>0);
    CHECK(access(dir,F_OK)==0&&c.results==0);
    c.block_result=0;
    CHECK(wait_empty(&loop));
    CHECK(c.results==1&&c.last.id==base&&c.last.run_id==7&&c.last.state==COMPLETED);
    CHECK(access(dir,F_OK)<0&&errno==ENOENT);
    CHECK(!c.error);
done:
    worker_loop_free(&loop);
    return ok;
}

static int concurrent(void)
{
    struct worker_loop loop;
    struct context c={0};
    struct job_req req=request();
    char *sleep_args[]={(char *)program,"sleep","5000",NULL};
    char *stream_args[]={(char *)program,"stream",NULL};
    int pair[2]={-1,-1},ok=1;
    CHECK(worker_loop_init(&loop,&hooks,&c)==RUN_OK);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    CHECK(worker_loop_watch(&loop,WORKER_CONN,pair[0],POLLIN)==0);
    CHECK(worker_loop_assign(&loop,base,1,&req,sleep_args)==RUN_OK);
    CHECK(worker_loop_assign(&loop,base+1,9,&req,stream_args)==RUN_OK);
    CHECK(worker_loop_assign(&loop,base,2,&req,sleep_args)==RUN_BUSY);
    CHECK(worker_loop_cancel(&loop,base,99)==RUN_NOT_FOUND);
    c.cancel_id=base;
    CHECK(write(pair[1],"cancel",6)==6);
    CHECK(wait_empty(&loop));
    CHECK(c.results==2&&c.bytes==28&&c.seen_conn==1&&c.ticks>1&&!c.error);
    CHECK((fcntl(pair[0],F_GETFL)&O_NONBLOCK)!=0);
    CHECK((fcntl(pair[0],F_GETFD)&FD_CLOEXEC)!=0);
done:
    worker_loop_free(&loop);
    if(pair[0]>=0) close(pair[0]);
    if(pair[1]>=0) close(pair[1]);
    return ok;
}

static int watch_reuse(void)
{
    struct worker_loop loop;
    struct context c={0};
    int a[2]={-1,-1},b[2]={-1,-1},ok=1;
    CHECK(worker_loop_init(&loop,&hooks,&c)==RUN_OK);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,a)==0);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,b)==0);
    CHECK(worker_loop_watch(&loop,WORKER_CONN,a[0],POLLIN)==0);
    CHECK(worker_loop_watch(&loop,WORKER_ADMIN,b[0],POLLIN)==0);
    c.fd=b[0]; c.replace=1;
    CHECK(worker_loop_watch(&loop,WORKER_ADMIN,a[0],POLLIN)==-1&&errno==EEXIST);
    CHECK(write(a[1],"a",1)==1&&write(b[1],"b",1)==1);
    CHECK(worker_loop_step(&loop,0)==0);
    CHECK(c.seen_conn==1&&c.seen_admin==0&&!c.error);
    CHECK(worker_loop_step(&loop,0)==0&&c.seen_admin==1);
    CHECK(worker_loop_watch(&loop,WORKER_CONN,-1,0)==0);
    CHECK(worker_loop_watch(&loop,WORKER_ADMIN,-1,0)==0);
    CHECK(worker_loop_watch(&loop,2,a[0],POLLIN)==-1&&errno==EINVAL);
    CHECK(worker_loop_step(&loop,-2)==-1&&errno==EINVAL);
done:
    worker_loop_free(&loop);
    if(a[0]>=0) close(a[0]);
    if(a[1]>=0) close(a[1]);
    if(b[0]>=0) close(b[0]);
    if(b[1]>=0) close(b[1]);
    return ok;
}

static int stop_cleanup(void)
{
    struct worker_loop loop;
    struct context c={0};
    struct job_req req=request();
    struct run_job *j;
    char *args[]={(char *)program,"sleep","5000",NULL};
    char dir[128];
    pid_t pid;
    int pair[2]={-1,-1},ok=1;
    CHECK(worker_loop_init(&loop,&hooks,&c)==RUN_OK);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    CHECK(worker_loop_watch(&loop,WORKER_CONN,pair[0],POLLIN)==0);
    CHECK(worker_loop_assign(&loop,base,1,&req,args)==RUN_OK);
    j=runner_find(&loop.runner,base,1);
    CHECK(j!=NULL);
    strcpy(dir,j->dir); pid=j->pid;
    worker_loop_stop(&loop);
    CHECK(worker_loop_assign(&loop,base+1,1,&req,args)==RUN_BUSY);
    CHECK(worker_loop_run(&loop)==0);
    worker_loop_free(&loop);
    CHECK(access(dir,F_OK)<0&&errno==ENOENT);
    CHECK(waitpid(pid,NULL,WNOHANG)==-1&&errno==ECHILD);
    CHECK(fcntl(pair[0],F_GETFD)>=0);
done:
    worker_loop_free(&loop);
    if(pair[0]>=0) close(pair[0]);
    if(pair[1]>=0) close(pair[1]);
    return ok;
}

int main(int argc,char **argv)
{
    char path[PATH_MAX];
    size_t i;
    int ran=0,failed=0;
    struct { const char *name; int (*run)(void); } tests[]={
        {"delivery",delivery},{"concurrent",concurrent},
        {"watch_reuse",watch_reuse},{"stop_cleanup",stop_cleanup}
    };
    if(argc<2||argc>3||!realpath(argv[1],path)||strlen(path)>=PATH_LEN){
        puts("usage: test_worker_loop <fixture-path> [test-name]"); return 1;
    }
    program=path;
    base=300000+(int)(getpid()%1000000)*10;
    for(i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        if(argc==3&&strcmp(argv[2],tests[i].name)) continue;
        ran++;
        if(tests[i].run()) printf("PASS worker loop %s\n",tests[i].name);
        else failed++;
    }
    printf("%d worker loop groups, %d checks, %d failed\n",ran,checks,failed);
    return !ran||failed?1:0;
}
