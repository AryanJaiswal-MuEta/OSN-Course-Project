#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scheduler.h"

#define CHECK(x) do { checks++; if(!(x)){ fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } }while(0)
static int checks;

static struct job_req req(const char *user,int cores,int prio)
{
    struct job_req r={0};
    strcpy(r.user,user); strcpy(r.path,"true");
    r.need=(struct resource){cores,64,1}; r.time_limit=10; r.prio=prio; r.preemptible=1;
    return r;
}
static int add(struct scheduler *s,const char *user,int cores,int prio,uint64_t now)
{
    struct job_req r=req(user,cores,prio);
    int id;
    CHECK(job_add(s,&r,now,&id)==S_OK);
    return id;
}
static int worker(struct scheduler *s,int cores,uint64_t now)
{
    int id;
    CHECK(worker_add(s,(struct resource){cores,8192,cores},now,&id)==S_OK);
    return id;
}
static uint64_t start(struct scheduler *s,int id,int wid,uint64_t now)
{
    uint64_t run;
    CHECK(job_start(s,id,wid,now,&run)==S_OK);
    return run;
}

static void conserved(struct scheduler *s)
{
    struct worker *w;
    struct job *j;
    struct resource sum;
    size_t pending=0,jobs,reserved;
    for(j=s->jobs;j;j=j->next) if(j->state==QUEUED||j->state==WAITING) pending++;
    CHECK(s->pending==pending);
    for(w=s->workers;w;w=w->next){
        sum=(struct resource){0,0,0}; jobs=reserved=0;
        for(j=s->jobs;j;j=j->next) if(j->worker_id==w->id){
            CHECK(j->state==RUNNING||j->dispatch_pending);
            sum.cores+=j->req.need.cores; sum.ram+=j->req.need.ram; sum.gpu+=j->req.need.gpu;
            if(j->dispatch_pending) reserved++; else jobs++;
        }
        CHECK(w->used.cores==sum.cores&&w->used.ram==sum.ram&&w->used.gpu==sum.gpu);
        CHECK(w->jobs==jobs&&w->reserved_jobs==reserved);
        CHECK(sum.cores>=0&&sum.cores<=w->total.cores&&sum.ram<=w->total.ram&&sum.gpu<=w->total.gpu);
    }
}

static void dependencies(void)
{
    struct scheduler s;
    struct job_req r=req("alice",1,2);
    struct sched_dep d[3];
    int a,b,c,e,w;
    uint64_t run;
    sched_init(&s); w=worker(&s,4,100);
    r.group=7; CHECK(job_add(&s,&r,100,&a)==S_OK);
    d[0]=(struct sched_dep){7,1}; d[1]=(struct sched_dep){a,0};
    CHECK(job_add_deps(&s,&r,d,2,200,&b)==S_OK);
    CHECK(job_find(&s,b)->dep_count==1&&job_find(&s,b)->state==WAITING);
    CHECK(job_add(&s,&r,200,&c)==S_OK);
    CHECK(job_find(&s,b)->dep_count==1); // Later group member snapshot mein nahi aayega.
    d[0]=(struct sched_dep){9999,0};
    CHECK(job_add_deps(&s,&r,d,1,200,&e)==S_NOT_FOUND&&s.job_count==3);
    run=start(&s,a,w,300);
    CHECK(sched_report(&s,w,a,run,COMPLETED,NULL,"ok",400)==S_OK);
    CHECK(job_find(&s,b)->state==QUEUED&&job_find(&s,b)->queue_ms==400);
    d[0]=(struct sched_dep){b,0}; CHECK(job_add_deps(&s,&r,d,1,400,&e)==S_OK);
    CHECK(sched_cancel(&s,b,500)==S_OK);
    CHECK(job_find(&s,e)->state==CANCELLED);
    d[0]=(struct sched_dep){e,0}; CHECK(job_add_deps(&s,&r,d,1,600,&b)==S_OK);
    CHECK(job_find(&s,b)->state==CANCELLED);
    conserved(&s); sched_free(&s);
}

static void quota_and_placement(void)
{
    struct scheduler s;
    struct sched_pick pick;
    struct job *j;
    int w,w2,a,b,c;
    uint64_t run;
    sched_init(&s); w=worker(&s,4,100);
    a=add(&s,"alice",3,2,100); run=start(&s,a,w,100);
    b=add(&s,"alice",2,1,100); c=add(&s,"bob",1,3,100);
    CHECK(sched_quota_skip(&s,job_find(&s,b)));
    CHECK(sched_pick_next(&s,SCHED_FIFO_ORDER,100,&pick)==S_OK&&pick.job_id==c);
    CHECK(sched_pick_next(&s,SCHED_PRIORITY_ORDER,100,&pick)==S_OK&&pick.job_id==c);
    CHECK(sched_cancel(&s,c,100)==S_OK);
    CHECK(!sched_quota_skip(&s,job_find(&s,b)));
    CHECK(sched_report(&s,w,a,run,COMPLETED,NULL,"",200)==S_OK);
    a=add(&s,"alice",2,2,200); run=start(&s,a,w,200);
    c=add(&s,"bob",1,3,200);
    CHECK(!sched_quota_skip(&s,job_find(&s,b))); // Exactly half allowed.
    conserved(&s); sched_free(&s);

    sched_init(&s); w=worker(&s,4,100); w2=worker(&s,4,100);
    a=add(&s,"alice",1,3,100); run=start(&s,a,w,100);
    b=add(&s,"alice",1,3,100); j=job_find(&s,b);
    CHECK(sched_content_set(&s,b,"opaque-content-A")==S_OK);
    CHECK(sched_cache_set(&s,w,"opaque-content-A",1)==S_OK);
    CHECK(sched_fit(&s,j)->id==w2); // Idle beats busy cached.
    CHECK(sched_report(&s,w,a,run,COMPLETED,NULL,"",200)==S_OK);
    CHECK(sched_fit(&s,j)->id==w); // Equal idle => cache wins.
    CHECK(sched_cache_set(&s,w,"opaque-content-A",0)==S_OK);
    CHECK(sched_heartbeat(&s,w,80,200)==S_OK);
    CHECK(sched_heartbeat(&s,w2,20,200)==S_OK);
    CHECK(sched_fit(&s,j)->id==w2);
    CHECK(worker_set_state(&s,w2,SUSPECTED)==S_OK);
    CHECK(sched_fit(&s,j)->id==w);
    CHECK(worker_set_state(&s,w,DRAINING)==S_OK);
    CHECK(!sched_fit(&s,j));
    CHECK(sched_heartbeat(&s,w2,10,201)==S_OK&&worker_find(&s,w2)->state==ALIVE);
    CHECK(sched_heartbeat(&s,w,10,201)==S_OK&&worker_find(&s,w)->state==DRAINING);
    conserved(&s); sched_free(&s);
}

static void loss_and_capacity(void)
{
    struct scheduler s;
    struct sched_dep dep;
    struct job_req r=req("alice",1,3);
    uint64_t run,old;
    int w,a,b,id,i;
    sched_init(&s); w=worker(&s,4,100);
    a=add(&s,"alice",1,2,100); run=start(&s,a,w,100); old=run;
    dep=(struct sched_dep){a,0}; CHECK(job_add_deps(&s,&r,&dep,1,100,&b)==S_OK);
    CHECK(sched_tick(&s,2099)==0);
    CHECK(sched_tick(&s,2100)==1&&worker_find(&s,w)->state==SUSPECTED);
    CHECK(sched_heartbeat(&s,w,25,2200)==S_OK&&worker_find(&s,w)->state==ALIVE);
    CHECK(sched_tick(&s,8199)==1&&worker_find(&s,w)->state==SUSPECTED);
    CHECK(sched_tick(&s,8200)==1&&worker_find(&s,w)->state==DEAD);
    CHECK(job_find(&s,a)->state==QUEUED&&job_find(&s,a)->failures==1);
    CHECK(job_find(&s,b)->state==WAITING);
    CHECK(sched_report(&s,w,a,old,COMPLETED,NULL,"late",8201)==S_STALE_RUN);
    conserved(&s);
    w=worker(&s,4,8300); run=start(&s,a,w,8300);
    CHECK(sched_worker_dead(&s,w,8400)==S_OK);
    w=worker(&s,4,8500); run=start(&s,a,w,8500);
    CHECK(sched_worker_dead(&s,w,8600)==S_OK);
    CHECK(job_find(&s,a)->state==FAILED_PERMANENT&&job_find(&s,a)->failures==3);
    CHECK(job_find(&s,b)->state==CANCELLED);
    conserved(&s); sched_free(&s);

    sched_init(&s); w=worker(&s,4,100);
    a=add(&s,"alice",1,2,100); run=start(&s,a,w,100);
    for(i=0;i<QUEUE_CAP;i++) CHECK(job_add(&s,&r,100,&id)==S_OK);
    CHECK(sched_worker_dead(&s,w,200)==S_OK&&s.pending==QUEUE_CAP+1);
    CHECK(job_add(&s,&r,200,&id)==S_QUEUE_FULL);
    conserved(&s); sched_free(&s);
    (void)run;
}

static void preemption_and_migration(void)
{
    struct scheduler s;
    struct job *j;
    struct sched_pick pick;
    uint64_t r1,r2,r3,run;
    int w,w2,a,b,c,urgent,target;
    sched_init(&s); w=worker(&s,4,100);
    a=add(&s,"alice",1,3,100); r1=start(&s,a,w,100);
    b=add(&s,"alice",1,3,200); r2=start(&s,b,w,200);
    c=add(&s,"alice",2,2,300); r3=start(&s,c,w,300);
    urgent=add(&s,"alice",2,1,400);
    CHECK(sched_preempt(&s,urgent,400,&target)==S_OK&&target==w);
    CHECK(job_find(&s,b)->stop_pending==S_STOP_PREEMPT&&job_find(&s,a)->stop_pending==S_STOP_PREEMPT);
    CHECK(!job_find(&s,c)->stop_pending&&worker_find(&s,w)->used.cores==4);
    CHECK(sched_report(&s,w,b,r2,FAILED,NULL,"killed",500)==S_OK);
    CHECK(job_find(&s,b)->hold_for==urgent&&!job_find(&s,b)->failures);
    CHECK(sched_pick_next(&s,SCHED_PRIORITY_ORDER,500,&pick)==S_NO_RESOURCE);
    CHECK(sched_report(&s,w,a,r1,FAILED,NULL,"killed",500)==S_OK);
    CHECK(sched_pick_next(&s,SCHED_PRIORITY_ORDER,500,&pick)==S_OK&&pick.job_id==urgent);
    run=start(&s,urgent,w,500);
    CHECK(!job_find(&s,a)->hold_for&&!worker_find(&s,w)->preempt_for);
    CHECK(sched_cancel(&s,urgent,600)==S_OK);
    CHECK(worker_find(&s,w)->used.cores==4);
    CHECK(sched_report(&s,w,urgent,run,COMPLETED,NULL,"race with cancel",601)==S_OK);
    CHECK(job_find(&s,urgent)->state==CANCELLED);
    conserved(&s);
    w2=worker(&s,4,700);
    CHECK(sched_checkpoint(&s,w,c,r3,1,"state1",6,700)==S_OK);
    CHECK(sched_checkpoint(&s,w,c,r3,1,"bad",3,700)==S_BAD_INPUT);
    CHECK(sched_checkpoint(&s,w2,c,r3,2,"bad",3,700)==S_STALE_RUN);
    CHECK(sched_migrate(&s,c,700)==S_OK);
    CHECK(sched_checkpoint(&s,w,c,r3,2,"state2",6,701)==S_OK);
    CHECK(sched_report(&s,w,c,r3,COMPLETED,NULL,"grace exit",702)==S_OK);
    j=job_find(&s,c);
    CHECK(j->state==QUEUED&&!j->failures&&j->checkpoint_no==2&&!memcmp(j->checkpoint,"state2",6));
    CHECK(sched_fit(&s,j)->id==w2);
    run=start(&s,c,w2,703);
    CHECK(sched_checkpoint(&s,w2,c,run,1,"state3",6,704)==S_OK&&j->checkpoint_no==3);
    CHECK(sched_report(&s,w,c,r3,FAILED,NULL,"stale",705)==S_STALE_RUN);
    CHECK(sched_report(&s,w2,c,run,COMPLETED,NULL,"done",705)==S_OK);
    CHECK(sched_migrate(&s,c,706)==S_BAD_STATE);
    conserved(&s); sched_free(&s);
}

static void reservation_races(void)
{
    struct scheduler s;
    int w,a,b,target;
    uint64_t run;
    sched_init(&s); w=worker(&s,2,100);
    a=add(&s,"alice",1,3,100);
    CHECK(job_reserve(&s,a,w,100,&run)==S_OK);
    CHECK(sched_cancel(&s,a,110)==S_OK);
    CHECK(job_confirm_start(&s,a,w,run,120)==S_OK);
    CHECK(sched_report(&s,w,a,run,FAILED,NULL,"cancelled",130)==S_OK);
    CHECK(job_find(&s,a)->state==CANCELLED);
    CHECK(sched_report(&s,w,a,run,FAILED,NULL,"duplicate",140)==S_STALE_RUN);
    a=add(&s,"alice",1,3,200);
    CHECK(job_reserve(&s,a,w,200,&run)==S_OK);
    CHECK(sched_cancel(&s,a,210)==S_OK);
    CHECK(sched_report(&s,w,a,run,FAILED,NULL,"exec fail",220)==S_OK);
    CHECK(job_find(&s,a)->state==CANCELLED);
    a=add(&s,"alice",2,3,300); run=start(&s,a,w,300);
    b=add(&s,"alice",2,1,400);
    job_find(&s,a)->req.preemptible=0;
    CHECK(sched_preempt(&s,b,400,&target)==S_NO_RESOURCE&&!job_find(&s,a)->stop_pending);
    job_find(&s,a)->req.preemptible=1;
    CHECK(sched_preempt(&s,b,400,&target)==S_OK);
    CHECK(sched_cancel(&s,b,401)==S_OK&&!job_find(&s,a)->hold_for&&!worker_find(&s,w)->preempt_for);
    CHECK(sched_report(&s,w,a,run,FAILED,NULL,"stopped",402)==S_OK);
    CHECK(job_find(&s,a)->state==QUEUED&&!job_find(&s,a)->failures);
    conserved(&s); sched_free(&s);
}

static void recovery_edges(void)
{
    struct scheduler s;
    uint64_t run,old;
    int w,w2,a,b;
    sched_init(&s); w=worker(&s,4,100);
    a=add(&s,"alice",1,3,100);
    CHECK(job_reserve(&s,a,w,200,&run)==S_OK);
    CHECK(sched_worker_dead(&s,w,199)==S_BAD_INPUT&&worker_find(&s,w)->state==ALIVE);
    CHECK(sched_worker_dead(&s,w,300)==S_OK);
    CHECK(s.pending==1&&job_find(&s,a)->wait_ms==200);
    old=run; w=worker(&s,4,400);
    CHECK(job_reserve(&s,a,w,400,&run)==S_OK&&old!=run);
    CHECK(job_confirm_start(&s,a,w,old,400)==S_STALE_RUN);
    CHECK(sched_report(&s,w,a,run,FAILED,NULL,"exec error",401)==S_OK);
    CHECK(job_find(&s,a)->state==FAILED&&s.pending==0);
    CHECK(sched_report(&s,w,a,run,FAILED,NULL,"duplicate",402)==S_STALE_RUN);
    a=add(&s,"alice",1,3,500); run=start(&s,a,w,500);
    CHECK(sched_cancel(&s,a,501)==S_OK);
    CHECK(sched_worker_dead(&s,w,502)==S_OK);
    CHECK(job_find(&s,a)->state==CANCELLED&&!job_find(&s,a)->failures);
    conserved(&s); sched_free(&s);

    sched_init(&s); w=worker(&s,4,100); w2=worker(&s,2,100);
    a=add(&s,"alice",3,2,100); run=start(&s,a,w,100);
    a=add(&s,"alice",1,1,100); b=add(&s,"bob",1,3,100);
    CHECK(!sched_quota_skip(&s,job_find(&s,a))); // 3/6 is exactly half.
    CHECK(worker_set_state(&s,w2,SUSPECTED)==S_OK&&!sched_quota_skip(&s,job_find(&s,a)));
    CHECK(worker_set_state(&s,w2,DRAINING)==S_OK&&!sched_quota_skip(&s,job_find(&s,a)));
    CHECK(sched_worker_dead(&s,w2,100)==S_OK&&sched_quota_skip(&s,job_find(&s,a)));
    CHECK(sched_cancel(&s,b,101)==S_OK&&!sched_quota_skip(&s,job_find(&s,a)));
    conserved(&s); sched_free(&s);
    (void)run;
}

static void output_tail(void)
{
    struct scheduler s;
    char buf[16384];
    int w,a,i;
    uint64_t run;
    sched_init(&s); w=worker(&s,2,100);
    a=add(&s,"alice",1,2,100); run=start(&s,a,w,100);
    for(i=0;i<4;i++){
        memset(buf,'A'+i,sizeof(buf));
        CHECK(sched_output(&s,w,a,run,(uint64_t)i*sizeof(buf),buf,sizeof(buf))==S_OK);
    }
    CHECK(job_find(&s,a)->output_size==65536);
    CHECK(sched_output(&s,w,a,run,65536,"E",1)==S_OK);
    CHECK(job_find(&s,a)->output[0]=='A'&&job_find(&s,a)->output[65535]=='E');
    CHECK(sched_output(&s,w,a,run,0,"duplicate",9)==S_BAD_INPUT);
    CHECK(sched_output(&s,w+1,a,run,65537,"x",1)==S_STALE_RUN);
    CHECK(sched_migrate(&s,a,200)==S_OK);
    CHECK(sched_report(&s,w,a,run,FAILED,NULL,"stopped",201)==S_OK);
    run=start(&s,a,w,202);
    CHECK(job_find(&s,a)->output_received==0);
    CHECK(sched_output(&s,w,a,run,0,"next run",8)==S_OK);
    CHECK(job_find(&s,a)->output_size==65536&&job_find(&s,a)->output[65535]=='n');
    conserved(&s); sched_free(&s);
}

int main(void)
{
    dependencies(); quota_and_placement(); loss_and_capacity();
    preemption_and_migration(); reservation_races(); recovery_edges(); output_tail();
    printf("PASS advanced scheduler: %d checks\n",checks);
    return 0;
}
