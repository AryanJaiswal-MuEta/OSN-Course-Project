#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include "worker_loop.h"

int worker_loop_init(struct worker_loop *loop,const struct worker_hooks *hooks,void *arg)
{
    int err;
    if(!loop||!hooks) return RUN_BAD_INPUT;
    memset(loop,0,sizeof(*loop));
    loop->watch[0].fd=loop->watch[1].fd=-1;
    err=runner_init(&loop->runner);
    if(err!=RUN_OK) return err;
    loop->hooks=*hooks;
    loop->arg=arg;
    loop->next_tag=1;
    loop->next_poll=runner_now_ms();
    return RUN_OK;
}

int worker_loop_watch(struct worker_loop *loop,int slot,int fd,short events)
{
    int flags;
    if(!loop||slot<0||slot>1||fd< -1){ errno=EINVAL; return -1; }
    if(loop->next_tag==UINT64_MAX){ errno=EOVERFLOW; return -1; }
    if(fd>=0){
        if(!loop->hooks.io||!events||(events&~(POLLIN|POLLOUT))){ errno=EINVAL; return -1; }
        if(loop->watch[1-slot].fd==fd){ errno=EEXIST; return -1; }
        flags=fcntl(fd,F_GETFL);
        if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) return -1;
        flags=fcntl(fd,F_GETFD);
        if(flags<0||fcntl(fd,F_SETFD,flags|FD_CLOEXEC)<0) return -1;
    }
    loop->watch[slot]=(struct worker_watch){fd,fd>=0?events:0,loop->next_tag++};
    return 0;
}

int worker_loop_assign(struct worker_loop *loop,int id,uint64_t run_id,
                       const struct job_req *req,char *const args[])
{
    int err;
    if(!loop) return RUN_BAD_INPUT;
    if(loop->stop) return RUN_BUSY;
    // File transport complete hone ke baad yahan aana hai. Local runner
    // job directory aur executable copy pehle se handle karta hai.
    err=runner_start(&loop->runner,id,run_id,req,args);
    if(err==RUN_OK) loop->next_poll=runner_now_ms();
    return err;
}

int worker_loop_cancel(struct worker_loop *loop,int id,uint64_t run_id)
{
    int err;
    if(!loop) return RUN_BAD_INPUT;
    err=runner_cancel(&loop->runner,id,run_id);
    if(err==RUN_OK) loop->next_poll=runner_now_ms();
    return err;
}

static int poll_jobs(struct worker_loop *loop)
{
    struct run_job *curr,*next;
    struct worker_result result;
    int err;
    if(runner_poll(&loop->runner,loop->hooks.output,loop->arg)<0) return -1;
    for(curr=loop->runner.head;curr&&!loop->stop;curr=next){
        next=curr->next;
        if(!curr->finished||!loop->hooks.result) continue;
        memset(&result,0,sizeof(result));
        result.id=curr->id;
        result.run_id=curr->run_id;
        result.state=curr->state;
        result.usage=curr->usage;
        memcpy(result.reason,curr->reason,sizeof(result.reason));
        // Output callback sab bytes accept kar chuka hai. Result pe bhi
        // backpressure ho sakta hai; reject hua toh files abhi nahi hatengi.
        if(loop->hooks.result(&result,loop->arg)!=0) continue;
        err=runner_remove(&loop->runner,result.id,result.run_id);
        if(err!=RUN_OK){ errno=EIO; return -1; }
    }
    return 0;
}

int worker_loop_step(struct worker_loop *loop,int wait_ms)
{
    struct pollfd fds[2];
    uint64_t tags[2],now,left;
    short events;
    int i,ready,timeout,err;
    if(!loop||wait_ms< -1){ errno=EINVAL; return -1; }
    if(loop->stepping){ errno=EBUSY; return -1; }
    if(loop->stop) return 0;
    for(i=0;i<2;i++){
        fds[i]=(struct pollfd){loop->watch[i].fd,loop->watch[i].events,0};
        tags[i]=loop->watch[i].tag;
    }
    now=runner_now_ms();
    left=loop->next_poll>now?loop->next_poll-now:0;
    timeout=(int)(left>INT_MAX?INT_MAX:left);
    if(wait_ms>=0&&wait_ms<timeout) timeout=wait_ms;
    loop->stepping=1;
    ready=poll(fds,2,timeout);
    if(ready<0&&errno!=EINTR) goto fail;
    for(i=0;ready>0&&i<2&&!loop->stop;i++){
        if(!fds[i].revents||loop->watch[i].tag!=tags[i]) continue;
        events=fds[i].revents&(loop->watch[i].events|POLLERR|POLLHUP|POLLNVAL);
        if(events) loop->hooks.io(loop,fds[i].fd,events,loop->arg);
    }
    now=runner_now_ms();
    if(!loop->stop&&now>=loop->next_poll){
        loop->next_poll=now+100;
        if(poll_jobs(loop)<0) goto fail;
        if(!loop->stop&&loop->hooks.tick) loop->hooks.tick(loop,now,loop->arg);
    }
    loop->stepping=0;
    return 0;
fail:
    err=errno;
    loop->stepping=0;
    errno=err;
    return -1;
}

int worker_loop_run(struct worker_loop *loop)
{
    if(!loop){ errno=EINVAL; return -1; }
    while(!loop->stop) if(worker_loop_step(loop,-1)<0) return -1;
    return 0;
}

void worker_loop_stop(struct worker_loop *loop)
{
    if(loop) loop->stop=1;
}

void worker_loop_free(struct worker_loop *loop)
{
    if(!loop) return;
    if(loop->stepping){ errno=EBUSY; return; }
    // Connection/admin fds caller ke hain. Abort pe child kill/reap runner
    // karega; pending network output shutdown pe deliver nahi hota.
    runner_free(&loop->runner);
    memset(loop,0,sizeof(*loop));
    loop->watch[0].fd=loop->watch[1].fd=-1;
}
