#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "discovery.h"
#include "logging.h"
#include "node.h"

static volatile sig_atomic_t drain_requested,abort_requested;
static void interrupt(int sig)
{
    if(sig==SIGTERM) abort_requested=1;
    drain_requested=1;
}

static int connect_to(const struct worker_endpoint *ep)
{
    struct sockaddr_in addr={0};
    struct pollfd p;
    int fd,flags,err=0;
    socklen_t len=sizeof(err);
    uint64_t end=runner_now_ms()+5000;
    fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -1;
    flags=fcntl(fd,F_GETFL);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0||fcntl(fd,F_SETFD,FD_CLOEXEC)<0) goto fail;
    addr.sin_family=AF_INET; addr.sin_port=htons((uint16_t)ep->port);
    if(inet_pton(AF_INET,ep->ip,&addr.sin_addr)!=1) goto fail;
    if(connect(fd,(struct sockaddr *)&addr,sizeof(addr))==0) return fd;
    if(errno!=EINPROGRESS) goto fail;
    p=(struct pollfd){fd,POLLOUT,0};
    while(runner_now_ms()<end&&!abort_requested&&!drain_requested){
        int ret=poll(&p,1,100);
        if(ret<0&&errno==EINTR) continue;
        if(ret<0) break;
        if(ret>0){
            if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len)<0||err){ if(err) errno=err; break; }
            return fd;
        }
    }
fail:
    close(fd); return -1;
}

static void io(struct worker_loop *loop,int fd,short events,void *arg)
{
    struct worker_node *n=arg;
    int err;
    if(fd==STDIN_FILENO){
        char buf[64];
        ssize_t count=read(fd,buf,sizeof(buf));
        if(count<=0){ if(!count||errno!=EAGAIN) worker_loop_watch(loop,WORKER_ADMIN,-1,0); return; }
        for(ssize_t i=0;i<count;i++){
            if(buf[i]=='\n'){
                n->admin[n->admin_len]=0;
                if(!strcmp(n->admin,"drain")) n->draining=1;
                else if(n->admin_len) log_line("WORKER","Local command: drain");
                n->admin_len=0;
            }else if(buf[i]!='\r'){
                if(n->admin_len+1<sizeof(n->admin)) n->admin[n->admin_len++]=buf[i];
                else n->admin_len=0;
            }
        }
        return;
    }
    if(events&POLLNVAL) goto lost;
    if(events&(POLLIN|POLLHUP|POLLERR)){
        err=transport_read(&n->trans,worker_message,n);
        if(err!=TRANSPORT_OK&&err!=TRANSPORT_WOULD_BLOCK) goto lost;
    }
    if(events&POLLOUT){
        size_t pending=n->trans.pending_len-n->trans.pending_offset;
        err=transport_flush(&n->trans);
        n->tx_sent+=pending-(n->trans.pending_len-n->trans.pending_offset);
        if(err!=TRANSPORT_OK&&err!=TRANSPORT_WOULD_BLOCK) goto lost;
    }
    if(worker_loop_watch(loop,WORKER_CONN,fd,POLLIN|(transport_wants_write(&n->trans)?POLLOUT:0))<0) goto lost;
    return;
lost:
    n->lost=1; worker_loop_stop(loop);
}

static void tick(struct worker_loop *loop,uint64_t now,void *arg)
{
    struct worker_node *n=arg;
    if(abort_requested){ worker_loop_stop(loop); return; }
    if(drain_requested) n->draining=1;
    worker_tick(loop,now,n);
}

static uint64_t cache_limit(void)
{
    const char *s=getenv("OSN_CACHE_SIZE_MB");
    char *end;
    unsigned long long value;
    if(!s) return 512u*1048576u;
    if(!*s||strspn(s,"0123456789")!=strlen(s)) return 0;
    errno=0; value=strtoull(s,&end,10);
    if(errno||*end||!value||value>UINT64_MAX/1048576) return 0;
    return (uint64_t)value*1048576;
}

int main(int argc,char **argv)
{
    struct worker_node n={0};
    struct resource physical;
    struct worker_endpoint ep;
    struct worker_hooks hooks={.io=io,.tick=tick,.output=worker_output,.result=worker_result,.start=worker_started};
    unsigned char reg[18]={0};
    char error[256],default_dir[80];
    const char *dir=getenv("OSN_CACHE_DIR");
    int status=0,stdin_flags=fcntl(STDIN_FILENO,F_GETFL);
    n.fd=-1;
    if(worker_config_parse(argc,argv,&n.cfg,error,sizeof(error))<0){ fprintf(stderr,"%s\n",error); return 1; }
    if(worker_capacity_detect(&physical)<0||worker_capacity_limit(&n.cfg,physical,error,sizeof(error))<0){ perror("worker capacity"); return 1; }
    if(error[0]) log_line("WORKER","%s",error);
    if(!dir){ snprintf(default_dir,sizeof(default_dir),"/tmp/osn_cache_%ld",(long)geteuid()); dir=default_dir; }
    if(worker_cache_init(&n.cache,dir,cache_limit())<0){ perror("worker cache"); return 1; }
    n.cache.changed=worker_cache_changed; n.cache.arg=&n;
    signal(SIGPIPE,SIG_IGN); signal(SIGINT,interrupt); signal(SIGTERM,interrupt);
    while(!abort_requested&&!drain_requested){
        log_line("WORKER","Discovering scheduler on UDP %d",n.cfg.discovery_port);
        if(worker_find_scheduler_cancel(&n.cfg,&ep,&drain_requested)!=DISCOVERY_OK){
            if(abort_requested||drain_requested) break;
            log_line("WORKER","Discovery failed; retrying");
            poll(NULL,0,500); continue;
        }
        if(abort_requested||drain_requested) break;
        log_line("WORKER","%s %s:%d",ep.used_fallback?"Fallback":"Discovered",ep.ip,ep.port);
        n.fd=connect_to(&ep);
        if(n.fd<0){ if(!abort_requested&&!drain_requested) poll(NULL,0,500); continue; }
        if(transport_init(&n.trans,n.fd,1)!=TRANSPORT_OK){ close(n.fd); status=1; break; }
        if(worker_loop_init(&n.loop,&hooks,&n)!=RUN_OK){ transport_free(&n.trans); status=1; break; }
        n.worker_id=n.lost=n.draining=n.drain_sent=0; n.heartbeat=n.tx_sent=0; n.registered_ms=runner_now_ms();
        worker_p32(reg,(uint32_t)n.cfg.capacity.cores); worker_p64(reg+4,(uint64_t)n.cfg.capacity.ram);
        worker_p32(reg+12,(uint32_t)n.cfg.capacity.gpu);
        struct protocol_message message={WORKER_REGISTER,1,(const char *)reg,sizeof(reg)};
        if(worker_loop_watch(&n.loop,WORKER_CONN,n.fd,POLLIN|POLLOUT)<0||transport_queue(&n.trans,&message)!=TRANSPORT_OK){
            n.lost=1;
        }else{
            // Stdin optional hai; redirected EOF ko loop se remove kar denge.
            worker_loop_watch(&n.loop,WORKER_ADMIN,STDIN_FILENO,POLLIN);
            if(worker_loop_run(&n.loop)<0){ perror("worker loop"); n.lost=1; }
        }
        int drained=n.draining&&!n.lost;
        n.worker_id=0;
        // Reconnect se pehle purane children kill/reap + directories clean hona zaroori hai.
        worker_loop_free(&n.loop); worker_tasks_free(&n); transport_free(&n.trans); n.fd=-1;
        if(drained||abort_requested||drain_requested) break;
        log_line("WORKER","Connection lost; local jobs stopped, rediscovering");
        poll(NULL,0,500);
    }
    worker_cache_free(&n.cache);
    if(stdin_flags>=0) (void)fcntl(STDIN_FILENO,F_SETFL,stdin_flags);
    return status;
}
