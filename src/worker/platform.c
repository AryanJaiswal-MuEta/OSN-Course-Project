#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#include "internal.h"

void run_sample(struct run_job *j)
{
#ifdef __APPLE__
    struct proc_taskinfo info;
    if(proc_pidinfo(j->pid,PROC_PIDTASKINFO,0,&info,sizeof(info))!=(int)sizeof(info)) return;
    j->usage.ram=(long long)((info.pti_resident_size+1048575)/1048576);
    j->usage.faults=info.pti_faults;
    j->usage.cpu_time=(double)(info.pti_total_user+info.pti_total_system)/1000000000.0;
#elif defined(__linux__)
    char path[80],buf[4096],*ptr,*save,*token;
    FILE *fp;
    uint64_t minor=0,major=0,user=0,system=0;
    long long pages=0;
    long ticks,page_size;
    int field;
    snprintf(path,sizeof(path),"/proc/%ld/stat",(long)j->pid);
    fp=fopen(path,"r");
    if(!fp) return;
    ptr=fgets(buf,sizeof(buf),fp);
    fclose(fp);
    if(!ptr) return;
    // comm field mein spaces aur ')' ho sakte hain; last ')' ke baad state
    // field se parse karo, warna saare column numbers shift ho jayenge.
    ptr=strrchr(buf,')');
    if(!ptr||ptr[1]!=' ') return;
    token=strtok_r(ptr+2," ",&save);
    for(field=3;token&&field<=24;field++,token=strtok_r(NULL," ",&save)){
        if(field==10) minor=strtoull(token,NULL,10);
        if(field==12) major=strtoull(token,NULL,10);
        if(field==14) user=strtoull(token,NULL,10);
        if(field==15) system=strtoull(token,NULL,10);
        if(field==24) pages=strtoll(token,NULL,10);
    }
    ticks=sysconf(_SC_CLK_TCK);
    page_size=sysconf(_SC_PAGESIZE);
    if(ticks>0) j->usage.cpu_time=((double)user+(double)system)/(double)ticks;
    j->usage.faults=minor+major;
    if(pages>=0&&page_size>0)
        j->usage.ram=(long long)(((long double)pages*page_size+1048575)/1048576);
#else
    (void)j;
#endif
    if(j->usage.ram>j->usage.peak_ram) j->usage.peak_ram=j->usage.ram;
}
