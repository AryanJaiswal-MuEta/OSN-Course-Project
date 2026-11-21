/**
 * @file main.c
 * @brief Interactive client CLI to submit jobs, check status and talk to scheduler
 */

#include <arpa/inet.h>
#include <ctype.h>
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

#define DEFAULT_DISCOVERY_PORT 9999
#define MAX_CMD_LEN 1024
#define USERNAME_LEN 32

/** holds client state, socket fd, buffers and logged in username */
struct client_app
{
    int server_fd;
    struct transport trans;
    char username[USERNAME_LEN];
    int logged_in;
    int follow_mode;
    int follow_job_id;
    uint32_t next_req_id;
    volatile sig_atomic_t running;
};

static struct client_app *global_app = NULL;

/** handle ctrl+c so if following logs it only stops following, not quit whole client */
static void sigint_handler(int sig)
{
    (void)sig;
    if (global_app && global_app->follow_mode)
    {
        global_app->follow_mode = 0;
        printf("\n[Detached from log stream]\nclient> ");
        fflush(stdout);
        return;
    }
    if (global_app)
        global_app->running = 0;
}

/** put16/put32/put64 helps in encoding integers in big endian order */
static void put16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
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
    for (int i = 7; i >= 0; i--)
    {
        p[i] = (unsigned char)value;
        value >>= 8;
    }
}

/** get16/get32 helps in decoding integers from big endian order */
static uint16_t get16(const void *ptr)
{
    const unsigned char *p = (const unsigned char *)ptr;
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t get32(const void *ptr)
{
    const unsigned char *p = (const unsigned char *)ptr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/** check if username is valid: 1 to 31 chars with letters, numbers, dot, dash, underscore */
static int validate_username(const char *name)
{
    if (!name || !name[0])
        return 0;
    size_t len = strlen(name);
    if (len >= USERNAME_LEN)
        return 0;
    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.')
            return 0;
    }
    return 1;
}

/** send udp broadcast on discovery port to find scheduler ip and tcp port */
static int discover_scheduler(int discovery_port, char *out_ip, int *out_port)
{
    int udp_sock = discovery_socket(1, 1);
    if (udp_sock < 0)
        return -1;

    struct discovery_packet req;
    req.kind = DISCOVERY_REQUEST;
    req.port = 0;
    req.nonce = 0xABCD1234;

    unsigned char in_buf[64];
    struct sockaddr_in reply_addr;
    socklen_t addr_len = sizeof(reply_addr);

    printf("[CLIENT] Searching for scheduler on UDP port %d...\n", discovery_port);
    for (int attempt = 0; attempt < 3; attempt++)
    {
        discovery_send(udp_sock, "255.255.255.255", (uint16_t)discovery_port, &req);

        struct pollfd pfd;
        pfd.fd = udp_sock;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, 1500) > 0)
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
    return -1;
}

/** connect to scheduler over tcp socket */
static int connect_scheduler(const char *ip, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &saddr.sin_addr) <= 0)
    {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        close(sock);
        return -1;
    }

    return sock;
}

/** parse incoming packets from scheduler and print output */
static int handle_server_packet(const struct protocol_message *msg, void *arg)
{
    struct client_app *app = arg;
    if (!msg)
        return -1;

    switch (msg->type)
    {
    case SCHED_REPLY:
    {
        // scheduler reply packet: contains err code, id, and text
        if (msg->payload_len >= 10)
        {
            const unsigned char *p = (const unsigned char *)msg->payload;
            uint32_t err = get32(p);
            uint32_t id = get32(p + 4);
            uint16_t tlen = get16(p + 8);
            const char *text = (const char *)(p + 10);
            if (err != 0)
            {
                printf("[ERROR] %.*s\n", (int)tlen, text);
            }
            else
            {
                if (id > 0)
                    printf("[Job ID: %u] %.*s\n", id, (int)tlen, text);
                else
                    printf("%.*s\n", (int)tlen, text);
            }
        }
        break;
    }
    case JOB_OUTPUT:
    {
        // streaming log output from running job
        if (msg->payload_len > 8)
        {
            const char *out = (const char *)msg->payload + 8;
            size_t out_len = msg->payload_len - 8;
            fwrite(out, 1, out_len, stdout);
            fflush(stdout);
        }
        return 0;
    }
    case JOB_COMPLETE:
    case JOB_FAILED:
    {
        // async event notification from cluster
        if (msg->payload_len > 0)
            printf("\n[Event] %.*s\n", (int)msg->payload_len, (const char *)msg->payload);
        break;
    }
    default:
        break;
    }

    if (!app->follow_mode)
    {
        printf("client> ");
        fflush(stdout);
    }
    return 0;
}

/** parse submit flags and send job request to scheduler */
static void execute_submit(struct client_app *app, char *args)
{
    char path[256] = {0};
    int cores = 1;
    long long ram = 256;
    int time_limit = 60;
    int prio = 2;
    int gpu = 0;

    char *token = strtok(args, " \t\r\n");
    if (!token)
    {
        printf("Usage: submit <path> -cores <n> -ram <MB> -time <s> -prio <1-3> [-gpu <n>]\n");
        return;
    }
    strncpy(path, token, sizeof(path) - 1);

    while ((token = strtok(NULL, " \t\r\n")) != NULL)
    {
        if (strcmp(token, "-cores") == 0)
        {
            token = strtok(NULL, " \t\r\n");
            if (token)
                cores = atoi(token);
        }
        else if (strcmp(token, "-ram") == 0)
        {
            token = strtok(NULL, " \t\r\n");
            if (token)
                ram = atoll(token);
        }
        else if (strcmp(token, "-time") == 0)
        {
            token = strtok(NULL, " \t\r\n");
            if (token)
                time_limit = atoi(token);
        }
        else if (strcmp(token, "-prio") == 0)
        {
            token = strtok(NULL, " \t\r\n");
            if (token)
                prio = atoi(token);
        }
        else if (strcmp(token, "-gpu") == 0)
        {
            token = strtok(NULL, " \t\r\n");
            if (token)
                gpu = atoi(token);
        }
    }

    // pack payload matching scheduler format: u16 user + u16 path + 26 bytes resources
    size_t ulen = strlen(app->username);
    size_t plen = strlen(path);
    size_t payload_len = 2 + ulen + 2 + plen + 26;
    unsigned char *payload = malloc(payload_len);
    if (!payload)
    {
        printf("Out of memory\n");
        return;
    }

    unsigned char *p = payload;
    put16(p, (uint16_t)ulen);
    p += 2;
    memcpy(p, app->username, ulen);
    p += ulen;

    put16(p, (uint16_t)plen);
    p += 2;
    memcpy(p, path, plen);
    p += plen;

    put32(p, (uint32_t)cores);
    p += 4;
    put64(p, (uint64_t)ram);
    p += 8;
    put32(p, (uint32_t)gpu);
    p += 4;
    put32(p, (uint32_t)time_limit);
    p += 4;
    *p++ = (unsigned char)prio;
    put32(p, (uint32_t)0); // group default 0
    p += 4;
    *p++ = (unsigned char)0; // preemptible default 0

    struct protocol_message pmsg;
    memset(&pmsg, 0, sizeof(pmsg));
    pmsg.type = JOB_SUBMIT;
    pmsg.request_id = ++app->next_req_id;
    pmsg.payload = (const void *)payload;
    pmsg.payload_len = payload_len;

    transport_queue(&app->trans, &pmsg);
    transport_flush(&app->trans);
    free(payload);
}

/** send status query packet for job id */
static void execute_status(struct client_app *app, char *args)
{
    char *token = strtok(args, " \t\r\n");
    if (!token)
    {
        printf("Usage: status <job_id>\n");
        return;
    }
    uint32_t jid = (uint32_t)atoi(token);
    unsigned char payload[4];
    put32(payload, jid);

    struct protocol_message pmsg;
    memset(&pmsg, 0, sizeof(pmsg));
    pmsg.type = STATUS_QUERY;
    pmsg.request_id = ++app->next_req_id;
    pmsg.payload = (const void *)payload;
    pmsg.payload_len = sizeof(payload);

    transport_queue(&app->trans, &pmsg);
    transport_flush(&app->trans);
}

/** send nodes query packet to get worker list */
static void execute_nodes(struct client_app *app)
{
    struct protocol_message pmsg;
    memset(&pmsg, 0, sizeof(pmsg));
    pmsg.type = NODES_QUERY;
    pmsg.request_id = ++app->next_req_id;
    pmsg.payload = NULL;
    pmsg.payload_len = 0;

    transport_queue(&app->trans, &pmsg);
    transport_flush(&app->trans);
}

/** send logs query packet: logs <job_id> [-f] */
static void execute_logs(struct client_app *app, char *args)
{
    char *token = strtok(args, " \t\r\n");
    if (!token)
    {
        printf("Usage: logs <job_id> [-f]\n");
        return;
    }
    uint32_t jid = (uint32_t)atoi(token);
    int follow = 0;
    token = strtok(NULL, " \t\r\n");
    if (token && strcmp(token, "-f") == 0)
    {
        follow = 1;
        app->follow_mode = 1;
        app->follow_job_id = (int)jid;
        printf("[Streaming logs for Job %u (Ctrl+C to detach)]\n", jid);
    }

    unsigned char payload[5];
    put32(payload, jid);
    payload[4] = (unsigned char)follow;

    struct protocol_message pmsg;
    memset(&pmsg, 0, sizeof(pmsg));
    pmsg.type = JOB_OUTPUT;
    pmsg.request_id = ++app->next_req_id;
    pmsg.payload = (const void *)payload;
    pmsg.payload_len = sizeof(payload);

    transport_queue(&app->trans, &pmsg);
    transport_flush(&app->trans);
}

/** send cancel packet for job id */
static void execute_cancel(struct client_app *app, char *args)
{
    char *token = strtok(args, " \t\r\n");
    if (!token)
    {
        printf("Usage: cancel <job_id>\n");
        return;
    }
    uint32_t jid = (uint32_t)atoi(token);
    unsigned char payload[4];
    put32(payload, jid);

    struct protocol_message pmsg;
    memset(&pmsg, 0, sizeof(pmsg));
    pmsg.type = JOB_CANCEL;
    pmsg.request_id = ++app->next_req_id;
    pmsg.payload = (const void *)payload;
    pmsg.payload_len = sizeof(payload);

    transport_queue(&app->trans, &pmsg);
    transport_flush(&app->trans);
}

/** send migrate packet for job id */
static void execute_migrate(struct client_app *app, char *args)
{
    char *token = strtok(args, " \t\r\n");
    if (!token)
    {
        printf("Usage: migrate <job_id>\n");
        return;
    }
    uint32_t jid = (uint32_t)atoi(token);
    unsigned char payload[4];
    put32(payload, jid);

    struct protocol_message pmsg;
    memset(&pmsg, 0, sizeof(pmsg));
    pmsg.type = JOB_MIGRATE;
    pmsg.request_id = ++app->next_req_id;
    pmsg.payload = (const void *)payload;
    pmsg.payload_len = sizeof(payload);

    transport_queue(&app->trans, &pmsg);
    transport_flush(&app->trans);
}

/** print help menu with available commands */
static void print_help(void)
{
    printf("Commands available:\n"
           "  submit <path> -cores <n> -ram <MB> -time <s> -prio <1-3> [-gpu <n>]\n"
           "  nodes\n"
           "  status <job_id>\n"
           "  logs <job_id> [-f]\n"
           "  cancel <job_id>\n"
           "  migrate <job_id>\n"
           "  help\n"
           "  exit / quit\n");
}

/** parse user typed line and call corresponding command */
static void process_cli_command(struct client_app *app, char *line)
{
    while (*line && isspace((unsigned char)*line))
        line++;
    if (!*line)
    {
        printf("client> ");
        fflush(stdout);
        return;
    }

    char *cmd = strtok(line, " \t\r\n");
    char *rest = strtok(NULL, "");

    if (strcmp(cmd, "nodes") == 0)
    {
        execute_nodes(app);
    }
    else if (strcmp(cmd, "submit") == 0)
    {
        execute_submit(app, rest);
    }
    else if (strcmp(cmd, "status") == 0)
    {
        execute_status(app, rest);
    }
    else if (strcmp(cmd, "logs") == 0)
    {
        execute_logs(app, rest);
    }
    else if (strcmp(cmd, "cancel") == 0)
    {
        execute_cancel(app, rest);
    }
    else if (strcmp(cmd, "migrate") == 0)
    {
        execute_migrate(app, rest);
    }
    else if (strcmp(cmd, "help") == 0)
    {
        print_help();
        printf("client> ");
        fflush(stdout);
    }
    else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0)
    {
        app->running = 0;
    }
    else
    {
        printf("Unknown command: %s. Type 'help' for usage.\nclient> ", cmd);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    char scheduler_ip[INET_ADDRSTRLEN] = "127.0.0.1";
    int scheduler_port = 0;
    int discovery_port = DEFAULT_DISCOVERY_PORT;
    char username_input[USERNAME_LEN] = {0};

    // check command line flags for username, discovery port, ip and port
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-user") == 0 && i + 1 < argc)
        {
            strncpy(username_input, argv[++i], sizeof(username_input) - 1);
        }
        else if (strcmp(argv[i], "-discovery-port") == 0 && i + 1 < argc)
        {
            discovery_port = atoi(argv[++i]);
        }
        else if (argv[i][0] != '-')
        {
            strncpy(scheduler_ip, argv[i], sizeof(scheduler_ip) - 1);
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                scheduler_port = atoi(argv[++i]);
            }
        }
    }

    // if username not given on command line, prompt user
    while (!validate_username(username_input))
    {
        printf("Username: ");
        fflush(stdout);
        if (!fgets(username_input, sizeof(username_input), stdin))
            return 1;

        size_t len = strlen(username_input);
        if (len > 0 && username_input[len - 1] == '\n')
            username_input[len - 1] = '\0';
        if (!validate_username(username_input))
        {
            printf("Invalid username. 1-31 alphanumeric, '.', '-' and '_' characters allowed.\n");
        }
    }

    // if scheduler port not given, use udp discovery to find it
    if (scheduler_port == 0)
    {
        if (discover_scheduler(discovery_port, scheduler_ip, &scheduler_port) != 0)
        {
            fprintf(stderr, "[CLIENT] Discovery failed. Defaulting to 127.0.0.1:8080\n");
            scheduler_port = 8080;
        }
        else
        {
            printf("[CLIENT] Discovered scheduler at %s:%d\n", scheduler_ip, scheduler_port);
        }
    }

    // connect to scheduler over tcp
    printf("[CLIENT] Connecting to scheduler at %s:%d...\n", scheduler_ip, scheduler_port);
    int sock = connect_scheduler(scheduler_ip, scheduler_port);
    if (sock < 0)
    {
        fprintf(stderr, "[CLIENT] Failed to connect: %s\n", strerror(errno));
        return 1;
    }

    struct client_app app;
    memset(&app, 0, sizeof(app));
    app.server_fd = sock;
    strncpy(app.username, username_input, sizeof(app.username) - 1);
    app.logged_in = 1;
    app.running = 1;
    global_app = &app;

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    transport_init(&app.trans, sock, 0);
    transport_set_nonblocking(&app.trans);

    // send login packet as first message so scheduler knows our username
    struct protocol_message login_msg;
    memset(&login_msg, 0, sizeof(login_msg));
    login_msg.type = CLIENT_LOGIN;
    login_msg.request_id = ++app.next_req_id;
    login_msg.payload = app.username;
    login_msg.payload_len = strlen(app.username);
    transport_queue(&app.trans, &login_msg);
    transport_flush(&app.trans);

    printf("\nLogged in as '%s'. Type 'help' for command list.\nclient> ", app.username);
    fflush(stdout);

    // dual poll loop to watch both stdin and server socket
    struct pollfd pfds[2];
    char cmd_buf[MAX_CMD_LEN];

    while (app.running)
    {
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;

        pfds[1].fd = app.server_fd;
        pfds[1].events = POLLIN;
        if (transport_wants_write(&app.trans))
            pfds[1].events |= POLLOUT;
        pfds[1].revents = 0;

        int ret = poll(pfds, 2, 200);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        // read data from scheduler socket
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))
        {
            int tret = transport_read(&app.trans, handle_server_packet, &app);
            if (tret == TRANSPORT_CLOSED || tret == TRANSPORT_IO || tret == TRANSPORT_PROTOCOL)
            {
                printf("\n[Disconnected from scheduler]\n");
                break;
            }
        }

        // flush pending outgoing bytes
        if (pfds[1].revents & POLLOUT)
        {
            transport_flush(&app.trans);
        }

        // read and process user command from stdin
        if ((pfds[0].revents & POLLIN) && !app.follow_mode)
        {
            if (fgets(cmd_buf, sizeof(cmd_buf), stdin))
            {
                process_cli_command(&app, cmd_buf);
            }
            else
            {
                printf("\nExiting.\n");
                break;
            }
        }
    }

    transport_free(&app.trans);
    close(sock);
    return 0;
}
