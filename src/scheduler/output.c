#include <stdlib.h>
#include <string.h>
#include "scheduler.h"

int sched_output(struct scheduler *s,int worker_id,int id,uint64_t run,
                  uint64_t offset,const void *buf,size_t size)
{
    struct job *j=job_find(s,id);
    size_t drop;
    if(!j) return S_NOT_FOUND;
    if(!run||j->worker_id!=worker_id||j->run_id!=run) return S_STALE_RUN;
    if(j->state!=RUNNING&&!j->dispatch_pending) return S_BAD_STATE;
    if((!buf&&size)||size>16384||offset!=j->output_received||size>UINT64_MAX-offset) return S_BAD_INPUT;
    if(!size) return S_OK;
    if(!j->output){ j->output=malloc(65536); if(!j->output) return S_NO_MEMORY; }
    // Sirf latest 64 KiB rakho; total output ki wajah se scheduler RAM grow nahi hogi.
    if(j->output_size+size>65536){
        drop=j->output_size+size-65536;
        memmove(j->output,j->output+drop,j->output_size-drop); j->output_size-=drop;
    }
    memcpy(j->output+j->output_size,buf,size); j->output_size+=size;
    j->output_received+=size;
    return S_OK;
}
