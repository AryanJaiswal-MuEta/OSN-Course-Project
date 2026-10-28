#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "scheduler_loop.h"
#include "scheduler_service.h"

static int checks;
#define CHECK(expr) do { checks++; if(!(expr)){ \
    printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); ok=0; goto done; \
} } while(0)

struct context {
    int a,b;
    int seen_a,seen_b;
    int reuse;
    int error;
    int ticks,schedules;
    int stop_after;
    int queries;
    size_t nodes;
    struct sched_service *svc;
    struct sched_loop *loop;
};

static void io(struct sched_loop *loop,int fd,short events,void *arg)
{
    struct context *c=arg;
    struct sched_node node;
    char buf[64];
    ssize_t n;
    if(fd==c->a) c->seen_a++;
    if(fd==c->b) c->seen_b++;
    if(events&POLLIN){
        n=read(fd,buf,sizeof(buf));
        if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK) c->error=1;
        if(c->svc&&fd==c->b&&n>0){
            c->queries++;
            if(sched_nodes_get(c->svc,&node,1,&c->nodes)!=S_OK) c->error=1;
        }
    }
    if(events&(POLLHUP|POLLERR|POLLNVAL)){
        if(sched_loop_remove(loop,fd)<0) c->error=1;
    }
    if(c->reuse&&fd==c->a){
        c->reuse=0;
        if(sched_loop_remove(loop,c->b)<0||sched_loop_add(loop,c->b,POLLIN)<0) c->error=1;
    }
    sched_loop_request(loop);
}

static void tick(struct sched_loop *loop,uint64_t now,void *arg)
{
    struct context *c=arg;
    (void)now;
    c->ticks++;
    if(sched_loop_step(loop,0)!=-1||errno!=EBUSY) c->error=1;
}

static void schedule(struct sched_loop *loop,uint64_t now,void *arg)
{
    struct context *c=arg;
    (void)now;
    c->schedules++;
    if(c->stop_after&&c->schedules>=c->stop_after) sched_loop_stop(loop);
}

static const struct sched_hooks hooks={io,tick,schedule};

static void request_schedule(void *arg)
{
    struct context *c=arg;
    sched_loop_request(c->loop);
}

static int submission_wakeup(void)
{
    struct scheduler s;
    struct sched_loop loop;
    struct sched_service svc;
    struct sched_status status;
    struct context c={0};
    struct job_req req;
    int id,ok=1;
    sched_init(&s);
    sched_loop_init(&loop,&s,&hooks,&c);
    c.loop=&loop;
    sched_service_init(&svc,&s,request_schedule,NULL,&c);
    memset(&req,0,sizeof(req));
    strcpy(req.path,"./hello");
    req.need=(struct resource){1,100,0};
    req.prio=2;
    req.time_limit=5;
    CHECK(sched_loop_step(&loop,0)==0&&c.schedules==1);
    CHECK(sched_submit(&svc,"aryan",&req,10,&id)==S_OK);
    CHECK(sched_submit(&svc,"kaush",&req,11,&id)==S_OK);
    CHECK(loop.dirty&&c.schedules==1);
    CHECK(sched_loop_step(&loop,0)==0&&c.schedules==2&&!loop.dirty);
    CHECK(sched_status_get(&svc,id,&status)==S_OK);
    CHECK(sched_loop_step(&loop,0)==0&&c.schedules==2);
    CHECK(sched_submit(&svc,"bad/name",&req,12,&id)==S_BAD_INPUT);
    CHECK(!loop.dirty);
done:
    sched_loop_free(&loop);
    sched_free(&s);
    return ok;
}

static int readiness(void)
{
    struct scheduler s;
    struct sched_loop loop;
    struct context c={0};
    int a[2]={-1,-1},b[2]={-1,-1},ok=1;
    sched_init(&s);
    sched_loop_init(&loop,&s,&hooks,&c);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,a)==0);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,b)==0);
    c.a=a[0]; c.b=b[0]; c.reuse=1;
    CHECK(sched_loop_add(&loop,a[0],POLLIN)==0);
    CHECK(sched_loop_add(&loop,b[0],POLLIN)==0);
    CHECK((fcntl(a[0],F_GETFL)&O_NONBLOCK)!=0);
    CHECK((fcntl(a[0],F_GETFD)&FD_CLOEXEC)!=0);
    CHECK(sched_loop_add(&loop,a[0],POLLIN)==-1&&errno==EEXIST);
    CHECK(write(a[1],"a",1)==1&&write(b[1],"b",1)==1);
    CHECK(sched_loop_step(&loop,0)==0);
    CHECK(c.seen_a==1&&c.seen_b==0&&!c.error);
    CHECK(sched_loop_step(&loop,0)==0);
    CHECK(c.seen_b==1&&c.schedules==2&&!c.error);
    close(b[1]); b[1]=-1;
    CHECK(sched_loop_step(&loop,10)==0);
    CHECK(loop.count==1);
    CHECK(sched_loop_remove(&loop,a[0])==0);
    CHECK(sched_loop_remove(&loop,a[0])==-1&&errno==ENOENT);
    CHECK(fcntl(a[0],F_GETFD)>=0);
done:
    sched_loop_free(&loop);
    if(a[0]>=0) close(a[0]);
    if(a[1]>=0) close(a[1]);
    if(b[0]>=0) close(b[0]);
    if(b[1]>=0) close(b[1]);
    sched_free(&s);
    return ok;
}

static int slow_peer(void)
{
    struct scheduler s;
    struct sched_loop loop;
    struct sched_service svc;
    struct context c={0};
    char buf[4096];
    ssize_t n;
    int a[2]={-1,-1},b[2]={-1,-1},wid,ok=1;
    sched_init(&s);
    sched_loop_init(&loop,&s,&hooks,&c);
    sched_service_init(&svc,&s,NULL,NULL,NULL);
    c.svc=&svc;
    CHECK(worker_add(&s,(struct resource){4,2048,0},0,&wid)==S_OK);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,a)==0);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,b)==0);
    c.a=a[0]; c.b=b[0];
    CHECK(sched_loop_add(&loop,a[0],POLLOUT)==0);
    CHECK(sched_loop_add(&loop,b[0],POLLIN)==0);
    // Peer A kuch read nahi karega. Send buffer full karke dekh rahe hain
    // ki peer B ki query phir bhi same loop se serve hoti hai ya nahi.
    memset(buf,'x',sizeof(buf));
    do { n=write(a[0],buf,sizeof(buf)); } while(n>0);
    CHECK(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK));
    CHECK(write(b[1],"nodes",5)==5);
    CHECK(sched_loop_step(&loop,20)==0);
    CHECK(c.seen_a==0&&c.queries==1&&c.nodes==1&&!c.error);
    CHECK(sched_loop_update(&loop,a[0],POLLIN)==0);
    CHECK(sched_loop_update(&loop,a[0],0)==-1&&errno==EINVAL);
done:
    sched_loop_free(&loop);
    if(a[0]>=0) close(a[0]);
    if(a[1]>=0) close(a[1]);
    if(b[0]>=0) close(b[0]);
    if(b[1]>=0) close(b[1]);
    sched_free(&s);
    return ok;
}

static int periodic(void)
{
    struct scheduler s;
    struct sched_loop loop;
    struct context c={0};
    uint64_t start;
    int ok=1;
    sched_init(&s);
    sched_loop_init(&loop,&s,&hooks,&c);
    c.stop_after=2;
    CHECK(sched_loop_step(&loop,0)==0&&c.schedules==1);
    start=sched_now_ms();
    alarm(5);
    CHECK(sched_loop_run(&loop)==0);
    alarm(0);
    CHECK(c.ticks==1&&c.schedules==2&&!c.error);
    CHECK(sched_now_ms()-start>=750&&sched_now_ms()-start<4000);
    CHECK(sched_loop_step(&loop,0)==0&&c.schedules==2);
done:
    alarm(0);
    sched_loop_free(&loop);
    sched_free(&s);
    return ok;
}

static int growth(void)
{
    struct scheduler s;
    struct sched_loop loop;
    struct context c={0};
    int pairs[24][2],i,ok=1;
    sched_init(&s);
    sched_loop_init(&loop,&s,&hooks,&c);
    for(i=0;i<24;i++) pairs[i][0]=pairs[i][1]=-1;
    for(i=0;i<24;i++){
        CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pairs[i])==0);
        CHECK(sched_loop_add(&loop,pairs[i][0],POLLIN)==0);
    }
    CHECK(loop.count==24&&loop.cap>=24);
    CHECK(sched_loop_step(&loop,0)==0);
    for(i=0;i<24;i++) CHECK(sched_loop_remove(&loop,pairs[i][0])==0);
    CHECK(loop.count==0);
    CHECK(sched_loop_add(&loop,-1,POLLIN)==-1&&errno==EINVAL);
    CHECK(sched_loop_step(&loop,-2)==-1&&errno==EINVAL);
done:
    sched_loop_free(&loop);
    for(i=0;i<24;i++){
        if(pairs[i][0]>=0) close(pairs[i][0]);
        if(pairs[i][1]>=0) close(pairs[i][1]);
    }
    sched_free(&s);
    return ok;
}

int main(int argc,char **argv)
{
    struct { const char *name; int (*run)(void); } tests[]={
        {"readiness",readiness},{"slow_peer",slow_peer},{"periodic",periodic},
        {"growth",growth},{"submission_wakeup",submission_wakeup}
    };
    size_t i;
    int ran=0,failed=0;
    if(argc>2){ puts("usage: test_loop [test-name]"); return 1; }
    for(i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        if(argc==2&&strcmp(argv[1],tests[i].name)) continue;
        ran++;
        if(tests[i].run()) printf("PASS loop %s\n",tests[i].name);
        else failed++;
    }
    printf("%d loop groups, %d checks, %d failed\n",ran,checks,failed);
    return !ran||failed?1:0;
}
