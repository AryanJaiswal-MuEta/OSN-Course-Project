#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(long ms)
{
    struct timespec ts={ms/1000,(ms%1000)*1000000};
    while(nanosleep(&ts,&ts)<0&&errno==EINTR){}
}

int main(int argc,char **argv)
{
    char buf[512],*ptr;
    struct rlimit lim;
    size_t i;
    int n;
    if(argc<2) return 2;
    if(!strcmp(argv[1],"hello")){
        if(!getcwd(buf,sizeof(buf))) return 3;
        printf("cwd=%s uid=%ld gid=%ld\n",buf,(long)getuid(),(long)getgid());
        puts("hello stdout");
        fflush(stdout);
        fprintf(stderr,"hello stderr\n");
    }else if(!strcmp(argv[1],"stream")){
        for(i=0;i<4;i++){
            printf("tick %zu\n",i);
            fflush(stdout);
            sleep_ms(200);
        }
    }else if(!strcmp(argv[1],"sleep")) sleep_ms(argc>2?strtol(argv[2],NULL,10):5000);
    else if(!strcmp(argv[1],"cpu")){
        volatile unsigned long value=0;
        while(1) value++;
    }else if(!strcmp(argv[1],"exit")) return 7;
    else if(!strcmp(argv[1],"segv")) raise(SIGSEGV);
    else if(!strcmp(argv[1],"spam")){
        memset(buf,'x',sizeof(buf));
        for(i=0;i<80;i++) if(fwrite(buf,1,sizeof(buf),stdout)!=sizeof(buf)) return 4;
    }else if(!strcmp(argv[1],"memory")){
        ptr=malloc(128*1024*1024);
        if(!ptr){ puts("allocation rejected"); return 42; }
        for(i=0;i<128*1024*1024;i+=4096) ptr[i]='a';
        puts("allocation accepted");
        fflush(stdout);
        sleep_ms(1000);
        free(ptr);
    }else if(!strcmp(argv[1],"limits")){
        if(getrlimit(RLIMIT_AS,&lim)<0) return 3;
        printf("address_limit=%llu\n",(unsigned long long)lim.rlim_cur);
        if(getrlimit(RLIMIT_CPU,&lim)<0) return 3;
        printf("cpu_limit=%llu\n",(unsigned long long)lim.rlim_cur);
        if(getrlimit(RLIMIT_NPROC,&lim)<0) return 3;
        printf("process_limit=%llu\n",(unsigned long long)lim.rlim_cur);
    }else if(!strcmp(argv[1],"nested")){
        if(mkdir("nested",0700)<0||symlink("/tmp","nested/link")<0) return 3;
        puts("nested link created");
    }else if(!strcmp(argv[1],"fd")&&argc==3){
        n=(int)strtol(argv[2],NULL,10);
        if(fcntl(n,F_GETFD)>=0){ puts("fd leaked"); return 3; }
        puts("fd closed");
    }else return 2;
    return 0;
}
