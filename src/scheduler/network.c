#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "server.h"

uint32_t wire32(const unsigned char *p)
{
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
uint64_t wire64(const unsigned char *p)
{
    uint64_t n=0;
    int i;
    for(i=0;i<8;i++) n=n<<8|p[i];
    return n;
}
void pack32(unsigned char *p,uint32_t n)
{
    int i;
    for(i=3;i>=0;i--){ p[i]=(unsigned char)n; n>>=8; }
}
void pack64(unsigned char *p,uint64_t n)
{
    int i;
    for(i=7;i>=0;i--){ p[i]=(unsigned char)n; n>>=8; }
}
static size_t wire16(const unsigned char *p){ return (size_t)p[0]*256+p[1]; }
static void pack16(unsigned char *p,size_t n){ p[0]=(unsigned char)(n>>8); p[1]=(unsigned char)n; }

int server_queue(struct peer *p,int type,uint64_t request,const void *buf,size_t size)
{
    struct protocol_message msg={(uint64_t)type,request,buf,size};
    if(p->failed||p->fd<0) return -1;
    // Slow peer ka queue bounded hai; poora cluster uske liye wait nahi karega.
    if(transport_queue(&p->trans,&msg)!=TRANSPORT_OK||
       sched_loop_update(&p->ctx->loop,p->fd,POLLIN|POLLOUT)<0){ p->failed=1; return -1; }
    return 0;
}

void server_close(struct peer *p)
{
    struct server *ctx=p->ctx;
    if(p->fd<0) return;
    sched_loop_remove(&ctx->loop,p->fd);
    transport_free(&p->trans); close(p->fd); p->fd=-1;
    // Socket gone => old worker session fenced. Re-register gets a fresh ID.
    if(p->worker_id) sched_worker_dead(&ctx->sched,p->worker_id,sched_now_ms());
    p->worker_id=0;
    sched_loop_request(&ctx->loop);
}

static int reply(struct peer *p,uint64_t req,int err,int id,const char *text)
{
    unsigned char buf[32768];
    size_t n=strlen(text);
    if(n>sizeof(buf)-10) return -1;
    pack32(buf,(uint32_t)err); pack32(buf+4,(uint32_t)id); pack16(buf+8,n);
    memcpy(buf+10,text,n);
    return server_queue(p,SCHED_REPLY,req,buf,n+10);
}

static int string_field(const unsigned char **ptr,size_t *left,char *out,size_t cap)
{
    size_t n;
    if(*left<2) return -1;
    n=wire16(*ptr); *ptr+=2; *left-=2;
    if(n>=cap||n>*left||memchr(*ptr,0,n)) return -1;
    memcpy(out,*ptr,n); out[n]=0; *ptr+=n; *left-=n;
    return 0;
}

static int submit(struct peer *p,const struct protocol_message *msg,uint64_t now,int *id)
{
    const unsigned char *buf=(const unsigned char *)msg->payload;
    size_t left=msg->payload_len,count=0,i;
    struct job_req req={0};
    struct sched_dep deps[QUEUE_CAP];
    if(!p->user[0]) return S_BAD_STATE;
    if(string_field(&buf,&left,req.user,sizeof(req.user))||
       string_field(&buf,&left,req.path,sizeof(req.path))||left<26) return S_BAD_INPUT;
    if(strcmp(req.user,p->user)) return S_BAD_INPUT;
    if(wire32(buf)>INT_MAX||wire64(buf+4)>LLONG_MAX||wire32(buf+12)>INT_MAX||
       wire32(buf+16)>INT_MAX||wire32(buf+21)>INT_MAX) return S_BAD_INPUT;
    req.need=(struct resource){(int)wire32(buf),(long long)wire64(buf+4),(int)wire32(buf+12)};
    req.time_limit=(int)wire32(buf+16); req.prio=buf[20];
    req.group=(int)wire32(buf+21); req.preemptible=buf[25];
    buf+=26; left-=26;
    // Optional deps trailer: count:u16, then group:u8 + id:u32 each.
    if(left){
        if(left<2) return S_BAD_INPUT;
        count=wire16(buf); buf+=2; left-=2;
        if(count>QUEUE_CAP||left!=count*5) return S_BAD_INPUT;
        for(i=0;i<count;i++,buf+=5){
            if(buf[0]>1||wire32(buf+1)>INT_MAX) return S_BAD_INPUT;
            deps[i]=(struct sched_dep){(int)wire32(buf+1),buf[0]};
        }
    }
    return sched_submit_deps(&p->ctx->svc,p->user,&req,deps,count,now,id);
}

static int nodes(struct peer *p,uint64_t req)
{
    char buf[30000];
    size_t used=0;
    int n;
    const struct worker *w;
    struct resource free_res;
    for(w=p->ctx->sched.workers;w;w=w->next){
        free_res=worker_free(w);
        n=snprintf(buf+used,sizeof(buf)-used,"%d %s cores=%d/%d ram=%lld/%lld gpu=%d/%d jobs=%zu reserved=%zu load=%.2f\n",
                   w->id,worker_state_name(w->state),free_res.cores,w->total.cores,
                   free_res.ram,w->total.ram,free_res.gpu,w->total.gpu,w->jobs,w->reserved_jobs,w->load);
        if(n<0||(size_t)n>=sizeof(buf)-used)
            return reply(p,req,S_BUFFER_SMALL,0,"nodes response too large; use service API pagination");
        used+=(size_t)n;
    }
    buf[used]=0;
    return reply(p,req,S_OK,0,buf);
}

int server_message(const struct protocol_message *msg,void *arg)
{
    struct peer *p=arg;
    struct scheduler *s=&p->ctx->sched;
    const unsigned char *buf=(const unsigned char *)msg->payload;
    size_t n=msg->payload_len,i;
    uint64_t now=sched_now_ms(),run;
    int err=S_BAD_INPUT,id=0;
    struct job *j;
    struct telemetry usage={0};
    char reason[160],text[1024];
    if(msg->type==WORKER_REGISTER){
        if(p->worker_id||p->user[0]||n<18||n!=18+wire16(buf+16)||
           wire32(buf)>INT_MAX||wire64(buf+4)>LLONG_MAX||wire32(buf+12)>INT_MAX) goto done;
        err=sched_register_worker(&p->ctx->svc,
             (struct resource){(int)wire32(buf),(long long)wire64(buf+4),(int)wire32(buf+12)},now,&id);
        if(err!=S_OK) goto done;
        p->worker_id=id;
        unsigned char out[4]; pack32(out,(uint32_t)id);
        return server_queue(p,WORKER_READY,msg->request_id,out,4);
    }
    if(p->worker_id){
        if(msg->type==WORKER_HEARTBEAT){
            // load in hundredths of percent, active count, then per-job telemetry.
            if(n<8||wire32(buf)>10000||wire32(buf+4)>(PROTOCOL_MAX_PAYLOAD-8)/40||
               n!=8+(size_t)wire32(buf+4)*40) goto done;
            // Validate entire batch first; stale individual jobs are ignored below.
            for(i=8;i<n;i+=40)
                if(wire64(buf+i+8)>LLONG_MAX||wire64(buf+i+16)>LLONG_MAX) goto done;
            err=sched_heartbeat(s,p->worker_id,wire32(buf)/100.0,now);
            if(err==S_OK) for(i=8;i<n;i+=40){
                j=job_find(s,(int)wire32(buf+i));
                if(!j||j->state!=RUNNING||j->worker_id!=p->worker_id||j->run_id!=wire32(buf+i+4)) continue;
                j->usage=(struct telemetry){(long long)wire64(buf+i+8),(long long)wire64(buf+i+16),
                                           wire64(buf+i+24),wire64(buf+i+32)/1000.0};
            }
            if(err==S_OK){ sched_loop_request(&p->ctx->loop); return 0; }
        }else if(msg->type==WORKER_DRAIN&&n==0){
            err=worker_set_state(s,p->worker_id,DRAINING);
        }else if(msg->type==WORKER_CACHE){
            if(n<2||n>=CONTENT_LEN+1||buf[0]>1||memchr(buf+1,0,n-1)) goto done;
            char key[CONTENT_LEN]; memcpy(key,buf+1,n-1); key[n-1]=0;
            err=sched_cache_set(s,p->worker_id,key,buf[0]);
        }else{
            if(n<8||wire32(buf)>INT_MAX) goto done;
            id=(int)wire32(buf); run=wire32(buf+4); j=job_find(s,id);
            if(!j){ err=S_NOT_FOUND; goto done; }
            if(j->worker_id!=p->worker_id||j->run_id!=run){ err=S_STALE_RUN; goto done; }
            if(msg->type==JOB_OUTPUT){
                if(n<16) goto done;
                err=sched_output(s,p->worker_id,id,run,wire64(buf+8),buf+16,n-16);
                if(err==S_OK) return 0;
            }else if(msg->type==EXEC_MISS){
                if(n<=8||n-8>=CONTENT_LEN||memchr(buf+8,0,n-8)) goto done;
                char key[CONTENT_LEN]; memcpy(key,buf+8,n-8); key[n-8]=0;
                err=sched_cache_set(s,p->worker_id,key,0);
                sched_note(s,"Job %d worker %d cache miss; artifact service must resend full bytes",id,p->worker_id);
            }else if(msg->type==JOB_ACCEPTED&&n==8){
                err=job_confirm_start(s,id,p->worker_id,run,now);
            }else if(msg->type==JOB_CHECKPOINT){
                if(n<20||wire32(buf+16)!=n-20) goto done;
                err=sched_checkpoint(s,p->worker_id,id,run,wire64(buf+8),buf+20,n-20,now);
            }else if(msg->type==JOB_COMPLETE||msg->type==JOB_FAILED||msg->type==JOB_STOPPED){
                if(n<34||n!=34+wire16(buf+32)||wire16(buf+32)>=sizeof(reason)||
                   wire64(buf+8)>LLONG_MAX) goto done;
                memcpy(reason,buf+34,n-34); reason[n-34]=0;
                usage.peak_ram=(long long)wire64(buf+8); usage.faults=wire64(buf+16);
                usage.cpu_time=wire64(buf+24)/1000.0;
                if(msg->type==JOB_STOPPED&&!j->stop_pending){ err=S_BAD_STATE; goto done; }
                err=sched_report(s,p->worker_id,id,run,msg->type==JOB_COMPLETE?COMPLETED:FAILED,&usage,reason,now);
            }
        }
    }else if(msg->type==CLIENT_LOGIN){
        if(p->user[0]||!n||n>=USER_LEN||memchr(buf,0,n)) goto done;
        memcpy(text,buf,n); text[n]=0;
        if(!user_valid(text)) goto done;
        strcpy(p->user,text); err=S_OK;
        sched_note(s,"Client %s logged in",p->user);
    }else if(p->user[0]){
        if(msg->type==JOB_SUBMIT) err=submit(p,msg,now,&id);
        else if(msg->type==NODES_QUERY&&n==0) return nodes(p,msg->request_id);
        else if((msg->type==STATUS_QUERY||msg->type==JOB_CANCEL||msg->type==JOB_MIGRATE)&&n==4){
            if(wire32(buf)>INT_MAX) goto done;
            id=(int)wire32(buf); j=job_find(s,id);
            if(!j){ err=S_NOT_FOUND; goto done; }
            struct sched_status status;
            err=sched_status_owned(&p->ctx->svc,p->user,id,&status);
            if(err!=S_OK) goto done;
            if(msg->type==JOB_CANCEL) err=sched_cancel_owned(&p->ctx->svc,p->user,id,now);
            else if(msg->type==JOB_MIGRATE) err=sched_migrate_owned(&p->ctx->svc,p->user,id,now);
            else{
                snprintf(text,sizeof(text),"%s worker=%d run=%llu pending=%d failures=%u ram=%lld peak=%lld faults=%llu cpu=%.3f checkpoint=%llu bytes=%zu age_ms=%llu reason=%s",
                    job_state_name(j->state),j->worker_id,(unsigned long long)j->run_id,j->dispatch_pending,
                    j->failures,j->usage.ram,j->usage.peak_ram,(unsigned long long)j->usage.faults,j->usage.cpu_time,
                    (unsigned long long)j->checkpoint_no,j->checkpoint_size,
                    (unsigned long long)(j->checkpoint_no?now-j->checkpoint_ms:0),j->reason);
                return reply(p,msg->request_id,S_OK,id,text);
            }
        }
    }
done:
    if(err==S_OK) sched_loop_request(&p->ctx->loop);
    return reply(p,msg->request_id,err,id,sched_error(err));
}

int server_assign(struct peer *p,struct job *j)
{
    unsigned char buf[PATH_LEN+34],*state;
    size_t n=strlen(j->req.path);
    if(j->run_id>UINT32_MAX) return -1;
    if(j->checkpoint_no){
        state=malloc(20+j->checkpoint_size);
        if(!state) return -1;
        pack32(state,(uint32_t)j->id); pack32(state+4,(uint32_t)j->run_id);
        pack64(state+8,j->checkpoint_no); pack32(state+16,(uint32_t)j->checkpoint_size);
        if(j->checkpoint_size) memcpy(state+20,j->checkpoint,j->checkpoint_size);
        int err=server_queue(p,JOB_RESTORE,0,state,20+j->checkpoint_size);
        free(state); if(err) return -1;
    }
    pack32(buf,(uint32_t)j->id); pack32(buf+4,(uint32_t)j->run_id);
    pack32(buf+8,(uint32_t)j->req.need.cores); pack64(buf+12,(uint64_t)j->req.need.ram);
    pack32(buf+20,(uint32_t)j->req.need.gpu); pack32(buf+24,(uint32_t)j->req.time_limit);
    pack16(buf+28,n); memcpy(buf+30,j->req.path,n); pack32(buf+30+n,0);
    return server_queue(p,JOB_ASSIGN,0,buf,n+34);
}

void server_stops(struct server *ctx)
{
    struct job *j;
    size_t i;
    unsigned char buf[9];
    for(j=ctx->sched.jobs;j;j=j->next){
        if(!j->stop_pending||j->stop_sent==j->stop_pending||(!j->dispatch_pending&&j->state!=RUNNING)) continue;
        for(i=0;i<MAX_PEERS;i++) if(ctx->peers[i].fd>=0&&ctx->peers[i].worker_id==j->worker_id){
            pack32(buf,(uint32_t)j->id); pack32(buf+4,(uint32_t)j->run_id); buf[8]=(unsigned char)j->stop_pending;
            // TCP queue reliable hai; repeated status/heartbeat pe duplicate stop nahi chahiye.
            if(server_queue(&ctx->peers[i],JOB_STOP,0,buf,9)==0) j->stop_sent=j->stop_pending;
            break;
        }
    }
}
