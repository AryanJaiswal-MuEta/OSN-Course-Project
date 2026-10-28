#include <stdio.h>
#include <string.h>
#include "scheduler_service.h"

static int checks;
#define CHECK(expr) do { checks++; if(!(expr)){ \
    printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); ok=0; goto done; \
} } while(0)

struct hooks {
    int changes;
    int logs;
    char msg[160];
};

static void changed(void *arg)
{
    struct hooks *h=arg;
    h->changes++;
}

static void log_msg(const char *msg,void *arg)
{
    struct hooks *h=arg;
    h->logs++;
    snprintf(h->msg,sizeof(h->msg),"%s",msg);
}

static struct job_req request(void)
{
    struct job_req req;
    memset(&req,0,sizeof(req));
    strcpy(req.user,"not_the_session_user");
    strcpy(req.path,"./hello");
    req.need=(struct resource){2,512,1};
    req.time_limit=5;
    req.prio=2;
    return req;
}

static int snapshots(void)
{
    struct scheduler s;
    struct sched_service svc;
    struct job_req req=request();
    struct hooks h={0,0,{0}};
    struct sched_status out;
    struct sched_node nodes[2];
    size_t count;
    uint64_t run;
    int id,wid,other,ok=1;
    sched_init(&s);
    CHECK(sched_service_init(&svc,&s,changed,log_msg,&h)==S_OK);
    CHECK(sched_nodes_get(&svc,NULL,0,&count)==S_OK&&count==0);
    CHECK(worker_add(&s,(struct resource){4,2048,1},0,&wid)==S_OK);
    CHECK(worker_add(&s,(struct resource){2,1024,0},0,&other)==S_OK);
    nodes[0].id=-42;
    CHECK(sched_nodes_get(&svc,nodes,1,&count)==S_BUFFER_SMALL&&count==2);
    CHECK(nodes[0].id==-42);
    CHECK(sched_nodes_get(&svc,NULL,0,&count)==S_BUFFER_SMALL&&count==2);
    CHECK(sched_submit(&svc,"aryan",&req,10,&id)==S_OK);
    CHECK(h.changes==1&&h.logs==1&&strstr(h.msg,"SUBMITTED by aryan"));
    CHECK(!strcmp(req.user,"not_the_session_user"));
    CHECK(sched_status_get(&svc,id,&out)==S_OK);
    CHECK(out.state==QUEUED&&!strcmp(out.req.user,"aryan"));
    out.req.need.cores=99;
    CHECK(job_find(&s,id)->req.need.cores==2);
    CHECK(job_start(&s,id,wid,20,&run)==S_OK);
    CHECK(sched_status_get(&svc,id,&out)==S_OK);
    CHECK(out.state==RUNNING&&out.worker_id==wid&&out.run_id==run);
    CHECK(sched_nodes_get(&svc,nodes,2,&count)==S_OK);
    CHECK(nodes[0].id==wid&&nodes[0].free.cores==2&&nodes[0].free.ram==1536);
    CHECK(nodes[0].free.gpu==0&&nodes[0].jobs==1&&nodes[1].id==other);
    nodes[0].total.cores=999;
    CHECK(worker_find(&s,wid)->total.cores==4);
    CHECK(job_finish(&s,id,run,FAILED,"exit 7",30)==S_OK);
    CHECK(sched_status_get(&svc,id,&out)==S_OK);
    CHECK(out.state==FAILED&&out.worker_id==0&&!strcmp(out.reason,"exit 7"));
    CHECK(h.changes==1&&h.logs==1);
    CHECK(sched_status_get(&svc,999,&out)==S_NOT_FOUND);
    CHECK(sched_status_get(&svc,0,&out)==S_BAD_INPUT);
    CHECK(sched_submit(&svc,"bad/user",&req,40,&id)==S_BAD_INPUT);
    CHECK(sched_nodes_get(&svc,NULL,1,&count)==S_BAD_INPUT);
    CHECK(sched_nodes_get(NULL,nodes,2,&count)==S_BAD_INPUT);
done:
    sched_free(&s);
    return ok;
}

static int full_queue(void)
{
    struct scheduler s;
    struct sched_service svc;
    struct job_req req=request();
    struct hooks h={0,0,{0}};
    struct sched_status out;
    struct sched_node node;
    size_t count;
    int i,id=0,wid,ok=1;
    sched_init(&s);
    CHECK(sched_service_init(&svc,&s,changed,log_msg,&h)==S_OK);
    CHECK(worker_add(&s,(struct resource){4,2048,1},0,&wid)==S_OK);
    for(i=0;i<QUEUE_CAP;i++) CHECK(sched_submit(&svc,"aryan",&req,(uint64_t)i,&id)==S_OK);
    CHECK(sched_submit(&svc,"aryan",&req,2000,&id)==S_QUEUE_FULL);
    CHECK(h.changes==QUEUE_CAP&&h.logs==QUEUE_CAP+1);
    CHECK(strstr(h.msg,"QUEUE_FULL")!=NULL);
    // Admission full hone se query band nahi honi chahiye. Query ka path
    // schedule callback ya work-queue wait ko touch hi nahi karta.
    CHECK(sched_status_get(&svc,QUEUE_CAP,&out)==S_OK&&out.state==QUEUED);
    CHECK(sched_nodes_get(&svc,&node,1,&count)==S_OK&&node.id==wid);
    CHECK(h.changes==QUEUE_CAP);
done:
    sched_free(&s);
    return ok;
}

int main(int argc,char **argv)
{
    struct { const char *name; int (*run)(void); } tests[]={
        {"snapshots",snapshots},{"full_queue",full_queue}
    };
    size_t i;
    int ran=0,failed=0;
    if(argc>2){ puts("usage: test_service [test-name]"); return 1; }
    for(i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        if(argc==2&&strcmp(argv[1],tests[i].name)) continue;
        ran++;
        if(tests[i].run()) printf("PASS service %s\n",tests[i].name);
        else failed++;
    }
    printf("%d service groups, %d checks, %d failed\n",ran,checks,failed);
    return !ran||failed?1:0;
}
