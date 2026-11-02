#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#include "worker_config.h"

static int fail(char *err,size_t cap,const char *msg)
{
    if(err&&cap) snprintf(err,cap,"%s",msg);
    errno=EINVAL;
    return -1;
}

static int number(const char *str,long long min,long long max,long long *n)
{
    char *end;
    if(!str||str[0]<'0'||str[0]>'9') return 0;
    errno=0;
    *n=strtoll(str,&end,10);
    return !errno&&!*end&&*n>=min&&*n<=max;
}

int worker_config_parse(int argc,char *const argv[],struct worker_config *cfg,
                        char *err,size_t cap)
{
    struct worker_config temp;
    struct in_addr ip;
    long long n;
    unsigned int seen=0,flag;
    int i=1;
    if(err&&cap) err[0]='\0';
    if(!cfg||!argv||argc<1) return fail(err,cap,"invalid argument list");
    memset(&temp,0,sizeof(temp));
    temp.discovery_port=9999;
    if(i<argc&&argv[i]&&argv[i][0]!='-'){
        if(i+1>=argc||inet_pton(AF_INET,argv[i],&ip)!=1||
           !number(argv[i+1],1,65535,&n)) return fail(err,cap,"invalid fallback IP/port");
        snprintf(temp.scheduler_ip,sizeof(temp.scheduler_ip),"%s",argv[i]);
        temp.scheduler_port=(int)n;
        temp.has_fallback=1;
        i+=2;
    }
    for(;i<argc;i+=2){
        if(!argv[i]||i+1>=argc||!argv[i+1]) return fail(err,cap,"option needs a value");
        if(!strcmp(argv[i],"-cores")) flag=1;
        else if(!strcmp(argv[i],"-ram")) flag=2;
        else if(!strcmp(argv[i],"-gpu")) flag=4;
        else if(!strcmp(argv[i],"-discovery-port")) flag=8;
        else return fail(err,cap,"unknown worker option");
        if(seen&flag) return fail(err,cap,"duplicate worker option");
        if(!number(argv[i+1],flag==4?0:1,flag==2?LLONG_MAX:flag==8?65535:INT_MAX,&n))
            return fail(err,cap,"invalid worker option value");
        seen|=flag;
        if(flag==1) temp.capacity.cores=(int)n;
        if(flag==2) temp.capacity.ram=n;
        if(flag==4) temp.capacity.gpu=(int)n;
        if(flag==8) temp.discovery_port=(int)n;
    }
    if((seen&3)!=3) return fail(err,cap,"-cores and -ram are required");
    // Parsing fail ho toh caller ka purana config untouched rahega.
    // Fallback sirf store kar rahe hain; discovery se pehle connect nahi karna.
    *cfg=temp;
    return 0;
}

int worker_capacity_detect(struct resource *total)
{
    struct resource temp={0,0,0};
    uint64_t bytes;
    long cores;
#ifdef __APPLE__
    int mib[2]={CTL_HW,HW_MEMSIZE};
    size_t len=sizeof(bytes);
#else
    long pages,page_size;
#endif
    if(!total){ errno=EINVAL; return -1; }
    cores=sysconf(_SC_NPROCESSORS_ONLN);
    if(cores<=0||cores>INT_MAX){ errno=EOVERFLOW; return -1; }
#ifdef __APPLE__
    if(sysctl(mib,2,&bytes,&len,NULL,0)<0) return -1;
    if(len!=sizeof(bytes)){ errno=EIO; return -1; }
#else
    pages=sysconf(_SC_PHYS_PAGES);
    page_size=sysconf(_SC_PAGESIZE);
    if(pages<=0||page_size<=0){ errno=EIO; return -1; }
    // Pehle overflow check, phir pages*page_size. RAM ko MB mein floor
    // karenge taaki physical bytes se zyada advertise na ho.
    if((uint64_t)pages>UINT64_MAX/(uint64_t)page_size){ errno=EOVERFLOW; return -1; }
    bytes=(uint64_t)pages*(uint64_t)page_size;
#endif
    temp.cores=(int)cores;
    temp.ram=(long long)(bytes/1048576);
    if(temp.ram<=0){ errno=EIO; return -1; }
    *total=temp;
    return 0;
}

int worker_capacity_limit(struct worker_config *cfg,struct resource total,
                          char *warn,size_t cap)
{
    int mask=0,cores;
    long long ram;
    if(warn&&cap) warn[0]='\0';
    if(!cfg||cfg->capacity.cores<=0||cfg->capacity.ram<=0||cfg->capacity.gpu<0||
       total.cores<=0||total.ram<=0){ errno=EINVAL; return -1; }
    cores=cfg->capacity.cores;
    ram=cfg->capacity.ram;
    if(cores>total.cores){ cfg->capacity.cores=total.cores; mask|=WORKER_CLAMP_CORES; }
    if(ram>total.ram){ cfg->capacity.ram=total.ram; mask|=WORKER_CLAMP_RAM; }
    // GPU sirf counter hai. Physical detection ke zero GPU field ko dekh
    // ke requested GPU count clamp mat karna.
    if(warn&&cap){
        if(mask==(WORKER_CLAMP_CORES|WORKER_CLAMP_RAM))
            snprintf(warn,cap,"Warning: cores clamped %d -> %d; RAM clamped %lld -> %lld MB",
                     cores,cfg->capacity.cores,ram,cfg->capacity.ram);
        else if(mask==WORKER_CLAMP_CORES)
            snprintf(warn,cap,"Warning: cores clamped %d -> %d",cores,cfg->capacity.cores);
        else if(mask==WORKER_CLAMP_RAM)
            snprintf(warn,cap,"Warning: RAM clamped %lld -> %lld MB",ram,cfg->capacity.ram);
    }
    return mask;
}
