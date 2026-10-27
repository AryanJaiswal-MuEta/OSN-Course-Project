#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "runner.h"

static volatile sig_atomic_t stop;

static void stop_handler(int sig)
{
    (void)sig;
    stop=1;
}

static int output(int id,uint64_t run_id,const void *buf,size_t len,void *arg)
{
    (void)id; (void)run_id; (void)arg;
    if(fwrite(buf,1,len,stdout)!=len) return -1;
    return fflush(stdout);
}

static int number(const char *str,long long *n)
{
    char *end;
    errno=0;
    *n=strtoll(str,&end,10);
    return !errno&&end!=str&&!*end&&*n>0;
}

int main(int argc,char **argv)
{
    struct runner r;
    struct run_job *j;
    struct job_req req;
    struct sigaction sa;
    struct timespec delay={0,50000000};
    long long n;
    uint64_t run_id=1;
    int id=(int)getpid(),i,err,result;
    memset(&req,0,sizeof(req));
    strcpy(req.user,"local");
    req.need=(struct resource){1,64,0};
    req.time_limit=5;
    req.prio=2;
    for(i=1;i<argc&&strcmp(argv[i],"--");i+=2){
        if(i+1>=argc||!number(argv[i+1],&n)) goto usage;
        if(!strcmp(argv[i],"-id")&&n<=INT_MAX) id=(int)n;
        else if(!strcmp(argv[i],"-run")) run_id=(uint64_t)n;
        else if(!strcmp(argv[i],"-ram")) req.need.ram=n;
        else if(!strcmp(argv[i],"-time")&&n<=INT_MAX) req.time_limit=(int)n;
        else goto usage;
    }
    if(i>=argc||i+1>=argc||strlen(argv[i+1])>=PATH_LEN) goto usage;
    strcpy(req.path,argv[i+1]);
    memset(&sa,0,sizeof(sa));
    sa.sa_handler=stop_handler;
    sigemptyset(&sa.sa_mask);
    if(sigaction(SIGINT,&sa,NULL)<0||sigaction(SIGTERM,&sa,NULL)<0){ perror("sigaction"); return 1; }
    err=runner_init(&r);
    if(err!=RUN_OK){ fprintf(stderr,"%s\n",runner_error(err)); return 1; }
    err=runner_start(&r,id,run_id,&req,&argv[i+1]);
    if(err!=RUN_OK){
        if(err==RUN_IO) fprintf(stderr,"%s: %s\n",runner_error(err),strerror(errno));
        else fprintf(stderr,"%s\n",runner_error(err));
        runner_free(&r);
        return 1;
    }
    j=runner_find(&r,id,run_id);
    while(!j->finished){
        if(stop){
            (void)runner_cancel(&r,id,run_id);
            stop=0;
        }
        if(runner_poll(&r,output,NULL)<0){ perror("runner_poll"); runner_free(&r); return 1; }
        if(ferror(stdout)){ runner_free(&r); return 1; }
        if(!j->finished) nanosleep(&delay,NULL);
    }
    fprintf(stderr,"[RUNNER] Job %d run %llu: %s; %s; peak RAM %lldMB; CPU %.3fs\n",
            id,(unsigned long long)run_id,j->state==COMPLETED?"COMPLETED":
            j->state==CANCELLED?"CANCELLED":"FAILED",j->reason,j->usage.peak_ram,j->usage.cpu_time);
    result=j->state==COMPLETED?0:j->state==CANCELLED?2:1;
    if(runner_remove(&r,id,run_id)!=RUN_OK){ perror("runner_remove"); result=1; }
    runner_free(&r);
    return result;
usage:
    fprintf(stderr,"usage: %s [-id N] [-run N] [-ram MB] [-time SEC] -- program [args...]\n",argv[0]);
    return 2;
}
