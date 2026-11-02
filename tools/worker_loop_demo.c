#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "worker_loop.h"

struct console {
    char line[2048];
    size_t len;
    int discard;
    int eof;
    int error;
};

static volatile sig_atomic_t interrupted;

static void signal_stop(int sig)
{
    (void)sig;
    interrupted=1;
}

static int number(const char *str,long long max,long long *n)
{
    char *end;
    errno=0;
    *n=strtoll(str,&end,10);
    return !errno&&end!=str&&!*end&&*n>0&&*n<=max;
}

static void help(void)
{
    puts("run <job_id> <run_id> <ram_MB> <time_s> <program> [args...]");
    puts("cancel <job_id> <run_id>");
    puts("jobs | help | quit");
    puts("EOF waits for active jobs; quit/Ctrl+C aborts them (not drain).");
}

static void command(struct worker_loop *loop,char *line)
{
    char *args[32],*ptr,*save;
    struct job_req req;
    struct run_job *curr;
    long long id,run,ram,secs;
    int count=0,err=RUN_BAD_INPUT;
    ptr=strtok_r(line," \t\r",&save);
    while(ptr&&count<31){
        args[count++]=ptr;
        ptr=strtok_r(NULL," \t\r",&save);
    }
    args[count]=NULL;
    if(ptr){ puts("too many arguments"); return; }
    if(!count) return;
    if(!strcmp(args[0],"help")&&count==1){ help(); return; }
    if(!strcmp(args[0],"quit")&&count==1){ worker_loop_stop(loop); return; }
    if(!strcmp(args[0],"jobs")&&count==1){
        for(curr=loop->runner.head;curr;curr=curr->next)
            printf("Job %d run %llu pid %ld %s\n",curr->id,(unsigned long long)curr->run_id,
                   (long)curr->pid,curr->finished?"result pending":curr->reaped?"output pending":"active");
        return;
    }
    if(!strcmp(args[0],"run")&&count>=6&&number(args[1],INT_MAX,&id)&&
       number(args[2],LLONG_MAX,&run)&&number(args[3],LLONG_MAX,&ram)&&
       number(args[4],INT_MAX,&secs)&&strlen(args[5])<PATH_LEN){
        memset(&req,0,sizeof(req));
        strcpy(req.user,"local");
        strcpy(req.path,args[5]);
        req.need=(struct resource){1,ram,0};
        req.time_limit=(int)secs;
        req.prio=2;
        err=worker_loop_assign(loop,(int)id,(uint64_t)run,&req,&args[5]);
        if(err==RUN_OK) printf("Job %lld run %lld accepted locally\n",id,run);
    }else if(!strcmp(args[0],"cancel")&&count==3&&number(args[1],INT_MAX,&id)&&
             number(args[2],LLONG_MAX,&run)){
        err=worker_loop_cancel(loop,(int)id,(uint64_t)run);
    }
    if(err!=RUN_OK){
        if(err==RUN_IO) printf("%s: %s\n",runner_error(err),strerror(errno));
        else puts(runner_error(err));
    }
}

static void io(struct worker_loop *loop,int fd,short events,void *arg)
{
    struct console *c=arg;
    char buf[512];
    ssize_t n;
    size_t i;
    if(events&(POLLERR|POLLNVAL)){ c->error=1; worker_loop_stop(loop); return; }
    if(!(events&(POLLIN|POLLHUP))) return;
    n=read(fd,buf,sizeof(buf));
    if(n<0){
        if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR) return;
        c->error=1; worker_loop_stop(loop); return;
    }
    if(!n){
        if(c->len&&!c->discard){ c->line[c->len]='\0'; command(loop,c->line); }
        c->len=0;
        c->eof=1;
        if(worker_loop_watch(loop,WORKER_ADMIN,-1,0)<0){ c->error=1; worker_loop_stop(loop); }
        return;
    }
    for(i=0;i<(size_t)n&&!loop->stop;i++){
        if(buf[i]=='\n'){
            if(!c->discard){ c->line[c->len]='\0'; command(loop,c->line); }
            c->len=0;
            c->discard=0;
        }else if(!c->discard){
            if(c->len==sizeof(c->line)-1){
                puts("command too long");
                c->discard=1;
            }else c->line[c->len++]=buf[i];
        }
    }
    fflush(stdout);
}

static int output(int id,uint64_t run,const void *buf,size_t len,void *arg)
{
    struct console *c=arg;
    if(printf("[Job %d run %llu] ",id,(unsigned long long)run)<0||
       fwrite(buf,1,len,stdout)!=len||fflush(stdout)<0){ c->error=1; return 1; }
    return 0;
}

static int result(const struct worker_result *r,void *arg)
{
    struct console *c=arg;
    const char *state=r->state==COMPLETED?"COMPLETED":r->state==CANCELLED?"CANCELLED":"FAILED";
    if(printf("[Result] Job %d run %llu %s: %s\n",r->id,
              (unsigned long long)r->run_id,state,r->reason)<0||fflush(stdout)<0){
        c->error=1;
        return 1;
    }
    return 0;
}

static void tick(struct worker_loop *loop,uint64_t now,void *arg)
{
    struct console *c=arg;
    (void)now;
    if(interrupted||c->error||(c->eof&&!loop->runner.head)) worker_loop_stop(loop);
}

int main(void)
{
    struct worker_loop loop;
    struct worker_hooks hooks={io,tick,output,result};
    struct console c={0};
    struct sigaction sa;
    int flags,err;
    memset(&sa,0,sizeof(sa));
    sa.sa_handler=signal_stop;
    sigemptyset(&sa.sa_mask);
    if(sigaction(SIGINT,&sa,NULL)<0||sigaction(SIGTERM,&sa,NULL)<0){ perror("sigaction"); return 1; }
    err=worker_loop_init(&loop,&hooks,&c);
    if(err!=RUN_OK){ fprintf(stderr,"%s\n",runner_error(err)); return 1; }
    flags=fcntl(STDIN_FILENO,F_GETFL);
    if(flags<0||worker_loop_watch(&loop,WORKER_ADMIN,STDIN_FILENO,POLLIN)<0){
        perror("worker_loop_watch");
        if(flags>=0) (void)fcntl(STDIN_FILENO,F_SETFL,flags);
        worker_loop_free(&loop);
        return 1;
    }
    puts("Local worker loop driver; no scheduler connection yet.");
    help();
    fflush(stdout);
    if(worker_loop_run(&loop)<0){ perror("worker_loop_run"); c.error=1; }
    worker_loop_free(&loop);
    // Stdin ke status flags shell ke saath shared ho sakte hain. Driver
    // exit pe O_NONBLOCK restore karo taaki terminal ka behaviour na badle.
    if(fcntl(STDIN_FILENO,F_SETFL,flags)<0){ perror("restore stdin"); return 1; }
    return c.error?1:0;
}
