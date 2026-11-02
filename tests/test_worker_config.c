#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "worker_config.h"

static int checks;
#define CHECK(expr) do { checks++; if(!(expr)){ \
    printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); return 0; \
} } while(0)

static int parse(char *const args[],struct worker_config *cfg,char *err)
{
    int count=0;
    while(args[count]) count++;
    return worker_config_parse(count,args,cfg,err,160);
}

static int options(void)
{
    struct worker_config cfg;
    char err[160];
    char *basic[]={"worker","-cores","4","-ram","2048",NULL};
    char *full[]={"worker","127.0.0.1","8080","-ram","4096","-gpu","3",
                  "-discovery-port","10001","-cores","8",NULL};
    CHECK(parse(basic,&cfg,err)==0);
    CHECK(cfg.capacity.cores==4&&cfg.capacity.ram==2048&&cfg.capacity.gpu==0);
    CHECK(!cfg.has_fallback&&cfg.scheduler_port==0&&!cfg.scheduler_ip[0]);
    CHECK(cfg.discovery_port==9999&&!err[0]);
    CHECK(parse(full,&cfg,err)==0);
    CHECK(cfg.has_fallback&&!strcmp(cfg.scheduler_ip,"127.0.0.1")&&cfg.scheduler_port==8080);
    CHECK(cfg.capacity.cores==8&&cfg.capacity.ram==4096&&cfg.capacity.gpu==3);
    CHECK(cfg.discovery_port==10001);
    return 1;
}

static int invalid(void)
{
    struct worker_config cfg,before;
    char err[160];
    char *bad[][14]={
        {"worker",NULL},
        {"worker","-cores","1",NULL},
        {"worker","-cores","0","-ram","100",NULL},
        {"worker","-cores","-1","-ram","100",NULL},
        {"worker","-cores","2x","-ram","100",NULL},
        {"worker","-cores","1","-ram","0",NULL},
        {"worker","-cores","1","-ram","9999999999999999999999999",NULL},
        {"worker","-cores","1","-ram","100","-gpu","-1",NULL},
        {"worker","-cores","1","-ram","100","-gpu",NULL},
        {"worker","-cores","1","-ram","100","-cores","2",NULL},
        {"worker","-cores","1","-ram","100","-gpu","0","-gpu","1",NULL},
        {"worker","-cores","1","-ram","100","-unknown","2",NULL},
        {"worker","-cores","1","-ram","100","-discovery-port","65536",NULL},
        {"worker","-cores","1","-ram","100","-discovery-port","0",NULL},
        {"worker","127.0.0.1",NULL},
        {"worker","999.1.1.1","8080","-cores","1","-ram","100",NULL},
        {"worker","127.0.0.1","0","-cores","1","-ram","100",NULL},
        {"worker","-cores","1","-ram","100","127.0.0.1","8080",NULL}
    };
    size_t i;
    memset(&cfg,0,sizeof(cfg));
    cfg.capacity.cores=7;
    before=cfg;
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
        CHECK(parse(bad[i],&cfg,err)==-1&&err[0]);
        CHECK(memcmp(&cfg,&before,sizeof(cfg))==0);
    }
    CHECK(worker_config_parse(0,NULL,&cfg,err,sizeof(err))==-1);
    CHECK(worker_config_parse(1,bad[0],NULL,err,sizeof(err))==-1);
    return 1;
}

static int clamp(void)
{
    struct worker_config cfg={0};
    struct resource total={4,2048,0};
    char warn[160];
    cfg.capacity=(struct resource){8,4096,7};
    CHECK(worker_capacity_limit(&cfg,total,warn,sizeof(warn))==3);
    CHECK(cfg.capacity.cores==4&&cfg.capacity.ram==2048&&cfg.capacity.gpu==7);
    CHECK(strstr(warn,"cores clamped 8 -> 4")&&strstr(warn,"RAM clamped 4096 -> 2048"));
    CHECK(worker_capacity_limit(&cfg,total,warn,sizeof(warn))==0&&!warn[0]);
    cfg.capacity=(struct resource){2,1024,7};
    CHECK(worker_capacity_limit(&cfg,total,warn,sizeof(warn))==0);
    CHECK(cfg.capacity.cores==2&&cfg.capacity.ram==1024&&cfg.capacity.gpu==7);
    cfg.capacity=(struct resource){8,1024,0};
    CHECK(worker_capacity_limit(&cfg,total,warn,sizeof(warn))==WORKER_CLAMP_CORES);
    CHECK(strstr(warn,"cores")&&!strstr(warn,"RAM"));
    cfg.capacity=(struct resource){2,4096,0};
    CHECK(worker_capacity_limit(&cfg,total,warn,sizeof(warn))==WORKER_CLAMP_RAM);
    CHECK(strstr(warn,"RAM")&&!strstr(warn,"cores"));
    CHECK(worker_capacity_limit(&cfg,(struct resource){0,1,0},warn,sizeof(warn))==-1);
    CHECK(worker_capacity_limit(NULL,total,warn,sizeof(warn))==-1);
    return 1;
}

static int physical(void)
{
    struct resource total;
    struct worker_config cfg={0};
    char warn[160];
    CHECK(worker_capacity_detect(&total)==0);
    CHECK(total.cores>0&&total.ram>0&&total.gpu==0);
    cfg.capacity=(struct resource){INT_MAX,LLONG_MAX,5};
    CHECK(worker_capacity_limit(&cfg,total,warn,sizeof(warn))>=0);
    CHECK(cfg.capacity.cores==total.cores&&cfg.capacity.ram==total.ram&&cfg.capacity.gpu==5);
    CHECK(worker_capacity_detect(NULL)==-1);
    return 1;
}

int main(int argc,char **argv)
{
    struct { const char *name; int (*run)(void); } tests[]={
        {"options",options},{"invalid",invalid},{"clamp",clamp},{"physical",physical}
    };
    size_t i;
    int ran=0,failed=0;
    if(argc>2){ puts("usage: test_worker_config [test-name]"); return 1; }
    for(i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        if(argc==2&&strcmp(argv[1],tests[i].name)) continue;
        ran++;
        if(tests[i].run()) printf("PASS worker config %s\n",tests[i].name);
        else failed++;
    }
    printf("%d config groups, %d checks, %d failed\n",ran,checks,failed);
    return !ran||failed?1:0;
}
