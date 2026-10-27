#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "scheduler.h"

void sched_init(struct scheduler *s)
{
    if(!s) return;
    memset(s,0,sizeof(*s));
    s->next_job=1;
    s->next_worker=1001;
    s->next_run=1;
}

void sched_free(struct scheduler *s)
{
    struct job *curr,*temp;
    struct worker *ptr,*next;
    if(!s) return;

    // next ko free se pehle save karo, warna freed node se link padhna padega.
    curr=s->jobs;
    while(curr){
        temp=curr->next;
        free(curr->deps);
        free(curr->checkpoint);
        free(curr);
        curr=temp;
    }
    ptr=s->workers;
    while(ptr){
        next=ptr->next;
        free(ptr);
        ptr=next;
    }
    sched_init(s);
}

int user_valid(const char *name)
{
    size_t i;
    unsigned char c;
    if(!name||!name[0]) return 0;
    for(i=0;i<USER_LEN;i++){
        c=(unsigned char)name[i];
        if(!c) return 1;
        // ASCII check rakha hai; locale ke chakkar mein extra letters na aa jayein.
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
             (c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return 0;
    }
    return 0;
}

struct worker *worker_find(struct scheduler *s,int id)
{
    struct worker *curr;
    if(!s) return NULL;
    for(curr=s->workers;curr;curr=curr->next)
        if(curr->id==id) return curr;
    return NULL;
}

struct job *job_find(struct scheduler *s,int id)
{
    struct job *curr;
    if(!s) return NULL;
    for(curr=s->jobs;curr;curr=curr->next)
        if(curr->id==id) return curr;
    return NULL;
}

int worker_add(struct scheduler *s,struct resource total,uint64_t now,int *id)
{
    struct worker *ptr;
    if(!s||!id||total.cores<=0||total.ram<=0||total.gpu<0) return S_BAD_INPUT;
    if(s->next_worker==INT_MAX) return S_ID_LIMIT;
    ptr=calloc(1,sizeof(*ptr));
    if(!ptr) return S_NO_MEMORY;
    ptr->id=s->next_worker++;
    ptr->state=ALIVE;
    ptr->total=total;
    ptr->heartbeat_ms=now;

    // Tail rakhi hai taaki har insert pe poori list walk na karni pade.
    // Lookup abhi simple linear scan hai; koi hash table nahi chahiye.
    if(s->worker_tail) s->worker_tail->next=ptr;
    else s->workers=ptr;
    s->worker_tail=ptr;
    s->worker_count++;
    *id=ptr->id;
    return S_OK;
}

int job_add(struct scheduler *s,const struct job_req *req,uint64_t now,int *id)
{
    struct job *ptr;
    if(!s||!req||!id) return S_BAD_INPUT;
    // Fixed arrays ka NUL pehle check karo; unchecked strlen yahan unsafe hoga.
    if(!memchr(req->user,'\0',USER_LEN)||!memchr(req->path,'\0',PATH_LEN))
        return S_BAD_INPUT;
    if(!user_valid(req->user)||!req->path[0]||req->need.cores<=0||
       req->need.ram<=0||req->need.gpu<0||req->time_limit<=0||
       req->prio<1||req->prio>3||req->group<0||
       (req->preemptible!=0&&req->preemptible!=1)) return S_BAD_INPUT;
    if(s->pending>=QUEUE_CAP) return S_QUEUE_FULL;
    if(s->next_job==INT_MAX) return S_ID_LIMIT;

    ptr=calloc(1,sizeof(*ptr));
    if(!ptr) return S_NO_MEMORY;
    ptr->id=s->next_job++;
    ptr->req=*req;
    ptr->state=QUEUED;
    ptr->submit_ms=now;
    ptr->queue_ms=now;
    if(s->job_tail) s->job_tail->next=ptr;
    else s->jobs=ptr;
    s->job_tail=ptr;
    s->job_count++;
    s->pending++;
    *id=ptr->id;
    return S_OK;
}
