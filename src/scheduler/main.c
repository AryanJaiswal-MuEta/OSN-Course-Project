/**
 * @file main.c
 * @brief scheduler main: sets up tcp listener and udp discovery and runs the event loop
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
#include "scheduler_loop.h"
#include "scheduler_service.h"
#include "transport.h"

#define MAX_PEERS 256

/** connection tracking for each socket */
struct peer_client
{
    int fd;
    int is_worker;
    int worker_id;
    struct transport trans;
    struct server_context*ctx;
};

/** scheduler context with service and loop */
struct server_context
{
    struct scheduler sched;
    struct sched_service service;
    struct sched_loop ev_loop;
    int tcp_port;
    int listen_fd;
    int udp_fd;
    struct peer_client peers[MAX_PEERS];
    int peer_count;
};

static struct sched_loop *active_loop = NULL;

static void clean_exit_handler(int sig)
{
    (void)sig;
    if (active_loop)
        sched_loop_stop(active_loop);
}

// read big endian integers matching discovery.c style
static uint32_t get32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t get64(const unsigned char *p)
{
    uint64_t value = 0;
    int i;
    for (i = 0; i < 8; i++)
        value = (value << 8) | p[i];
    return value;
}

/** handle message arrived over tcp */
static int handle_client_message(const struct protocol_message *msg, void *arg)
{
    struct peer_client *client = arg;

    if (!msg || !client)
        return -1;

    // worker registration packet
    if (msg->type == WORKER_REGISTER)
    {
        if (msg->payload_len < 16)
            return -1;

        const unsigned char *p = (const unsigned char *)msg->payload;
        struct resource res;
        res.cores = (int)get32(p);
        res.ram = (long long)get64(p + 4);
        res.gpu = (int)get32(p + 12);

        struct server_context *ctx = client->ctx;
        (void)ctx;
        printf("[SCHEDULER] Registration request from fd %d: cores=%d ram=%lldMB gpu=%d\n",
               client->fd, res.cores, res.ram, res.gpu);

        client->is_worker = 1;
    }
    return 0;
}

/** accept incoming client or worker */
static void accept_new_client(struct server_context *ctx)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(ctx->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0)
        return;

    for (int i = 0; i < MAX_PEERS; i++)
    {
        if (ctx->peers[i].fd < 0)
        {
            ctx->peers[i].fd = client_fd;
            ctx->peers[i].is_worker = 0;
            ctx->peers[i].worker_id = 0;
            ctx->peers[i].ctx = ctx;  

            // init transport and set non blocking
            transport_init(&ctx->peers[i].trans, client_fd, 0);
            transport_set_nonblocking(&ctx->peers[i].trans);
            sched_loop_add(&ctx->ev_loop, client_fd, POLLIN);
            ctx->peer_count++;
            printf("[SCHEDULER] New connection accepted on fd %d\n", client_fd);
            return;
        }
    }
    // no free peer slot available
    close(client_fd);
}

/** io event callback for poll loop */
static void on_io_event(struct sched_loop *loop, int fd, short events, void *arg)
{
    struct server_context *ctx = arg;
    (void)loop;

    // new incoming connection on tcp
    if (fd == ctx->listen_fd)
    {
        accept_new_client(ctx);
        return;
    }

    // udp broadcast packet arrived
    if (fd == ctx->udp_fd)
    {
        unsigned char packet_buffer[DISCOVERY_MAX_PACKET];
        struct sockaddr_in sender_addr;
        socklen_t slen = sizeof(sender_addr);
        ssize_t received_bytes = recvfrom(ctx->udp_fd, packet_buffer, sizeof(packet_buffer), 0,
                                          (struct sockaddr *)&sender_addr, &slen);
        if (received_bytes == 16)
        {
            struct discovery_packet incoming, reply;
            if (discovery_decode(packet_buffer, (size_t)received_bytes, &incoming) == DISCOVERY_OK)
            {
                if (incoming.kind == DISCOVERY_REQUEST)
                {
                    reply.kind = DISCOVERY_RESPONSE;
                    reply.port = (uint16_t)ctx->tcp_port;
                    reply.nonce = incoming.nonce;
                    char sender_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, sizeof(sender_ip));
                    discovery_send(ctx->udp_fd, sender_ip, ntohs(sender_addr.sin_port), &reply);
                }
            }
        }
        return;
    }

    // regular data on connected socket
    for (int i = 0; i < MAX_PEERS; i++)
    {
        if (ctx->peers[i].fd != fd)
            continue;

        if (events & (POLLIN | POLLHUP | POLLERR))
        {
            int ret = transport_read(&ctx->peers[i].trans, handle_client_message, &ctx->peers[i]);
            if (ret == TRANSPORT_CLOSED || ret == TRANSPORT_IO || ret == TRANSPORT_PROTOCOL)
            {
                sched_loop_remove(&ctx->ev_loop, fd);
                transport_free(&ctx->peers[i].trans);
                close(fd);
                ctx->peers[i].fd = -1;
                ctx->peer_count--;
                printf("[SCHEDULER] Connection closed on fd %d\n", fd);
                return;
            }
        }

        if (ctx->peers[i].fd >= 0 && (events & POLLOUT))
        {
            transport_flush(&ctx->peers[i].trans);
        }

        if (ctx->peers[i].fd >= 0)
        {
            short wanted = POLLIN;
            if (transport_wants_write(&ctx->peers[i].trans))
                wanted |= POLLOUT;
            sched_loop_update(&ctx->ev_loop, fd, wanted);
        }
        return;
    }
}

/** periodic 1 second tick */
static void on_tick_event(struct sched_loop *loop, uint64_t now, void *arg)
{
    (void)loop;
    (void)now;
    (void)arg;
    // heartbeat deadline checking
}

/** schedule jobs if queue is ready */
static void on_schedule_event(struct sched_loop *loop, uint64_t now, void *arg)
{
    (void)loop;
    (void)now;
    (void)arg;
    // fifo or priority logic to pick job and dispatch
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s <port> <FIFO|PRIORITY> [-discovery-port <port>]\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "Invalid port number\n");
        return 1;
    }

    if (strcmp(argv[2], "FIFO") != 0 && strcmp(argv[2], "PRIORITY") != 0)
    {
        fprintf(stderr, "Algorithm must be FIFO or PRIORITY\n");
        return 1;
    }

    int disc_port = 9999;
    for (int i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "-discovery-port") == 0 && i + 1 < argc)
        {
            disc_port = atoi(argv[++i]);
        }
    }

    struct server_context server;
    memset(&server, 0, sizeof(server));
    server.tcp_port = port;
    for (int i = 0; i < MAX_PEERS; i++)
        server.peers[i].fd = -1;

    sched_init(&server.sched);
    sched_service_init(&server.service, &server.sched, NULL, NULL, &server);

    // create tcp listen socket
    server.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server.listen_fd < 0)
    {
        perror("socket");
        return 1;
    }
    int opt = 1;
    setsockopt(server.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)port);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server.listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("bind tcp");
        return 1;
    }
    if (listen(server.listen_fd, 64) < 0)
    {
        perror("listen");
        return 1;
    }

    // create udp socket for discovery
    server.udp_fd = discovery_socket(1, 0);
    if (server.udp_fd < 0)
    {
        perror("udp discovery socket");
        return 1;
    }
    struct sockaddr_in disc_addr;
    memset(&disc_addr, 0, sizeof(disc_addr));
    disc_addr.sin_family = AF_INET;
    disc_addr.sin_port = htons((uint16_t)disc_port);
    disc_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server.udp_fd, (struct sockaddr *)&disc_addr, sizeof(disc_addr)) < 0)
    {
        perror("bind udp discovery");
        return 1;
    }

    // setting up the poll loop
    struct sched_hooks hooks = {on_io_event, on_tick_event, on_schedule_event};
    sched_loop_init(&server.ev_loop, &server.sched, &hooks, &server);
    sched_loop_add(&server.ev_loop, server.listen_fd, POLLIN);
    sched_loop_add(&server.ev_loop, server.udp_fd, POLLIN);

    active_loop = &server.ev_loop;
    signal(SIGINT, clean_exit_handler);
    signal(SIGTERM, clean_exit_handler);

    printf("[SCHEDULER] Running on TCP port %d, UDP discovery port %d, algorithm %s\n",
           port, disc_port, argv[2]);

    sched_loop_run(&server.ev_loop);

    // free everything on exit
    for (int i = 0; i < MAX_PEERS; i++)
    {
        if (server.peers[i].fd >= 0)
        {
            transport_free(&server.peers[i].trans);
            close(server.peers[i].fd);
        }
    }
    close(server.listen_fd);
    close(server.udp_fd);
    sched_loop_free(&server.ev_loop);
    sched_free(&server.sched);

    return 0;
}
