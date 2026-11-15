/**
 * @file test_network.c
 * @brief Unit tests for TCP non-blocking transport, short-writes, buffering and pipelining
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"
#include "transport.h"

static int checks = 0;
#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 0; \
    } \
} while (0)

static int on_msg_received(const struct protocol_message *message, void *arg)
{
    int *count = arg;
    if (message->type != JOB_COMPLETE || message->payload_len != 3 ||
        memcmp(message->payload, "ok!", 3) != 0)
        return -1;
    (*count)++;
    return 0;
}

static int test_transport_basic(void)
{
    int fds[2], count = 0;
    struct transport sender = {0}, receiver = {0};
    const unsigned char payload[] = "ok!";
    struct protocol_message message = {JOB_COMPLETE, 9, payload, 3};

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(transport_init(&sender, fds[0], 1) == TRANSPORT_OK);
    CHECK(transport_init(&receiver, fds[1], 1) == TRANSPORT_OK);
    CHECK(transport_set_nonblocking(&sender) == TRANSPORT_OK);
    CHECK(transport_set_nonblocking(&receiver) == TRANSPORT_OK);

    CHECK(transport_queue(&sender, &message) == TRANSPORT_OK);
    CHECK(transport_wants_write(&sender));
    CHECK(transport_flush(&sender) == TRANSPORT_OK);

    CHECK(transport_read(&receiver, on_msg_received, &count) == TRANSPORT_WOULD_BLOCK);
    CHECK(count == 1 && !transport_wants_write(&sender));

    transport_free(&sender);
    transport_free(&receiver);
    return 1;
}

static int on_large_received(const struct protocol_message *message, void *arg)
{
    size_t *count = arg;
    if (message->type != JOB_COMPLETE || message->payload_len != 65536)
        return -1;
    (*count)++;
    return 0;
}

static int test_transport_short_write(void)
{
    int fds[2];
    int sndbuf_size = 2048;
    size_t count = 0;
    const size_t test_payload_len = 65536;

    unsigned char *payload = malloc(test_payload_len);
    CHECK(payload != NULL);
    memset(payload, 'A', test_payload_len);

    struct transport sender = {0}, receiver = {0};
    struct protocol_message message = {JOB_COMPLETE, 10, payload, test_payload_len};

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size)) == 0);
    CHECK(transport_init(&sender, fds[0], 1) == TRANSPORT_OK);
    CHECK(transport_init(&receiver, fds[1], 1) == TRANSPORT_OK);
    CHECK(transport_set_nonblocking(&sender) == TRANSPORT_OK);
    CHECK(transport_set_nonblocking(&receiver) == TRANSPORT_OK);

    CHECK(transport_queue(&sender, &message) == TRANSPORT_OK);

    while (transport_wants_write(&sender)) {
        int result = transport_flush(&sender);
        CHECK(result == TRANSPORT_OK || result == TRANSPORT_WOULD_BLOCK);
        CHECK(transport_read(&receiver, on_large_received, &count) == TRANSPORT_WOULD_BLOCK || count == 1);
    }

    CHECK(transport_read(&receiver, on_large_received, &count) == TRANSPORT_WOULD_BLOCK);
    CHECK(count == 1);

    transport_free(&sender);
    transport_free(&receiver);
    free(payload);
    return 1;
}

int main(void)
{
    int passed = 0;
    if (test_transport_basic()) {
        puts("PASS transport nonblocking send/recv");
        passed++;
    }
    if (test_transport_short_write()) {
        puts("PASS transport short-write backpressure");
        passed++;
    }

    printf("Network tests: %d groups passed, %d checks\n", passed, checks);
    return passed == 2 ? 0 : 1;
}

