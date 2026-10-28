#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "scheduler_loop.h"

uint64_t sched_now_ms(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)<0) return 0;
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}

int sched_loop_init(struct sched_loop *loop,struct scheduler *s,
                    const struct sched_hooks *hooks,void *arg)
{
    if(!loop||!s||!hooks){ errno=EINVAL; return -1; }
    memset(loop,0,sizeof(*loop));
    loop->s=s;
    loop->hooks=*hooks;
    loop->arg=arg;
    loop->next_tag=1;
    loop->next_tick=sched_now_ms()+1000;
    loop->dirty=1;
    return 0;
}

static struct sched_watch *find_watch(struct sched_loop *loop,int fd)
{
    size_t i;
    for(i=0;i<loop->count;i++) if(loop->watch[i].fd==fd) return &loop->watch[i];
    return NULL;
}

static int events_valid(short events)
{
    return events&&(events&~(POLLIN|POLLOUT))==0;
}

int sched_loop_add(struct sched_loop *loop,int fd,short events)
{
    struct sched_watch *ptr;
    size_t cap;
    int flags;
    if(!loop||fd<0||!events_valid(events)||!loop->hooks.io){ errno=EINVAL; return -1; }
    if(find_watch(loop,fd)){ errno=EEXIST; return -1; }
    if(loop->next_tag==UINT64_MAX){ errno=EOVERFLOW; return -1; }
    if(loop->count==loop->cap){
        if(loop->cap>SIZE_MAX/2/sizeof(*ptr)){ errno=ENOMEM; return -1; }
        cap=loop->cap?loop->cap*2:16;
        ptr=realloc(loop->watch,cap*sizeof(*ptr));
        if(!ptr) return -1;
        loop->watch=ptr;
        loop->cap=cap;
    }
    flags=fcntl(fd,F_GETFL);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) return -1;
    flags=fcntl(fd,F_GETFD);
    if(flags<0||fcntl(fd,F_SETFD,flags|FD_CLOEXEC)<0) return -1;
    loop->watch[loop->count++]=(struct sched_watch){fd,events,loop->next_tag++};
    return 0;
}

int sched_loop_update(struct sched_loop *loop,int fd,short events)
{
    struct sched_watch *ptr;
    if(!loop||!events_valid(events)){ errno=EINVAL; return -1; }
    ptr=find_watch(loop,fd);
    if(!ptr){ errno=ENOENT; return -1; }
    ptr->events=events;
    return 0;
}

int sched_loop_remove(struct sched_loop *loop,int fd)
{
    size_t i;
    if(!loop){ errno=EINVAL; return -1; }
    for(i=0;i<loop->count;i++){
        if(loop->watch[i].fd!=fd) continue;
        // Last entry ko yahan la do; fd ka order public API ka part nahi hai.
        loop->watch[i]=loop->watch[--loop->count];
        return 0;
    }
    errno=ENOENT;
    return -1;
}

void sched_loop_request(struct sched_loop *loop)
{
    if(loop) loop->dirty=1;
}

void sched_loop_stop(struct sched_loop *loop)
{
    if(loop) loop->stop=1;
}

int sched_loop_step(struct sched_loop *loop,int wait_ms)
{
    struct pollfd *fds=NULL;
    struct sched_watch *curr;
    uint64_t *tags=NULL,now,left;
    size_t i,count;
    int ready,timeout,err;
    short events;
    if(!loop||wait_ms< -1){ errno=EINVAL; return -1; }
    if(loop->stepping){ errno=EBUSY; return -1; }
    if(loop->stop) return 0;
    count=loop->count;
    if(count){
        fds=calloc(count,sizeof(*fds));
        tags=calloc(count,sizeof(*tags));
        if(!fds||!tags){ free(fds); free(tags); return -1; }
        for(i=0;i<count;i++){
            fds[i].fd=loop->watch[i].fd;
            fds[i].events=loop->watch[i].events;
            tags[i]=loop->watch[i].tag;
        }
    }
    now=sched_now_ms();
    left=loop->next_tick>now?loop->next_tick-now:0;
    timeout=(int)(left>INT_MAX?INT_MAX:left);
    if(wait_ms>=0&&wait_ms<timeout) timeout=wait_ms;
    if(loop->dirty) timeout=0;
    loop->stepping=1;
    ready=poll(fds,(nfds_t)count,timeout);
    if(ready<0&&errno!=EINTR){
        err=errno;
        loop->stepping=0;
        free(fds); free(tags);
        errno=err;
        return -1;
    }
    for(i=0;ready>0&&i<count&&!loop->stop;i++){
        if(!fds[i].revents) continue;
        // Callback kisi aur fd ko remove/close karke same number reuse kar
        // sakta hai. Purane poll result ko nayi connection pe mat lagao.
        curr=find_watch(loop,fds[i].fd);
        if(!curr||curr->tag!=tags[i]) continue;
        events=fds[i].revents&(curr->events|POLLERR|POLLHUP|POLLNVAL);
        if(events) loop->hooks.io(loop,fds[i].fd,events,loop->arg);
    }
    free(fds); free(tags);
    now=sched_now_ms();
    if(!loop->stop&&now>=loop->next_tick){
        // Missed ticks ke liye catch-up loop nahi; ek callback latest time pe.
        // Heavy callback ko fix karna padega, backlog of timer callbacks nahi.
        loop->next_tick=now+1000;
        if(loop->hooks.tick) loop->hooks.tick(loop,now,loop->arg);
        loop->dirty=1;
    }
    if(!loop->stop&&loop->dirty){
        loop->dirty=0;
        if(loop->hooks.schedule) loop->hooks.schedule(loop,now,loop->arg);
    }
    loop->stepping=0;
    return 0;
}

int sched_loop_run(struct sched_loop *loop)
{
    if(!loop){ errno=EINVAL; return -1; }
    while(!loop->stop) if(sched_loop_step(loop,-1)<0) return -1;
    return 0;
}

void sched_loop_free(struct sched_loop *loop)
{
    if(!loop) return;
    // Fds caller ke hain: remove/close usko hi karna hai. Registry bhi
    // caller own karta hai; yahan sirf watch array release kar rahe hain.
    if(loop->stepping){ errno=EBUSY; return; }
    free(loop->watch);
    memset(loop,0,sizeof(*loop));
}
