#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "internal.h"

uint64_t runner_now_ms(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)<0) return 0;
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}

const char *runner_error(int err)
{
    switch(err){
    case RUN_OK: return "OK";
    case RUN_BAD_INPUT: return "invalid runner input";
    case RUN_NO_MEMORY: return "memory allocation failed";
    case RUN_NOT_FOUND: return "execution not found";
    case RUN_IO: return "worker file/process operation failed";
    case RUN_BUSY: return "job ID is already present or output is still pending";
    default: return "unknown runner error";
    }
}

int runner_init(struct runner *r)
{
    struct passwd *ptr;
    if(!r) return RUN_BAD_INPUT;
    memset(r,0,sizeof(*r));
    r->uid=geteuid();
    r->gid=getegid();
    if(!r->uid){
        ptr=getpwnam("nobody");
        if(!ptr){ fprintf(stderr,"[WORKER] nobody user not found\n"); return RUN_IO; }
        r->uid=ptr->pw_uid;
        r->gid=ptr->pw_gid;
        r->drop_user=1;
    }else{
        fprintf(stderr,"[WORKER] Warning: not root; jobs run without privilege-dropping (chown/setgid/setuid skipped).\n");
    }
    return RUN_OK;
}

struct run_job *runner_find(struct runner *r,int id,uint64_t run_id)
{
    struct run_job *curr;
    if(!r) return NULL;
    for(curr=r->head;curr;curr=curr->next)
        if(curr->id==id&&curr->run_id==run_id) return curr;
    return NULL;
}

static void child_error(int fd,int stage)
{
    int msg[2]={stage,errno};
    ssize_t n;
    do { n=write(fd,msg,sizeof(msg)); } while(n<0&&errno==EINTR);
    _exit(127);
}

static void child_run(struct runner *r,struct run_job *j,char *const args[],int err_fd,long max_fd)
{
    struct rlimit lim;
    const char *path;
    int fd;
    long i;
    if(setpgid(0,0)<0) child_error(err_fd,1);
    signal(SIGINT,SIG_DFL);
    signal(SIGTERM,SIG_DFL);
    signal(SIGPIPE,SIG_DFL);
    signal(SIGCHLD,SIG_DFL);
    signal(SIGXCPU,SIG_DFL);

    if(fchdir(j->dir_fd)<0) child_error(err_fd,2);
    if(r->drop_user){
        if(fchown(j->dir_fd,r->uid,r->gid)<0||
           fchown(j->out_fd,r->uid,r->gid)<0) child_error(err_fd,3);
        if(strchr(j->req.path,'/')&&fchownat(j->dir_fd,"program",r->uid,r->gid,0)<0)
            child_error(err_fd,3);
        // Supplementary groups bhi hatao, phir pehle gid aur baad mein uid.
        // Root se drop fail hua toh program ko root mein bilkul mat chalao.
        if(setgroups(0,NULL)<0||setgid(r->gid)<0||setuid(r->uid)<0)
            child_error(err_fd,3);
    }
    lim.rlim_cur=(rlim_t)j->req.time_limit;
    lim.rlim_max=lim.rlim_cur+1;
    if(setrlimit(RLIMIT_CPU,&lim)<0) child_error(err_fd,4);
    lim.rlim_cur=lim.rlim_max=(rlim_t)((uint64_t)j->req.need.ram*1048576);
    if(setrlimit(RLIMIT_AS,&lim)<0) child_error(err_fd,4);
    // NPROC user-wide hota hai, har job ke liye separate 64 slots nahi milte.
    lim.rlim_cur=lim.rlim_max=64;
    if(setrlimit(RLIMIT_NPROC,&lim)<0) child_error(err_fd,4);
    lim.rlim_cur=lim.rlim_max=0;
    if(setrlimit(RLIMIT_CORE,&lim)<0) child_error(err_fd,4);

    fd=open("/dev/null",O_RDONLY);
    if(fd<0||dup2(fd,STDIN_FILENO)<0||dup2(j->out_fd,STDOUT_FILENO)<0||
       dup2(j->out_fd,STDERR_FILENO)<0) child_error(err_fd,5);
    // Worker ke sockets/fds exec mein leak nahi hone chahiye. Error pipe ko
    // chhod rahe hain; successful exec pe uska CLOEXEC khud close karega.
    for(i=3;i<max_fd;i++) if(i!=err_fd) close((int)i);
    path=strchr(j->req.path,'/')?"./program":j->req.path;
    execvp(path,args);
    child_error(err_fd,6);
}

int runner_start(struct runner *r,int id,uint64_t run_id,
                 const struct job_req *req,char *const args[])
{
    struct run_job *j,*curr;
    uint64_t bytes;
    long max_fd;
    int pipe_fd[2],flags,saved;
    if(!r||!req||!args||!args[0]||id<=0||!run_id||
       !memchr(req->path,'\0',PATH_LEN)||!req->path[0]||
       req->need.cores<=0||req->need.ram<=0||req->need.gpu<0||
       req->time_limit<=0||req->prio<1||req->prio>3) return RUN_BAD_INPUT;
    if((uint64_t)req->need.ram>UINT64_MAX/1048576) return RUN_BAD_INPUT;
    bytes=(uint64_t)req->need.ram*1048576;
    if((uint64_t)(rlim_t)bytes!=bytes) return RUN_BAD_INPUT;
    for(curr=r->head;curr;curr=curr->next) if(curr->id==id) return RUN_BUSY;
    j=calloc(1,sizeof(*j));
    if(!j) return RUN_NO_MEMORY;
    j->id=id; j->run_id=run_id; j->req=*req;
    j->dir_fd=j->out_fd=j->err_fd=-1;
    if(run_prepare(j,req->path)<0) goto fail;
    if(pipe(pipe_fd)<0) goto fail;
    j->err_fd=pipe_fd[0];
    if(fcntl(pipe_fd[0],F_SETFD,FD_CLOEXEC)<0||
       fcntl(pipe_fd[1],F_SETFD,FD_CLOEXEC)<0){ close(pipe_fd[1]); goto fail; }
    flags=fcntl(j->err_fd,F_GETFL);
    if(flags<0||fcntl(j->err_fd,F_SETFL,flags|O_NONBLOCK)<0){
        close(pipe_fd[1]); goto fail;
    }
    max_fd=sysconf(_SC_OPEN_MAX);
    if(max_fd<0) max_fd=1024;
    j->start_ms=runner_now_ms();
    j->pid=fork();
    if(j->pid==0){
        close(pipe_fd[0]);
        child_run(r,j,args,pipe_fd[1],max_fd);
    }
    close(pipe_fd[1]);
    if(j->pid<0) goto fail;
    // Parent bhi group set karta hai taaki immediate cancel child ke setup
    // se race na kare. Child already exec kar chuka ho toh error harmless hai.
    (void)setpgid(j->pid,j->pid);
    j->state=RUNNING;
    j->next=r->head;
    r->head=j;
    return RUN_OK;
fail:
    saved=errno;
    run_cleanup(j);
    free(j);
    errno=saved;
    return RUN_IO;
}

static void read_setup_error(struct run_job *j)
{
    ssize_t n;
    int msg[2];
    while(j->err_count<sizeof(j->err_buf)){
        n=read(j->err_fd,j->err_buf+j->err_count,sizeof(j->err_buf)-j->err_count);
        if(n<0&&errno==EINTR) continue;
        if(n<=0) break;
        j->err_count+=(size_t)n;
    }
    if(j->err_count==sizeof(j->err_buf)){
        memcpy(msg,j->err_buf,sizeof(msg));
        j->setup_stage=msg[0];
        j->setup_errno=msg[1];
    }
}

static void diagnose(struct run_job *j)
{
    static const char *stages[]={"unknown","process group","working directory",
        "privilege dropping","resource limits","output redirection","execvp"};
    int sig,code;
    j->state=FAILED;
    if(j->stop==STOP_CANCEL){
        j->state=CANCELLED;
        snprintf(j->reason,sizeof(j->reason),"Cancelled by request");
    }else if(j->stop==STOP_TIMEOUT||(WIFSIGNALED(j->status)&&WTERMSIG(j->status)==SIGXCPU)){
        snprintf(j->reason,sizeof(j->reason),"Wall-clock timeout exceeded (%ds limit)",j->req.time_limit);
    }else if(j->stop==STOP_MEMORY){
        snprintf(j->reason,sizeof(j->reason),"Memory limit exceeded (%lldMB)",j->req.need.ram);
    }else if(j->setup_stage){
        code=j->setup_stage;
        if(code<1||code>6) code=0;
        snprintf(j->reason,sizeof(j->reason),"Exited with code 127: %s failed: %s",
                 stages[code],strerror(j->setup_errno));
    }else if(WIFEXITED(j->status)){
        code=WEXITSTATUS(j->status);
        if(!code) j->state=COMPLETED;
        snprintf(j->reason,sizeof(j->reason),"Exited with code %d",code);
    }else if(WIFSIGNALED(j->status)){
        sig=WTERMSIG(j->status);
        snprintf(j->reason,sizeof(j->reason),"Terminated by signal %d (%s)",sig,strsignal(sig));
    }else snprintf(j->reason,sizeof(j->reason),"Unexpected child status");
}

int runner_poll(struct runner *r,output_fn output,void *arg)
{
    struct run_job *j;
    struct rusage usage;
    char buf[16384];
    ssize_t n;
    pid_t pid;
    uint64_t now;
    long long peak;
    int count=0;
    if(!r) return -1;
    now=runner_now_ms();
    for(j=r->head;j;j=j->next){
        if(j->finished) continue;
        read_setup_error(j);
        if(!j->reaped){
            run_sample(j);
            do { pid=wait4(j->pid,&j->status,WNOHANG,&usage); } while(pid<0&&errno==EINTR);
            if(pid<0) return -1;
            if(pid==j->pid){
                j->reaped=1;
                // Main process exit ke baad bache hue same-group children ko
                // bhi stop karo; warna output.log mein likhte reh sakte hain.
                (void)kill(-j->pid,SIGKILL);
                read_setup_error(j);
                j->usage.cpu_time=(double)usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1000000.0+
                                  usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1000000.0;
                j->usage.faults=(uint64_t)usage.ru_minflt+(uint64_t)usage.ru_majflt;
#ifdef __APPLE__
                peak=(long long)((usage.ru_maxrss+1048575)/1048576);
#else
                peak=(long long)((usage.ru_maxrss+1023)/1024);
#endif
                if(peak>j->usage.peak_ram) j->usage.peak_ram=peak;
                diagnose(j);
            }else if(j->stop==STOP_NONE){
                if(now>=j->start_ms&&now-j->start_ms>=(uint64_t)j->req.time_limit*1000){
                    if(kill(-j->pid,SIGKILL)<0&&errno!=ESRCH) return -1;
                    j->stop=STOP_TIMEOUT;
                }
#ifdef __APPLE__
                else if(j->usage.ram>j->req.need.ram){
                    if(kill(-j->pid,SIGKILL)<0&&errno!=ESRCH) return -1;
                    j->stop=STOP_MEMORY;
                }
#endif
            }
        }
        // pread se parent ka read position aur child ka write position alag
        // rehta hai. Ek poll mein bounded chunk, taaki dusri jobs starve na ho.
        do { n=pread(j->out_fd,buf,sizeof(buf),j->offset); } while(n<0&&errno==EINTR);
        if(n<0) return -1;
        if(n>0){
            if(output&&output(j->id,j->run_id,buf,(size_t)n,arg)==0) j->offset+=n;
        }else if(j->reaped){
            j->finished=1;
            count++;
        }
    }
    return count;
}

int runner_cancel(struct runner *r,int id,uint64_t run_id)
{
    struct run_job *j=runner_find(r,id,run_id);
    if(!j) return RUN_NOT_FOUND;
    if(j->reaped||j->stop!=STOP_NONE) return RUN_BUSY;
    if(kill(-j->pid,SIGKILL)<0&&errno!=ESRCH) return RUN_IO;
    j->stop=STOP_CANCEL;
    return RUN_OK;
}

int runner_remove(struct runner *r,int id,uint64_t run_id)
{
    struct run_job **ptr,*j;
    int ok;
    if(!r) return RUN_BAD_INPUT;
    for(ptr=&r->head;*ptr;ptr=&(*ptr)->next){
        j=*ptr;
        if(j->id!=id||j->run_id!=run_id) continue;
        if(!j->finished) return RUN_BUSY;
        // Scheduler ko final output/result send karne ke baad caller remove
        // karega. Poll ke andar folder delete karna jaldi ho jayega.
        ok=run_cleanup(j);
        *ptr=j->next;
        free(j);
        return ok<0?RUN_IO:RUN_OK;
    }
    return RUN_NOT_FOUND;
}

void runner_free(struct runner *r)
{
    struct run_job *curr,*temp;
    if(!r) return;
    curr=r->head;
    while(curr){
        temp=curr->next;
        if(!curr->reaped){
            (void)kill(-curr->pid,SIGKILL);
            while(waitpid(curr->pid,NULL,0)<0&&errno==EINTR){}
        }
        run_cleanup(curr);
        free(curr);
        curr=temp;
    }
    r->head=NULL;
}
