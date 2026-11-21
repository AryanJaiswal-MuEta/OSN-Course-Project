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
#include "worker_discovery.h"
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
    if (events & POLLNVAL)
    {
        worker_loop_stop(loop);
        return;
    }
    if (events & (POLLIN | POLLHUP | POLLERR))
    {
        int r = transport_read(&node->conn_trans, on_scheduler_message, node);
        if (r != TRANSPORT_OK && r != TRANSPORT_WOULD_BLOCK)
        {
            printf("[WORKER] Scheduler disconnected\n");
            worker_loop_stop(&node->w_loop);
            return;
        }
    }
    if (events & POLLOUT)
    {
        int r = transport_flush(&node->conn_trans);
        if (r != TRANSPORT_OK && r != TRANSPORT_WOULD_BLOCK)
        {
            fprintf(stderr, "[WORKER] Send failed: %s\n", transport_error_name(r));
            worker_loop_stop(loop);
            return;
        }
    }
    // update poll mask if pending bytes exist
    short want = POLLIN;
    if (transport_wants_write(&node->conn_trans))
        want |= POLLOUT;
    if (worker_loop_watch(loop, WORKER_CONN, fd, want) < 0)
        worker_loop_stop(loop);
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
    if (worker_capacity_detect(&total_capacity) < 0)
    {
        perror("worker capacity");
        return 1;
    }
    char limit_warn[256];
    if (worker_capacity_limit(&worker.cfg, total_capacity, limit_warn, sizeof(limit_warn)) < 0)
        return 1;
    if (limit_warn[0])
        printf("[WORKER] %s\n", limit_warn);
    struct worker_endpoint ep;
    printf("[WORKER] Discovering scheduler on port %d...\n", worker.cfg.discovery_port);
    if (worker_find_scheduler(&worker.cfg, &ep) != DISCOVERY_OK)
    {
        fprintf(stderr, "[WORKER] Could not discover scheduler and no fallback available\n");
        return 1;
    }
    printf("[WORKER] %s %s:%d\n", ep.used_fallback ? "Using fallback" : "Found scheduler",
           ep.ip, ep.port);
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
    s_addr.sin_port = htons((uint16_t)ep.port);
    inet_pton(AF_INET, ep.ip, &s_addr.sin_addr);
    if (connect(worker.connection_fd, (struct sockaddr *)&s_addr, sizeof(s_addr)) < 0)
    {
        perror("connect to scheduler");
        close(worker.connection_fd);
        return 1;
    }
    // Peer close ho toh send error handle karna hai, process terminate nahi.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR ||
        transport_init(&worker.conn_trans, worker.connection_fd, 1) != TRANSPORT_OK)
    {
        close(worker.connection_fd);
        return 1;
    }

    struct worker_hooks hooks = {
        .io = worker_io_event, .tick = worker_tick_event,
        .output = worker_output_stream, .result = worker_job_result
    };
    if (worker_loop_init(&worker.w_loop, &hooks, &worker) != RUN_OK)
    {
        transport_free(&worker.conn_trans);
        return 1;
    }

    int status = 1;
    // Queue flush POLLOUT pe hoga; partial write pe registration drop nahi hogi.
    if (send_worker_registration(&worker) != TRANSPORT_OK ||
        worker_loop_watch(&worker.w_loop, WORKER_CONN, worker.connection_fd, POLLIN | POLLOUT) < 0)
    {
        fprintf(stderr, "[WORKER] Could not queue registration\n");
        goto done;
    }
    active_worker_loop = &worker.w_loop;
    signal(SIGINT, worker_exit_signal);
    signal(SIGTERM, worker_exit_signal);
    printf("[WORKER] Registration queued (cores=%d ram=%lldMB gpu=%d)\n",
           worker.cfg.capacity.cores, worker.cfg.capacity.ram, worker.cfg.capacity.gpu);
    status = worker_loop_run(&worker.w_loop) < 0;
    active_worker_loop = NULL;
done:
    worker_loop_free(&worker.w_loop);
    transport_free(&worker.conn_trans);
    return status;
}
