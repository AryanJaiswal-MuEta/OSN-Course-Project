#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "discovery.h"
#include "logging.h"
#include "server.h"

static struct sched_loop *active;
static void stop(int sig){ (void)sig; if(active) sched_loop_stop(active); }

void server_log(const char *msg,void *arg)
{
    (void)arg;
    log_line("SCHEDULER","%s",msg);
}
static void changed(void *arg){ sched_loop_request(&((struct server *)arg)->loop); }

static void accept_peer(struct server *ctx)
{
    size_t i;
    int fd,count;
    struct peer *p;
    // Ek busy accept queue bhi timer ko starve na kare: per pass 32 accepts max.
    for(count=0;count<32;count++){
        fd=accept(ctx->listen_fd,NULL,NULL);
        if(fd<0){ if(errno==EINTR) continue; return; }
        for(i=0;i<MAX_PEERS&&ctx->peers[i].fd>=0;i++){}
        if(i==MAX_PEERS){ close(fd); continue; }
        p=&ctx->peers[i]; memset(p,0,sizeof(*p));
        p->fd=fd; p->ctx=ctx; p->connected_ms=sched_now_ms();
        if(transport_init(&p->trans,fd,0)!=TRANSPORT_OK||
           sched_loop_add(&ctx->loop,fd,POLLIN)<0) server_close(p);
    }
}

static void discovery(struct server *ctx)
{
    unsigned char buf[DISCOVERY_MAX_PACKET];
    struct sockaddr_in addr;
    socklen_t len=sizeof(addr);
    struct discovery_packet packet;
    char ip[INET_ADDRSTRLEN];
    ssize_t n=recvfrom(ctx->udp_fd,buf,sizeof(buf),0,(struct sockaddr *)&addr,&len);
    if(n!=16||discovery_decode(buf,(size_t)n,&packet)!=DISCOVERY_OK||packet.kind!=DISCOVERY_REQUEST) return;
    packet.kind=DISCOVERY_RESPONSE; packet.port=(uint16_t)ctx->port;
    if(inet_ntop(AF_INET,&addr.sin_addr,ip,sizeof(ip)))
        discovery_send(ctx->udp_fd,ip,ntohs(addr.sin_port),&packet);
}

static void on_io(struct sched_loop *loop,int fd,short events,void *arg)
{
    struct server *ctx=arg;
    struct peer *p=NULL;
    size_t i;
    int err;
    if(fd==ctx->listen_fd){ if(events&POLLIN) accept_peer(ctx); return; }
    if(fd==ctx->udp_fd){ if(events&POLLIN) discovery(ctx); return; }
    for(i=0;i<MAX_PEERS;i++) if(ctx->peers[i].fd==fd){ p=&ctx->peers[i]; break; }
    if(!p) return;
    if(events&POLLNVAL){ server_close(p); return; }
    if(events&(POLLIN|POLLHUP|POLLERR)){
        err=transport_read(&p->trans,server_message,p);
        if(err!=TRANSPORT_OK&&err!=TRANSPORT_WOULD_BLOCK){ server_close(p); return; }
    }
    if(p->failed){ server_close(p); return; }
    if(events&POLLOUT){
        err=transport_flush(&p->trans);
        if(err!=TRANSPORT_OK&&err!=TRANSPORT_WOULD_BLOCK){ server_close(p); return; }
    }
    sched_loop_update(loop,fd,POLLIN|(transport_wants_write(&p->trans)?POLLOUT:0));
}

static void on_tick(struct sched_loop *loop,uint64_t now,void *arg)
{
    struct server *ctx=arg;
    struct worker *w;
    struct job *j;
    size_t i;
    (void)loop;
    sched_tick(&ctx->sched,now);
    // Heartbeat aata rahe par start/stop ack na aaye toh session fence karo.
    // Timeout pe akeli reservation release karna remote overcommit kara sakta hai.
    for(j=ctx->sched.jobs;j;j=j->next){
        if(!j->worker_id) continue;
        if((j->dispatch_pending&&now-j->dispatch_ms>=30000)||
           (j->stop_pending&&now-j->stop_ms>=10000)){
            for(i=0;i<MAX_PEERS;i++)
                if(ctx->peers[i].fd>=0&&ctx->peers[i].worker_id==j->worker_id){ server_close(&ctx->peers[i]); break; }
        }
    }
    for(i=0;i<MAX_PEERS;i++){
        struct peer *p=&ctx->peers[i];
        if(p->fd<0) continue;
        w=p->worker_id?worker_find(&ctx->sched,p->worker_id):NULL;
        if(p->failed||(w&&w->state==DEAD)||
           (!p->worker_id&&!p->user[0]&&now-p->connected_ms>=10000)) server_close(p);
    }
}

static void on_schedule(struct sched_loop *loop,uint64_t now,void *arg)
{
    struct server *ctx=arg;
    struct sched_pick pick;
    struct job *j;
    uint64_t run;
    size_t i;
    int err;
    (void)loop;
    // Quota skips ek pass mein ek baar log karo, har inner pick pe nahi.
    for(j=ctx->sched.jobs;j;j=j->next)
        if(sched_job_eligible(&ctx->sched,j)&&sched_quota_skip(&ctx->sched,j))
            sched_note(&ctx->sched,"Job %d skipped: %s holds over 50%% cores while another user waits",j->id,j->req.user);
    sched_preempt_next(&ctx->sched,ctx->policy,now);
    while((err=sched_pick_next(&ctx->sched,ctx->policy,now,&pick))==S_OK){
        if(ctx->sched.next_run>UINT32_MAX){
            sched_note(&ctx->sched,"v1 wire execution token range exhausted"); break;
        }
        for(i=0;i<MAX_PEERS;i++)
            if(ctx->peers[i].fd>=0&&!ctx->peers[i].failed&&ctx->peers[i].worker_id==pick.worker_id) break;
        if(i==MAX_PEERS){ sched_worker_dead(&ctx->sched,pick.worker_id,now); continue; }
        if(job_reserve(&ctx->sched,pick.job_id,pick.worker_id,now,&run)!=S_OK) break;
        j=job_find(&ctx->sched,pick.job_id);
        if(server_assign(&ctx->peers[i],j)<0){
            // Queue partial ho sakti hai; whole session fence karke hi reservation chhodo.
            server_close(&ctx->peers[i]);
        }
    }
    if(err==S_NO_RESOURCE) sched_preempt_next(&ctx->sched,ctx->policy,now);
    server_stops(ctx);
    for(i=0;i<MAX_PEERS;i++) if(ctx->peers[i].fd>=0&&ctx->peers[i].failed) server_close(&ctx->peers[i]);
}

static int port_num(const char *s)
{
    char *end;
    long n;
    if(!s||!*s||strspn(s,"0123456789")!=strlen(s)) return -1;
    errno=0; n=strtol(s,&end,10);
    return errno||*end||n<1||n>65535?-1:(int)n;
}

static int bind_socket(int fd,int port)
{
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr)); addr.sin_family=AF_INET;
    addr.sin_port=htons((uint16_t)port); addr.sin_addr.s_addr=htonl(INADDR_ANY);
    return bind(fd,(struct sockaddr *)&addr,sizeof(addr));
}

int main(int argc,char **argv)
{
    struct server *ctx;
    struct sched_hooks hooks={on_io,on_tick,on_schedule};
    int port,disc=9999,opt=1,err=1;
    size_t i;
    if((argc!=3&&argc!=5)||(port=port_num(argv[1]))<0||
       (strcmp(argv[2],"FIFO")&&strcmp(argv[2],"PRIORITY"))||
       (argc==5&&(strcmp(argv[3],"-discovery-port")||(disc=port_num(argv[4]))<0))){
        fprintf(stderr,"Usage: %s <port> <FIFO|PRIORITY> [-discovery-port <port>]\n",argv[0]); return 1;
    }
    ctx=calloc(1,sizeof(*ctx));
    if(!ctx){ perror("scheduler allocation"); return 1; }
    ctx->listen_fd=ctx->udp_fd=-1; ctx->port=port;
    ctx->policy=!strcmp(argv[2],"FIFO")?SCHED_FIFO_ORDER:SCHED_PRIORITY_ORDER;
    for(i=0;i<MAX_PEERS;i++) ctx->peers[i].fd=-1;
    sched_init(&ctx->sched); ctx->sched.event=server_log;
    sched_service_init(&ctx->svc,&ctx->sched,changed,NULL,ctx);
    if(sched_loop_init(&ctx->loop,&ctx->sched,&hooks,ctx)<0) goto done;
    ctx->listen_fd=socket(AF_INET,SOCK_STREAM,0);
    if(ctx->listen_fd<0) goto done;
    if(setsockopt(ctx->listen_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt))<0||
       bind_socket(ctx->listen_fd,port)<0||listen(ctx->listen_fd,64)<0) goto done;
    ctx->udp_fd=discovery_socket(1,0);
    if(ctx->udp_fd<0||bind_socket(ctx->udp_fd,disc)<0) goto done;
    if(sched_loop_add(&ctx->loop,ctx->listen_fd,POLLIN)<0||
       sched_loop_add(&ctx->loop,ctx->udp_fd,POLLIN)<0) goto done;
    active=&ctx->loop;
    signal(SIGINT,stop); signal(SIGTERM,stop); signal(SIGPIPE,SIG_IGN);
    sched_note(&ctx->sched,"Running: TCP=%d discovery=%d policy=%s",port,disc,argv[2]);
    err=sched_loop_run(&ctx->loop)<0?1:0;
done:
    if(err) perror("scheduler");
    active=NULL;
    for(i=0;i<MAX_PEERS;i++) if(ctx->peers[i].fd>=0) server_close(&ctx->peers[i]);
    if(ctx->listen_fd>=0) close(ctx->listen_fd);
    if(ctx->udp_fd>=0) close(ctx->udp_fd);
    sched_loop_free(&ctx->loop); sched_free(&ctx->sched); free(ctx);
    return err;
}
