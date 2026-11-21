#include <stdlib.h>
#include <string.h>
#include "scheduler.h"

int sched_cached(const struct worker *w,const char *key)
{
    const struct cache_entry *curr;
    if(!key||!key[0]) return 0;
    for(curr=w->cache;curr;curr=curr->next) if(!strcmp(curr->key,key)) return 1;
    return 0;
}

int sched_cache_set(struct scheduler *s,int worker_id,const char *key,int present)
{
    struct worker *w=worker_find(s,worker_id);
    struct cache_entry **link,*ptr;
    if(!w) return S_NOT_FOUND;
    if(!key||!key[0]||strnlen(key,CONTENT_LEN)>=CONTENT_LEN||
       (present!=0&&present!=1)) return S_BAD_INPUT;
    // Content identity Member 3 deta hai; yahan string compare hai, hash function nahi.
    for(link=&w->cache;*link;link=&(*link)->next){
        if(strcmp((*link)->key,key)) continue;
        if(!present){ ptr=*link; *link=ptr->next; free(ptr); }
        return S_OK;
    }
    if(!present) return S_OK;
    ptr=calloc(1,sizeof(*ptr));
    if(!ptr) return S_NO_MEMORY;
    strcpy(ptr->key,key); ptr->next=w->cache; w->cache=ptr;
    return S_OK;
}

int sched_content_set(struct scheduler *s,int id,const char *key)
{
    struct job *j=job_find(s,id);
    if(!j) return S_NOT_FOUND;
    if(!key||strnlen(key,CONTENT_LEN)>=CONTENT_LEN) return S_BAD_INPUT;
    if((j->state!=QUEUED&&j->state!=WAITING)||j->dispatch_pending) return S_BAD_STATE;
    strcpy(j->content,key);
    return S_OK;
}

static int better(const struct worker *w,const struct worker *best,const struct job *j)
{
    int idle=!(w->jobs+w->reserved_jobs),old_idle=!(best->jobs+best->reserved_jobs);
    int hit=sched_cached(w,j->content),old_hit=sched_cached(best,j->content);
    if(idle!=old_idle) return idle>old_idle;
    if(hit!=old_hit) return hit>old_hit;
    if(w->load!=best->load) return w->load<best->load;
    if(w->jobs+w->reserved_jobs!=best->jobs+best->reserved_jobs)
        return w->jobs+w->reserved_jobs<best->jobs+best->reserved_jobs;
    return w->id<best->id;
}

const struct worker *sched_fit(const struct scheduler *s,const struct job *j)
{
    const struct worker *w,*best=NULL;
    for(w=s->workers;w;w=w->next){
        if(w->dispatch_blocked||!worker_fits(w,j->req.need)||(w->preempt_for&&w->preempt_for!=j->id)) continue;
        if(j->preempt_worker&&w->id!=j->preempt_worker) continue;
        if(best&&j->migrated_from){
            if(w->id==j->migrated_from&&best->id!=j->migrated_from) continue;
            if(best->id==j->migrated_from&&w->id!=j->migrated_from){ best=w; continue; }
        }
        if(!best||better(w,best,j)) best=w;
    }
    return best;
}

int sched_worker_writable(struct scheduler *s,int worker_id,int writable)
{
    struct worker *w=worker_find(s,worker_id);
    if(!w) return S_NOT_FOUND;
    if(writable!=0&&writable!=1) return S_BAD_INPUT;
    if(w->state==DEAD) return S_BAD_STATE;
    // Network queue busy hai toh selection pause karo; resources/retry budget
    // ko touch nahi karna. Flush ke baad same worker phir eligible ho jayega.
    w->dispatch_blocked=!writable;
    return S_OK;
}
