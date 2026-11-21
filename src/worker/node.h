#ifndef OSN_WORKER_NODE_H
#define OSN_WORKER_NODE_H
#include "worker_cache.h"
#include "worker_discovery.h"
#include "worker_loop.h"
#include "transport.h"

struct worker_task {
    int id,assigned,pinned,stop_intent,final_queued,restore_seen,cache_wait,files_removed;
    uint64_t run,created,cp_seq,cp_ms,out_offset;
    char key[CONTENT_LEN],path[PATH_LEN];
    unsigned char *restore,*last_state;
    size_t restore_size,last_size;
    int have_state;
    struct cache_transfer transfer;
    struct worker_task *next;
};
struct worker_node {
    struct worker_config cfg;
    struct worker_loop loop;
    struct worker_cache cache;
    struct transport trans;
    struct worker_task *tasks;
    int fd,worker_id,lost,draining,drain_sent;
    uint64_t heartbeat,registered_ms;
    char admin[32];
    size_t admin_len;
};
uint32_t worker_u32(const unsigned char *p);
uint64_t worker_u64(const unsigned char *p);
void worker_p32(unsigned char *p,uint32_t n);
void worker_p64(unsigned char *p,uint64_t n);
int worker_send(struct worker_node *n,int type,const void *buf,size_t size);
int worker_message(const struct protocol_message *msg,void *arg);
int worker_started(const struct worker_start *start,void *arg);
int worker_output(int id,uint64_t run,const void *buf,size_t size,void *arg);
int worker_result(const struct worker_result *result,void *arg);
void worker_tick(struct worker_loop *loop,uint64_t now,void *arg);
void worker_tasks_free(struct worker_node *n);
void worker_cache_changed(const char *key,int present,void *arg);
#endif
