/**
 * @file main.c
 * @brief Worker main file: discovery, connects to scheduler, registers capacity and executes jobs
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "discovery.h"
#include "protocol.h"
#include "transport.h"
#include "worker_config.h"
#include "worker_loop.h"
/**holds state for worker instance */
struct worker_node
{
    struct worker_config cfg;
    struct worker_loop w_loop;
    struct transport conn_trans;
    int connection_fd;
};
static struct worker_loop *active_worker_loop = NULL;
static void worker_exit_signal(int sig)
{
    (void)sig;
    if (active_worker_loop)
        worker_loop_stop(active_worker_loop);
}
/** sends broadcast packet to find scheduler ip and tcp port*/
static int locate_scheduler(struct worker_config *cfg, char *out_ip, int *out_port)
{
    int udp_sock = discovery_socket(1, 1); // broadcast enabled
    if (udp_sock < 0)
        return -1;
    struct discovery_packet req;
    req.kind = DISCOVERY_REQUEST;
    req.port = 0;
    req.nonce = (uint64_t)runner_now_ms();
    unsigned char in_buf[64];
    struct sockaddr_in reply_addr;
    socklen_t addr_len = sizeof(reply_addr);
    // try up to 3 times before fallback
    for (int attempt = 0; attempt < 3; attempt++)
    {
        discovery_send(udp_sock, "255.255.255.255", (uint16_t)cfg->discovery_port, &req);
        struct pollfd pfd;
        pfd.fd = udp_sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        // 2 second wait
        if (poll(&pfd, 1, 2000) > 0)
        {
            ssize_t recvd = recvfrom(udp_sock, in_buf, sizeof(in_buf), 0,
                                     (struct sockaddr *)&reply_addr, &addr_len);
            struct discovery_packet rep;
            if (recvd == 16 && discovery_decode(in_buf, 16, &rep) == DISCOVERY_OK)
            {
                if (rep.kind == DISCOVERY_RESPONSE && rep.nonce == req.nonce)
                {
                    inet_ntop(AF_INET, &reply_addr.sin_addr, out_ip, INET_ADDRSTRLEN);
                    *out_port = rep.port;
                    close(udp_sock);
                    return 0;
                }
            }
        }
    }
    close(udp_sock);
    // fallback to passed ip and port if discovery fails
    if (cfg->has_fallback)
    {
        strncpy(out_ip, cfg->scheduler_ip, INET_ADDRSTRLEN - 1);
        *out_port = cfg->scheduler_port;
        return 0;
    }
    return -1;
}
static void put32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}
static void put64(unsigned char *p, uint64_t value)
{
    int i;
    for (i = 7; i >= 0; i--)
    {
        p[i] = (unsigned char)value;
        value >>= 8;
    }
}
/**encode and send worker capacity to scheduler */
static int send_worker_registration(struct worker_node *node)
{
    unsigned char payload_buf[18];
    put32(payload_buf, (uint32_t)node->cfg.capacity.cores);
    put64(payload_buf + 4, (uint64_t)node->cfg.capacity.ram);
    put32(payload_buf + 12, (uint32_t)node->cfg.capacity.gpu);
    // zero length name
    payload_buf[16] = 0;
    payload_buf[17] = 0;
    struct protocol_message reg_msg;
    reg_msg.type = WORKER_REGISTER;
    reg_msg.request_id = 1;
    reg_msg.payload = (const char *)payload_buf;
    reg_msg.payload_len = sizeof(payload_buf);
    return transport_queue(&node->conn_trans, &reg_msg);
}
// message callback when scheduler sends something
static int on_scheduler_message(const struct protocol_message *msg, void *arg)
{
    struct worker_node *node = arg;
    (void)node;
    if (!msg)
        return -1;
    if (msg->type == JOB_ASSIGN)
    {
        // job assigned to this worker
        printf("[WORKER] Received JOB_ASSIGN message\n");
    }
    return 0;
}
/**io callback for worker loop */
static void worker_io_event(struct worker_loop *loop, int fd, short events, void *arg)
{
    struct worker_node *node = arg;
    (void)loop;
    if (events & (POLLIN | POLLHUP | POLLERR))
    {
        int r = transport_read(&node->conn_trans, on_scheduler_message, node);
        if (r == TRANSPORT_CLOSED || r == TRANSPORT_IO)
        {
            printf("[WORKER] Scheduler disconnected\n");
            worker_loop_stop(&node->w_loop);
            return;
        }
    }
    if (events & POLLOUT)
    {
        transport_flush(&node->conn_trans);
    }
    // update poll mask if pending bytes exist
    short want = POLLIN;
    if (transport_wants_write(&node->conn_trans))
        want |= POLLOUT;
    worker_loop_watch(&node->w_loop, WORKER_CONN, fd, want);
}
/**tick callback every 100ms  */
static void worker_tick_event(struct worker_loop *loop, uint64_t now, void *arg)
{
    (void)loop;
    (void)now;
    (void)arg;
    // heartbeat to scheduler every 2 sec
}
/** streaming output of running jobs*/
static int worker_output_stream(int id, uint64_t run_id, const void *buf, size_t len, void *arg)
{
    (void)id;
    (void)run_id;
    (void)buf;
    (void)len;
    (void)arg;
    return 0;
}
/** result callback when a job exits*/
static int worker_job_result(const struct worker_result *res, void *arg)
{
    (void)res;
    (void)arg;
    return 0;
}
int main(int argc, char **argv)
{
    struct worker_node worker;
    memset(&worker, 0, sizeof(worker));
    char parse_err[256];
    if (worker_config_parse(argc, argv, &worker.cfg, parse_err, sizeof(parse_err)) < 0)
    {
        fprintf(stderr, "Worker config error: %s\n", parse_err);
        return 1;
    }
    struct resource total_capacity;
    if (worker_capacity_detect(&total_capacity) == 0)
    {
        char limit_warn[256];
        worker_capacity_limit(&worker.cfg, total_capacity, limit_warn, sizeof(limit_warn));
        if (limit_warn[0])
            printf("[WORKER] %s\n", limit_warn);
    }
    char sched_ip[INET_ADDRSTRLEN];
    int sched_port = 0;
    printf("[WORKER] Discovering scheduler on port %d...\n", worker.cfg.discovery_port);
    if (locate_scheduler(&worker.cfg, sched_ip, &sched_port) < 0)
    {
        fprintf(stderr, "[WORKER] Error: could not discover scheduler and no fallback provided\n");
        return 1;
    }
    printf("[WORKER] Found scheduler at %s:%d\n", sched_ip, sched_port);
    // connect to scheduler over tcp
    worker.connection_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (worker.connection_fd < 0)
    {
        perror("socket");
        return 1;
    }
    struct sockaddr_in s_addr;
    memset(&s_addr, 0, sizeof(s_addr));
    s_addr.sin_family = AF_INET;
    s_addr.sin_port = htons((uint16_t)sched_port);
    inet_pton(AF_INET, sched_ip, &s_addr.sin_addr);
    if (connect(worker.connection_fd, (struct sockaddr *)&s_addr, sizeof(s_addr)) < 0)
    {
        perror("connect to scheduler");
        close(worker.connection_fd);
        return 1;
    }
    // wrap in transport
    transport_init(&worker.conn_trans, worker.connection_fd, 1);
    transport_set_nonblocking(&worker.conn_trans);
    // send capacity registration
    send_worker_registration(&worker);
    transport_flush(&worker.conn_trans);
    // setup worker loop
    struct worker_hooks hooks;
    hooks.io = worker_io_event;
    hooks.tick = worker_tick_event;
    hooks.output = worker_output_stream;
    hooks.result = worker_job_result;
    worker_loop_init(&worker.w_loop, &hooks, &worker);
    worker_loop_watch(&worker.w_loop, WORKER_CONN, worker.connection_fd, POLLIN);
    active_worker_loop = &worker.w_loop;
    signal(SIGINT, worker_exit_signal);
    signal(SIGTERM, worker_exit_signal);
    printf("[WORKER] Registered and running (cores=%d ram=%lldMB gpu=%d)\n",
           worker.cfg.capacity.cores, worker.cfg.capacity.ram, worker.cfg.capacity.gpu);
    worker_loop_run(&worker.w_loop);
    // cleanup
    worker_loop_free(&worker.w_loop);
    transport_free(&worker.conn_trans);
    return 0;
}
