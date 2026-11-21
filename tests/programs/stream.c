#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
    char buf[4096];
    struct timespec delay;
    int i;
    memset(buf,'x',sizeof(buf));
    for(i=0;i<500;i++){
        if(fwrite(buf,1,sizeof(buf),stdout)!=sizeof(buf)||fflush(stdout)!=0) return 1;
        delay=(struct timespec){0,10000000};
        while(nanosleep(&delay,&delay)<0&&errno==EINTR){}
    }
    return 0;
}
