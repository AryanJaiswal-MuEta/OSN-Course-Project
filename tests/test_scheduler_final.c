#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scheduler_service.h"

static int checks,wakeups;
#define CHECK(x) do{ checks++; if(!(x)){ fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } }while(0)
static struct job_req req(const char *user,int cores,int prio)
{
    struct job_req r={0};
    strcpy(r.user,user); strcpy(r.path,"true");
    r.need=(struct resource){cores,64,1}; r.time_limit=30; r.prio=prio; r.preemptible=1;
    return r;
}
static int add(struct scheduler *s,const char *user,int cores,int prio)
{
    struct job_req r=req(user,cores,prio); int id;
    CHECK(job_add(s,&r,100,&id)==S_OK); return id;
}
static int worker(struct scheduler *s,int cores)
{
    int id; CHECK(worker_add(s,(struct resource){cores,8192,8},100,&id)==S_OK); return id;
}
static uint64_t start(struct scheduler *s,int id,int w)
{
    uint64_t run; CHECK(job_start(s,id,w,100,&run)==S_OK); return run;
}
static void wake(void *arg){ (void)arg; wakeups++; }

static void preemption_candidates(void)
{
    struct scheduler s;
    int w,victim,impossible,urgent;
    uint64_t run;
    sched_init(&s); w=worker(&s,2);
    victim=add(&s,"a",2,3); run=start(&s,victim,w);
    impossible=add(&s,"a",4,1); urgent=add(&s,"a",2,1);
    CHECK(sched_preempt_next(&s,SCHED_FIFO_ORDER,100)==S_NOT_FOUND);
    CHECK(!job_find(&s,victim)->stop_pending);
    CHECK(sched_preempt_next(&s,SCHED_PRIORITY_ORDER,100)==S_OK);
    CHECK(job_find(&s,victim)->hold_for==urgent&&worker_find(&s,w)->preempt_for==urgent);
    CHECK(!job_find(&s,impossible)->preempt_worker);
    CHECK(sched_report(&s,w,victim,run,FAILED,NULL,"stopped",110)==S_OK);
    CHECK(job_find(&s,victim)->failures==0);
    sched_free(&s);
}

static void cancellation_and_transport(void)
{
    struct scheduler s;
    struct sched_pick pick;
    uint64_t run;
    int w,a,urgent,target;
    sched_init(&s); w=worker(&s,4); a=add(&s,"a",4,3);
    CHECK(sched_worker_writable(&s,w,0)==S_OK);
    CHECK(sched_pick_next(&s,SCHED_FIFO_ORDER,100,&pick)==S_NO_RESOURCE);
    CHECK(job_reserve(&s,a,w,100,&run)==S_BUSY);
    CHECK(s.pending==1&&worker_find(&s,w)->used.cores==0);
    CHECK(sched_worker_writable(&s,w,2)==S_BAD_INPUT);
    CHECK(sched_worker_writable(&s,w,1)==S_OK);
    run=start(&s,a,w);
    urgent=add(&s,"a",1,1);
    CHECK(sched_worker_writable(&s,w,0)==S_OK);
    CHECK(sched_preempt(&s,urgent,100,&target)==S_NO_RESOURCE&&!job_find(&s,a)->stop_pending);
    CHECK(sched_worker_writable(&s,w,1)==S_OK);
    CHECK(sched_cancel(&s,a,110)==S_OK);
    job_find(&s,a)->stop_sent=S_STOP_CANCEL;
    CHECK(sched_cancel(&s,a,9000)==S_OK);
    CHECK(job_find(&s,a)->stop_ms==110&&job_find(&s,a)->stop_sent==S_STOP_CANCEL);
    CHECK(worker_find(&s,w)->used.cores==4);
    CHECK(sched_report(&s,w,a,run,COMPLETED,NULL,"race",9001)==S_OK);
    CHECK(job_find(&s,a)->state==CANCELLED&&worker_find(&s,w)->used.cores==0);
    sched_free(&s);
}

static void quota_releases_holds(void)
{
    struct scheduler s;
    int w,w2,a,victim,urgent,other,target;
    uint64_t run;
    sched_init(&s); w=worker(&s,4); w2=worker(&s,8);
    a=add(&s,"a",5,2); start(&s,a,w2);
    victim=add(&s,"b",4,3); run=start(&s,victim,w);
    urgent=add(&s,"a",4,1);
    CHECK(sched_preempt(&s,urgent,100,&target)==S_OK&&target==w);
    a=add(&s,"a",2,2); start(&s,a,w2);
    other=add(&s,"b",4,3); (void)other;
    CHECK(sched_quota_skip(&s,job_find(&s,urgent)));
    sched_refresh_holds(&s);
    CHECK(!worker_find(&s,w)->preempt_for&&!job_find(&s,victim)->hold_for);
    CHECK(!job_find(&s,urgent)->preempt_worker);
    CHECK(sched_report(&s,w,victim,run,FAILED,NULL,"stopped",110)==S_OK);
    CHECK(sched_fit(&s,job_find(&s,victim))->id==w);
    CHECK(job_find(&s,victim)->failures==0);
    sched_free(&s);

    sched_init(&s); w=worker(&s,4);
    victim=add(&s,"a",4,3); start(&s,victim,w);
    urgent=add(&s,"a",4,1);
    CHECK(sched_preempt(&s,urgent,100,&target)==S_OK);
    CHECK(worker_set_state(&s,w,DRAINING)==S_OK);
    sched_refresh_holds(&s);
    CHECK(!worker_find(&s,w)->preempt_for&&!job_find(&s,victim)->hold_for);
    CHECK(worker_find(&s,w)->used.cores==4&&job_find(&s,victim)->stop_pending==S_STOP_PREEMPT);
    sched_free(&s);
}

static void service_boundaries(void)
{
    struct scheduler s;
    struct sched_service svc;
    struct sched_status status;
    struct sched_output_info output;
    struct sched_checkpoint_info cp;
    struct job_req r=req("forged_owner",1,2);
    struct sched_node nodes[1];
    unsigned char data[]={0,'a',0xff,'z'},buf[8];
    size_t count;
    uint64_t run;
    int w,id,before;
    sched_init(&s); w=worker(&s,2);
    CHECK(sched_service_init(&svc,&s,wake,NULL,NULL)==S_OK);
    CHECK(sched_submit(&svc,"alice",&r,100,&id)==S_OK);
    CHECK(!strcmp(job_find(&s,id)->req.user,"alice"));
    run=start(&s,id,w);
    CHECK(sched_output(&s,w,id,run,0,data,sizeof(data))==S_OK);
    CHECK(sched_checkpoint(&s,w,id,run,1,data,sizeof(data),110)==S_OK);
    memset(buf,0x55,sizeof(buf));
    CHECK(sched_output_get(&svc,"bob",id,buf,sizeof(buf),&output)==S_FORBIDDEN);
    CHECK(output.size==0&&buf[0]==0x55);
    CHECK(sched_checkpoint_get(&svc,"bob",id,buf,sizeof(buf),&cp)==S_FORBIDDEN&&cp.number==0);
    CHECK(sched_output_get(&svc,"alice",id,buf,3,&output)==S_BUFFER_SMALL);
    CHECK(output.size==sizeof(data)&&buf[0]==0x55);
    CHECK(sched_output_get(&svc,"alice",id,buf,sizeof(buf),&output)==S_OK&&!memcmp(buf,data,sizeof(data)));
    CHECK(output.current_run_received==sizeof(data)&&output.run_id==run);
    CHECK(sched_checkpoint_get(&svc,"alice",id,NULL,0,&cp)==S_BUFFER_SMALL);
    CHECK(cp.size==sizeof(data)&&cp.number==1&&cp.received_ms==110);
    CHECK(sched_checkpoint_get(&svc,"alice",id,buf,sizeof(buf),&cp)==S_OK&&!memcmp(buf,data,sizeof(data)));
    CHECK(sched_status_owned(&svc,"bob",id,&status)==S_FORBIDDEN&&status.id==0);
    CHECK(sched_status_owned(&svc,"alice",id,&status)==S_OK&&status.output_size==4&&status.start_ms==100);
    before=wakeups;
    CHECK(sched_cancel_owned(&svc,"bob",id,120)==S_FORBIDDEN&&!job_find(&s,id)->stop_pending);
    CHECK(sched_migrate_owned(&svc,"bob",id,120)==S_FORBIDDEN&&wakeups==before);
    CHECK(sched_migrate_owned(&svc,"alice",id,120)==S_OK&&wakeups==before+1);
    CHECK(sched_cancel_owned(&svc,"alice",id,121)==S_OK&&job_find(&s,id)->stop_pending==S_STOP_CANCEL);
    CHECK(sched_worker_writable(&s,w,0)==S_OK);
    CHECK(sched_nodes_get(&svc,nodes,1,&count)==S_OK&&nodes[0].dispatch_blocked==1);
    CHECK(sched_report(&s,w,id,run,FAILED,NULL,"stopped",122)==S_OK);
    CHECK(sched_checkpoint_get(&svc,"alice",id,buf,sizeof(buf),&cp)==S_OK&&cp.number==1);
    CHECK(sched_output_get(&svc,"alice",id,buf,sizeof(buf),&output)==S_OK&&output.state==CANCELLED);
    sched_free(&s);
}

int main(void)
{
    preemption_candidates(); cancellation_and_transport(); quota_releases_holds(); service_boundaries();
    printf("PASS scheduler final policy/service regressions: %d checks\n",checks);
    return 0;
}
