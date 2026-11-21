#ifndef OSN_SCHED_SERVER_H
#define OSN_SCHED_SERVER_H
#include "scheduler_loop.h"
#include "scheduler_service.h"
#include "transport.h"

#define MAX_PEERS 256
struct server;
struct peer {
    int fd,worker_id,failed;
    char user[USER_LEN];
    uint64_t connected_ms;
    struct transport trans;
    struct server *ctx;
};
struct server {
    struct scheduler sched;
    struct sched_service svc;
    struct sched_loop loop;
    enum sched_policy policy;
    int port,listen_fd,udp_fd;
    struct peer peers[MAX_PEERS];
};

uint32_t wire32(const unsigned char *p);
uint64_t wire64(const unsigned char *p);
void pack32(unsigned char *p,uint32_t n);
void pack64(unsigned char *p,uint64_t n);
int server_queue(struct peer *p,int type,uint64_t request,const void *buf,size_t size);
int server_message(const struct protocol_message *msg,void *arg);
int server_assign(struct peer *p,struct job *j);
void server_stops(struct server *ctx);
void server_close(struct peer *p);
void server_log(const char *msg,void *arg);
#endif
