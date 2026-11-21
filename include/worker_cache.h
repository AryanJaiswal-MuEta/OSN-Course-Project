#ifndef OSN_WORKER_CACHE_H
#define OSN_WORKER_CACHE_H
#include "model.h"

struct worker_cache_entry {
    char key[CONTENT_LEN];
    uint64_t size,used;
    unsigned int pins;
    int reported;
    struct worker_cache_entry *next;
};
struct worker_cache {
    int fd,lock_fd;
    char dir[PATH_LEN];
    uint64_t size,limit,clock;
    struct worker_cache_entry *head;
    void (*changed)(const char *key,int present,void *arg);
    void *arg;
};
struct cache_transfer {
    int fd;
    char temp[80],key[CONTENT_LEN];
    uint64_t size,received;
};
int worker_cache_init(struct worker_cache *c,const char *dir,uint64_t limit);
void worker_cache_free(struct worker_cache *c);
int worker_cache_pin(struct worker_cache *c,const char *key,char *path,size_t cap);
void worker_cache_unpin(struct worker_cache *c,const char *key);
void worker_cache_touch(struct worker_cache *c,const char *key);
int worker_cache_evict(struct worker_cache *c,const char *protect);
int worker_cache_begin(struct worker_cache *c,struct cache_transfer *t,const char *key,
                       uint64_t size,int id,uint64_t run);
int worker_cache_write(struct cache_transfer *t,uint64_t off,const void *buf,size_t size);
int worker_cache_end(struct worker_cache *c,struct cache_transfer *t,char *path,size_t cap);
void worker_cache_abort(struct worker_cache *c,struct cache_transfer *t);
#endif
