#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "logging.h"
#include "internal.h"
#include "node.h"

uint32_t worker_u32(const unsigned char *p){ return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
uint64_t worker_u64(const unsigned char *p)
{
    uint64_t x=0; int i;
    for(i=0;i<8;i++) x=x<<8|p[i];
    return x;
}
void worker_p32(unsigned char *p,uint32_t x){ int i; for(i=3;i>=0;i--){ p[i]=(unsigned char)x; x>>=8; } }
void worker_p64(unsigned char *p,uint64_t x){ int i; for(i=7;i>=0;i--){ p[i]=(unsigned char)x; x>>=8; } }
static size_t u16(const unsigned char *p){ return (size_t)p[0]*256+p[1]; }
static void p16(unsigned char *p,size_t x){ p[0]=(unsigned char)(x>>8); p[1]=(unsigned char)x; }

int worker_send(struct worker_node *n,int type,const void *buf,size_t size)
{
    struct protocol_message msg={(uint64_t)type,0,buf,size};
    int err;
    if(n->lost||n->fd<0) return 1;
    err=transport_queue(&n->trans,&msg);
    if(err==TRANSPORT_NO_MEMORY) return 1;
    if(err!=TRANSPORT_OK||worker_loop_watch(&n->loop,WORKER_CONN,n->fd,POLLIN|POLLOUT)<0){
        n->lost=1; worker_loop_stop(&n->loop); return 1;
    }
    return 0;
}

static void remove_task(struct worker_node *n,struct worker_task *t);
static struct worker_task *task(struct worker_node *n,int id,uint64_t run,int create)
{
    struct worker_task *t;
    size_t count=0;
    for(t=n->tasks;t;t=t->next){
        if(t->id==id){
            if(t->run==run) return t;
            // Scheduler ko final milte hi next run aa sakta hai, 100ms cleanup
            // tick se pehle bhi. Old finished execution ab retire kar sakte hain.
            if(create&&t->final_queued&&t->files_removed){
                if(runner_remove(&n->loop.runner,t->id,t->run)!=RUN_OK) return NULL;
                remove_task(n,t);
                return task(n,id,run,create);
            }
            return NULL;
        }
        count++;
    }
    if(!create||count>=256) return NULL;
    t=calloc(1,sizeof(*t)); if(!t) return NULL;
    t->id=id; t->run=run; t->created=runner_now_ms(); t->transfer.fd=-1;
    t->next=n->tasks; n->tasks=t;
    return t;
}
static void remove_task(struct worker_node *n,struct worker_task *t)
{
    struct worker_task **link;
    for(link=&n->tasks;*link&&*link!=t;link=&(*link)->next){}
    if(!*link) return;
    *link=t->next;
    worker_cache_abort(&n->cache,&t->transfer);
    if(t->pinned) worker_cache_unpin(&n->cache,t->key);
    free(t->restore); free(t->last_state); free(t);
}
void worker_tasks_free(struct worker_node *n)
{
    while(n->tasks) remove_task(n,n->tasks);
}

static int terminal(struct worker_node *n,int id,uint64_t run,int type,
                     const struct telemetry *usage,const char *reason)
{
    unsigned char buf[194];
    size_t size=strlen(reason);
    if(size>159) size=159;
    worker_p32(buf,(uint32_t)id); worker_p32(buf+4,(uint32_t)run);
    worker_p64(buf+8,(uint64_t)usage->peak_ram); worker_p64(buf+16,usage->faults);
    worker_p64(buf+24,(uint64_t)(usage->cpu_time*1000)); p16(buf+32,size);
    memcpy(buf+34,reason,size);
    return worker_send(n,type,buf,34+size);
}
void worker_cache_changed(const char *key,int present,void *arg)
{
    struct worker_node *n=arg;
    unsigned char buf[CONTENT_LEN+1];
    if(!n->worker_id) return;
    buf[0]=(unsigned char)present; memcpy(buf+1,key,strlen(key));
    // Inventory update drop hua toh session reconnect par complete inventory aayegi.
    if(worker_send(n,WORKER_CACHE,buf,strlen(key)+1)) n->lost=1;
}

static int capacity(struct worker_node *n,const struct job_req *req)
{
    struct run_job *j;
    long long cores=req->need.cores,ram=req->need.ram,gpu=req->need.gpu;
    if(req->need.cores>n->cfg.capacity.cores||ram>n->cfg.capacity.ram||gpu>n->cfg.capacity.gpu) return 0;
    for(j=n->loop.runner.head;j;j=j->next){
        if(j->reaped) continue;
        cores+=j->req.need.cores; gpu+=j->req.need.gpu;
        if(j->req.need.ram>n->cfg.capacity.ram-ram) return 0;
        ram+=j->req.need.ram;
    }
    return cores<=n->cfg.capacity.cores&&ram<=n->cfg.capacity.ram&&gpu<=n->cfg.capacity.gpu;
}

int worker_message(const struct protocol_message *msg,void *arg)
{
    struct worker_node *n=arg;
    const unsigned char *p=(const unsigned char *)msg->payload;
    size_t size=msg->payload_len,len;
    struct worker_task *t;
    struct run_job *j;
    struct job_req req={0};
    struct telemetry zero={0};
    int id,err;
    uint64_t run;
    if(msg->type==WORKER_READY){
        if(size!=4||n->worker_id||!worker_u32(p)||worker_u32(p)>INT_MAX) return -1;
        n->worker_id=(int)worker_u32(p); n->heartbeat=0;
        log_line("WORKER","Registered as worker %d",n->worker_id);
        struct worker_cache_entry *e;
        for(e=n->cache.head;e;e=e->next) e->reported=0;
        return n->lost?-1:0;
    }
    if(msg->type==SCHED_REPLY){
        if(size<10||u16(p+8)!=size-10) return -1;
        if(worker_u32(p)) log_line("WORKER","Scheduler rejected request: %.*s",(int)(size-10),(const char *)p+10);
        return 0;
    }
    if(!n->worker_id||size<8||!worker_u32(p)||worker_u32(p)>INT_MAX||!worker_u32(p+4)) return -1;
    id=(int)worker_u32(p); run=worker_u32(p+4);
    if(msg->type==JOB_STOP){
        if(size!=9||p[8]<1||p[8]>3) return -1;
        t=task(n,id,run,0);
        // Purane token ka stop naye execution ko kill nahi karega, aur
        // connection bhi unnecessarily nahi todega. Scheduler stale ack ignore karega.
        if(!t) return terminal(n,id,run,JOB_STOPPED,&zero,"no active execution for token")?-1:0;
    }else t=task(n,id,run,1);
    if(!t) return -1;
    if(msg->type==JOB_RESTORE){
        if(t->assigned||t->restore_seen||size<20||worker_u32(p+16)!=size-20||size-20>CHECKPOINT_CAP) return -1;
        t->restore=malloc(size-20+1); if(!t->restore) return -1;
        if(size>20) memcpy(t->restore,p+20,size-20);
        t->restore_size=size-20; t->restore_seen=1;
    }else if(msg->type==EXEC_BEGIN){
        if(t->assigned||t->pinned||t->transfer.fd>=0||size<18||u16(p+16)!=size-18||size-18>=CONTENT_LEN||memchr(p+18,0,size-18)) return -1;
        char key[CONTENT_LEN]; memcpy(key,p+18,size-18); key[size-18]=0;
        if(worker_cache_begin(&n->cache,&t->transfer,key,worker_u64(p+8),id,run)<0) return -1;
    }else if(msg->type==EXEC_CHUNK){
        if(size<16||size-16>65536||worker_cache_write(&t->transfer,worker_u64(p+8),p+16,size-16)<0) return -1;
    }else if(msg->type==EXEC_END){
        if(size!=8||t->pinned||worker_cache_end(&n->cache,&t->transfer,t->path,sizeof(t->path))<0) return -1;
        strcpy(t->key,t->transfer.key); t->pinned=1; t->cache_wait=0;
        if(n->cache.size>n->cache.limit)
            log_line("WORKER","Cache over budget temporarily; active/new executables remain pinned");
    }else if(msg->type==EXEC_REFERENCE){
        if(t->assigned||t->pinned||size<=8||size-8>=CONTENT_LEN||memchr(p+8,0,size-8)) return -1;
        char key[CONTENT_LEN]; memcpy(key,p+8,size-8); key[size-8]=0;
        err=worker_cache_pin(&n->cache,key,t->path,sizeof(t->path));
        if(err<0) return -1;
        if(err){
            t->cache_wait=1;
            unsigned char buf[CONTENT_LEN+8]; worker_p32(buf,(uint32_t)id); worker_p32(buf+4,(uint32_t)run);
            memcpy(buf+8,key,strlen(key));
            // Reference miss pe execution start nahi hui; same token pe full bytes mangenge.
            if(worker_send(n,EXEC_MISS,buf,strlen(key)+8)) return -1;
        }else{ strcpy(t->key,key); t->pinned=1; t->cache_wait=0; }
    }else if(msg->type==JOB_ASSIGN){
        if(t->assigned||t->cache_wait||t->transfer.fd>=0||size<34||worker_u32(p+8)>INT_MAX||
           worker_u64(p+12)>LLONG_MAX||worker_u32(p+20)>INT_MAX||worker_u32(p+24)>INT_MAX) return -1;
        len=u16(p+28);
        if(!len||len>=PATH_LEN||size<34+len||memchr(p+30,0,len)||worker_u32(p+30+len)!=size-34-len) return -1;
        if(size-34-len>CHECKPOINT_CAP||(t->restore_seen&&size-34-len)) return -1;
        strcpy(req.user,"worker"); memcpy(req.path,p+30,len); req.path[len]=0;
        if(t->pinned) strcpy(req.path,t->path);
        req.need=(struct resource){(int)worker_u32(p+8),(long long)worker_u64(p+12),(int)worker_u32(p+20)};
        req.time_limit=(int)worker_u32(p+24); req.prio=2;
        if(size>34+len){
            t->restore=malloc(size-34-len); if(!t->restore) return -1;
            memcpy(t->restore,p+34+len,size-34-len); t->restore_size=size-34-len;
        }
        char *args[]={req.path,NULL};
        err=n->draining||!capacity(n,&req)?RUN_BUSY:
            runner_start_state(&n->loop.runner,id,run,&req,args,t->restore,t->restore_size);
        if(err!=RUN_OK){
            if(terminal(n,id,run,JOB_FAILED,&zero,runner_error(err))) return -1;
            remove_task(n,t); return 0;
        }
        t->assigned=1;
        if(t->pinned) worker_cache_touch(&n->cache,t->key);
        log_line("WORKER","Job %d run %llu forked; waiting for exec outcome",id,(unsigned long long)run);
    }else if(msg->type==JOB_STOP){
        if(size!=9||p[8]<1||p[8]>3) return -1;
        j=runner_find(&n->loop.runner,id,run);
        if(!j){
            if(terminal(n,id,run,JOB_STOPPED,&zero,"stopped before assignment")) return -1;
            remove_task(n,t); return 0;
        }
        // Exit race mein bhi requested intent final report mein preserve karo.
        t->stop_intent=p[8];
        if(!j->reaped){
            enum stop_reason why=p[8]==1?STOP_CANCEL:p[8]==2?STOP_PREEMPT:STOP_MIGRATE;
            err=runner_stop(&n->loop.runner,id,run,why);
            if(err!=RUN_OK&&err!=RUN_BUSY) return -1;
        }
    }else return -1;
    return 0;
}

int worker_started(const struct worker_start *start,void *arg)
{
    struct worker_node *n=arg;
    unsigned char buf[8];
    log_line("WORKER","Job %d: %s",start->id,start->reason);
    if(start->exec_state!=RUN_START_OK) return 0;
    worker_p32(buf,(uint32_t)start->id); worker_p32(buf+4,(uint32_t)start->run_id);
    return worker_send(n,JOB_ACCEPTED,buf,8);
}

int worker_output(int id,uint64_t run,const void *buf,size_t size,void *arg)
{
    struct worker_node *n=arg;
    struct worker_task *t=task(n,id,run,0);
    unsigned char data[16400];
    if(!t||size>16384) return 1;
    worker_p32(data,(uint32_t)id); worker_p32(data+4,(uint32_t)run); worker_p64(data+8,t->out_offset);
    memcpy(data+16,buf,size);
    if(worker_send(n,JOB_OUTPUT,data,size+16)) return 1;
    t->out_offset+=size;
    return 0;
}

static int checkpoint(struct worker_node *n,struct worker_task *t,struct run_job *j,uint64_t now,int final)
{
    unsigned char *state,*buf;
    size_t size;
    int err;
    if(j->exec_state!=RUN_START_OK||!j->start_reported||(!final&&now-t->cp_ms<500)) return 0;
    t->cp_ms=now;
    err=runner_checkpoint(j,&state,&size);
    if(err<=0){
        if(err<0) log_line("WORKER","Job %d checkpoint ignored (invalid/unreadable state)",t->id);
        return 0;
    }
    if(t->have_state&&size==t->last_size&&!memcmp(state,t->last_state,size)){ free(state); return 0; }
    buf=malloc(size+20); if(!buf){ free(state); return 1; }
    worker_p32(buf,(uint32_t)t->id); worker_p32(buf+4,(uint32_t)t->run);
    worker_p64(buf+8,t->cp_seq+1); worker_p32(buf+16,(uint32_t)size);
    if(size) memcpy(buf+20,state,size);
    err=worker_send(n,JOB_CHECKPOINT,buf,size+20); free(buf);
    if(err){ free(state); return 1; }
    free(t->last_state); t->last_state=state; t->last_size=size; t->have_state=1; t->cp_seq++;
    return 0;
}

int worker_result(const struct worker_result *res,void *arg)
{
    struct worker_node *n=arg;
    struct worker_task *t=task(n,res->id,res->run_id,0);
    struct run_job *j=runner_find(&n->loop.runner,res->id,res->run_id);
    if(!t||!j) return 1;
    if(!t->final_queued){
        if(!t->files_removed){
            if(checkpoint(n,t,j,runner_now_ms(),1)) return 1;
            if(transport_wants_write(&n->trans)) return 1;
            // Same host pe migration destination ko wahi job ID mil sakta hai.
            // Reap/output flush ke baad folder hatao, phir stop/result ack bhejo.
            if(run_cleanup(j)<0){ n->lost=1; return 1; }
            t->files_removed=1;
        }
        if(terminal(n,res->id,res->run_id,t->stop_intent?JOB_STOPPED:
                    res->state==COMPLETED?JOB_COMPLETE:JOB_FAILED,&res->usage,res->reason)) return 1;
        t->final_queued=1;
        log_line("WORKER","Job %d: %s",res->id,res->reason);
        return 1;
    }
    // Output/checkpoint/result socket se flush hone ke baad hi files aur pin chhodna.
    if(transport_wants_write(&n->trans)) return 1;
    remove_task(n,t);
    return 0;
}

void worker_tick(struct worker_loop *loop,uint64_t now,void *arg)
{
    struct worker_node *n=arg;
    struct run_job *j;
    struct worker_task *t;
    unsigned char buf[8+256*40];
    size_t count=0,off=8;
    double load=0;
    if(n->lost){ worker_loop_stop(loop); return; }
    if(!n->worker_id){ if(now-n->registered_ms>10000){ n->lost=1; worker_loop_stop(loop); } return; }
    struct worker_cache_entry *entry;
    size_t sent=0;
    for(entry=n->cache.head;entry&&sent<16;entry=entry->next){
        if(entry->reported) continue;
        unsigned char inventory[CONTENT_LEN+1];
        inventory[0]=1; memcpy(inventory+1,entry->key,strlen(entry->key));
        if(worker_send(n,WORKER_CACHE,inventory,strlen(entry->key)+1)) break;
        entry->reported=1; sent++;
    }
    if(n->draining&&!n->drain_sent){ if(!worker_send(n,WORKER_DRAIN,NULL,0)) n->drain_sent=1; }
    if(n->draining){
        struct worker_task *next;
        struct telemetry zero={0};
        for(t=n->tasks;t;t=next){
            next=t->next;
            if(!t->assigned&&!terminal(n,t->id,t->run,JOB_FAILED,&zero,"worker draining before start"))
                remove_task(n,t);
        }
    }
    for(j=loop->runner.head;j;j=j->next){
        t=task(n,j->id,j->run_id,0);
        if(t&&!t->final_queued&&!t->files_removed) checkpoint(n,t,j,now,0);
    }
    if(now-n->heartbeat>=2000){
        if(getloadavg(&load,1)!=1) load=0;
        load=load*100.0/n->cfg.capacity.cores; if(load>100) load=100; if(load<0) load=0;
        for(j=loop->runner.head;j&&count<256;j=j->next){
            if(j->reaped||j->exec_state!=RUN_START_OK||!j->start_reported) continue;
            worker_p32(buf+off,(uint32_t)j->id); worker_p32(buf+off+4,(uint32_t)j->run_id);
            worker_p64(buf+off+8,(uint64_t)j->usage.ram); worker_p64(buf+off+16,(uint64_t)j->usage.peak_ram);
            worker_p64(buf+off+24,j->usage.faults); worker_p64(buf+off+32,(uint64_t)(j->usage.cpu_time*1000));
            count++; off+=40;
        }
        worker_p32(buf,(uint32_t)(load*100)); worker_p32(buf+4,(uint32_t)count);
        if(!worker_send(n,WORKER_HEARTBEAT,buf,off)) n->heartbeat=now;
    }
    if(n->draining&&n->drain_sent&&!n->tasks&&!loop->runner.head&&!transport_wants_write(&n->trans)) worker_loop_stop(loop);
    for(t=n->tasks;t;t=t->next)
        if(!t->assigned&&now-t->created>30000){ n->lost=1; worker_loop_stop(loop); break; }
}
