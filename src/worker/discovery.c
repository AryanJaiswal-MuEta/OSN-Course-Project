#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "discovery.h"
#include "worker_discovery.h"

static int random_nonce(uint64_t *nonce)
{
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    size_t off=0;
    ssize_t n;
    if(fd<0) return -1;
    while(off<sizeof(*nonce)){
        n=read(fd,(char *)nonce+off,sizeof(*nonce)-off);
        if(n<0&&errno==EINTR) continue;
        if(n<=0){ close(fd); return -1; }
        off+=(size_t)n;
    }
    close(fd);
    return 0;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)<0) return -1;
    return (int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
}

static int wait_reply(int fd,uint64_t nonce,struct worker_endpoint *out)
{
    struct discovery_packet reply;
    struct sockaddr_in from;
    struct pollfd p={fd,POLLIN,0};
    unsigned char buf[DISCOVERY_MAX_PACKET];
    struct worker_endpoint ep={0};
    socklen_t len;
    int64_t now=now_ms(),end;
    ssize_t n;
    int ready;
    if(now<0) return DISCOVERY_IO;
    end=now+2000;
    for(;;){
        now=now_ms();
        if(now<0) return DISCOVERY_IO;
        if(now>=end) return DISCOVERY_TIMEOUT;
        ready=poll(&p,1,(int)(end-now));
        if(ready<0&&errno==EINTR) continue;
        if(ready<0) return DISCOVERY_IO;
        if(!ready) return DISCOVERY_TIMEOUT;
        if(!(p.revents&POLLIN)) return DISCOVERY_IO;
        len=sizeof(from);
        n=recvfrom(fd,buf,sizeof(buf),MSG_DONTWAIT,(struct sockaddr *)&from,&len);
        if(n<0){
            if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK) continue;
            return DISCOVERY_IO;
        }
        // Wrong nonce/packet se retry ka timer reset nahi hoga.
        if(len<sizeof(from)||from.sin_family!=AF_INET||
           discovery_decode(buf,(size_t)n,&reply)!=DISCOVERY_OK||
           reply.kind!=DISCOVERY_RESPONSE||reply.nonce!=nonce||!reply.port) continue;
        if(!inet_ntop(AF_INET,&from.sin_addr,ep.ip,sizeof(ep.ip))) return DISCOVERY_IO;
        ep.port=reply.port;
        *out=ep;
        return DISCOVERY_OK;
    }
}

int worker_find_scheduler(const struct worker_config *cfg,struct worker_endpoint *out)
{
    struct discovery_packet req={.kind=DISCOVERY_REQUEST};
    struct worker_endpoint ep={0};
    struct in_addr ip;
    int fd,i,ret=DISCOVERY_IO;
    if(!cfg||!out||cfg->discovery_port<1||cfg->discovery_port>65535)
        return DISCOVERY_BAD_INPUT;
    if(cfg->has_fallback&&(!memchr(cfg->scheduler_ip,0,sizeof(cfg->scheduler_ip))||
       inet_pton(AF_INET,cfg->scheduler_ip,&ip)!=1||
       cfg->scheduler_port<1||cfg->scheduler_port>65535)) return DISCOVERY_BAD_INPUT;
    fd=discovery_socket(0,1);
    if(fd>=0){
        for(i=0;i<3;i++){
            if(random_nonce(&req.nonce)<0){ ret=DISCOVERY_IO; break; }
            ret=discovery_send(fd,"255.255.255.255",(uint16_t)cfg->discovery_port,&req);
            if(ret==DISCOVERY_OK) ret=wait_reply(fd,req.nonce,out);
            if(ret==DISCOVERY_OK) break;
        }
        close(fd);
    }
    if(ret==DISCOVERY_OK||!cfg->has_fallback) return ret;
    // Broadcast attempts khatam, ab configured endpoint use kar sakte hain.
    strcpy(ep.ip,cfg->scheduler_ip);
    ep.port=cfg->scheduler_port;
    ep.used_fallback=1;
    *out=ep;
    return DISCOVERY_OK;
}
