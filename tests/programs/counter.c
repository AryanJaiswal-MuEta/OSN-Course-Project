#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop(int sig){ (void)sig; stopping=1; }

int main(void)
{
    const char *dir=getenv("OSN_CHECKPOINT_DIR");
    char state[512],temp[512];
    FILE *fp;
    struct timespec delay={0,100000000};
    int i=0,total=50;
    if(!dir) return 2;
    if(snprintf(state,sizeof(state),"%s/state",dir)>=(int)sizeof(state)||
       snprintf(temp,sizeof(temp),"%s/state.tmp",dir)>=(int)sizeof(temp)) return 2;
    fp=fopen(state,"r");
    if(fp){ if(fscanf(fp,"%d",&i)!=1||i<0||i>total){ fclose(fp); return 3; } fclose(fp); }
    signal(SIGTERM,stop);
    printf("resume=%d uid=%ld\n",i,(long)getuid()); fflush(stdout);
    while(i<total&&!stopping){
        i++;
        fp=fopen(temp,"w"); if(!fp) return 4;
        if(fprintf(fp,"%d\n",i)<0||fclose(fp)!=0) return 4;
        if(rename(temp,state)<0) return 4;
        printf("progress=%d\n",i); fflush(stdout);
        struct timespec left=delay;
        while(nanosleep(&left,&left)<0&&errno==EINTR&&!stopping){}
    }
    return 0;
}
