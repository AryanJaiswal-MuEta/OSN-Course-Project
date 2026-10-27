#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "scheduler.h"

static uint64_t now_ms(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)<0){
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}

static int number(const char *str,long long min,long long max,long long *val)
{
    char *end;
    long long n;
    errno=0;
    n=strtoll(str,&end,10);
    if(errno||end==str||*end||n<min||n>max) return 0;
    *val=n;
    return 1;
}

static int read_id(const char *str,int *id)
{
    long long n;
    if(!number(str,1,INT_MAX,&n)) return 0;
    *id=(int)n;
    return 1;
}

static int read_run(const char *str,uint64_t *run)
{
    char *end;
    unsigned long long n;
    if(*str=='-'||*str=='+') return 0;
    errno=0;
    n=strtoull(str,&end,10);
    if(errno||end==str||*end||!n||n>UINT64_MAX) return 0;
    *run=(uint64_t)n;
    return 1;
}

static void help(void)
{
    puts("worker <cores> <ram_MB> <gpu>");
    puts("submit <user> <path> <cores> <ram_MB> <time_s> <prio> [gpu]");
    puts("nodes | jobs | status <job_id>");
    puts("start <job_id> <worker_id>");
    puts("finish <job_id> <run_id> COMPLETED|FAILED|CANCELLED");
    puts("cancel <job_id>  (pending jobs only)");
    puts("worker-state <worker_id> ALIVE|SUSPECTED|DEAD|DRAINING");
    puts("help | quit");
}

static void nodes(struct scheduler *s)
{
    struct worker *curr;
    struct resource avail;
    puts("ID State Cores(free/total) RAM_MB(free/total) GPU(free/total) Jobs");
    for(curr=s->workers;curr;curr=curr->next){
        avail=worker_free(curr);
        printf("%d %s %d/%d %lld/%lld %d/%d %zu\n",curr->id,
               worker_state_name(curr->state),avail.cores,curr->total.cores,
               avail.ram,curr->total.ram,avail.gpu,curr->total.gpu,curr->jobs);
    }
}

static void show_job(const struct job *j)
{
    printf("Job %d User:%s State:%s Worker:%d LastWorker:%d Run:%llu\n",
           j->id,j->req.user,job_state_name(j->state),j->worker_id,j->last_worker,
           (unsigned long long)j->run_id);
    printf("  Path:%s Cores:%d RAM:%lldMB GPU:%d Time:%ds Priority:%d\n",
           j->req.path,j->req.need.cores,j->req.need.ram,j->req.need.gpu,
           j->req.time_limit,j->req.prio);
    if(j->reason[0]) printf("  Reason:%s\n",j->reason);
}

int main(void)
{
    struct scheduler s;
    struct job_req req;
    struct job *curr;
    struct resource res;
    char buf[2048],*args[12],*ptr;
    long long a,b,c,d,e;
    uint64_t run;
    int count,id,wid,err,ch;
    size_t i;

    sched_init(&s);
    puts("Scheduler core driver: records/accounting only, no processes or sockets.");
    help();
    while(1){
        printf("core> ");
        fflush(stdout);
        if(!fgets(buf,sizeof(buf),stdin)) break;
        // Lambi input ko next command ki tarah process nahi karna hai.
        if(!strchr(buf,'\n')&&!feof(stdin)){
            while((ch=getchar())!='\n'&&ch!=EOF){}
            puts("input too long");
            continue;
        }
        count=0;
        ptr=strtok(buf," \t\r\n");
        while(ptr&&count<12){
            args[count++]=ptr;
            ptr=strtok(NULL," \t\r\n");
        }
        if(ptr){ puts("too many arguments"); continue; }
        if(!count) continue;
        if(!strcmp(args[0],"quit")&&count==1) break;
        if(!strcmp(args[0],"help")&&count==1){ help(); continue; }
        if(!strcmp(args[0],"nodes")&&count==1){ nodes(&s); continue; }
        if(!strcmp(args[0],"jobs")&&count==1){
            for(curr=s.jobs;curr;curr=curr->next) show_job(curr);
            continue;
        }
        if(!strcmp(args[0],"status")&&count==2&&read_id(args[1],&id)){
            curr=job_find(&s,id);
            if(curr) show_job(curr);
            else puts("job not found");
            continue;
        }
        err=S_BAD_INPUT;
        if(!strcmp(args[0],"worker")&&count==4&&
           number(args[1],1,INT_MAX,&a)&&number(args[2],1,LLONG_MAX,&b)&&
           number(args[3],0,INT_MAX,&c)){
            res=(struct resource){(int)a,b,(int)c};
            err=worker_add(&s,res,now_ms(),&id);
            if(err==S_OK) printf("worker registered: %d\n",id);
        }else if(!strcmp(args[0],"submit")&&(count==7||count==8)&&
                 strlen(args[1])<USER_LEN&&strlen(args[2])<PATH_LEN&&
                 number(args[3],1,INT_MAX,&a)&&number(args[4],1,LLONG_MAX,&b)&&
                 number(args[5],1,INT_MAX,&c)&&number(args[6],1,3,&d)){
            e=0;
            if(count==8&&!number(args[7],0,INT_MAX,&e)){
                puts("invalid GPU count");
                continue;
            }
            memset(&req,0,sizeof(req));
            strcpy(req.user,args[1]);
            strcpy(req.path,args[2]);
            req.need=(struct resource){(int)a,b,(int)e};
            req.time_limit=(int)c;
            req.prio=(int)d;
            err=job_add(&s,&req,now_ms(),&id);
            if(err==S_OK) printf("job submitted: %d\n",id);
        }else if(!strcmp(args[0],"start")&&count==3&&
                 read_id(args[1],&id)&&read_id(args[2],&wid)){
            err=job_start(&s,id,wid,now_ms(),&run);
            if(err==S_OK) printf("job %d RUNNING, run %llu\n",id,(unsigned long long)run);
        }else if(!strcmp(args[0],"finish")&&count==4&&
                 read_id(args[1],&id)&&read_run(args[2],&run)){
            enum job_state state=WAITING;
            if(!strcmp(args[3],"COMPLETED")) state=COMPLETED;
            if(!strcmp(args[3],"FAILED")) state=FAILED;
            if(!strcmp(args[3],"CANCELLED")) state=CANCELLED;
            err=job_finish(&s,id,run,state,"manual result from core driver",now_ms());
        }else if(!strcmp(args[0],"cancel")&&count==2&&read_id(args[1],&id)){
            err=job_cancel_pending(&s,id,"cancelled in core driver",now_ms());
        }else if(!strcmp(args[0],"worker-state")&&count==3&&read_id(args[1],&id)){
            for(i=0;i<4;i++){
                if(!strcmp(args[2],worker_state_name((enum worker_state)i))){
                    err=worker_set_state(&s,id,(enum worker_state)i);
                    break;
                }
            }
        }
        printf("%s\n",sched_error(err));
    }
    sched_free(&s);
    return ferror(stdin)?1:0;
}
