/**
 * @file test_discovery.c
 * @brief Unit and stress tests for UDP discovery encoding, decoding and packet exchange
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "discovery.h"

static int checks = 0;
#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 0; \
    } \
} while (0)

static int test_discovery_encode_decode(void)
{
    unsigned char bytes[DISCOVERY_MAX_PACKET];
    struct discovery_packet input = {DISCOVERY_RESPONSE, 4321, 0x123456789ABCDEF0ULL};
    struct discovery_packet output;
    size_t length = 0;

    CHECK(discovery_encode(&input, bytes, sizeof(bytes), &length) == DISCOVERY_OK);
    CHECK(length == 16);
    CHECK(discovery_decode(bytes, length, &output) == DISCOVERY_OK);
    CHECK(output.kind == input.kind);
    CHECK(output.port == input.port);
    CHECK(output.nonce == input.nonce);

    // corrupted magic number
    bytes[0] = 0;
    CHECK(discovery_decode(bytes, length, &output) == DISCOVERY_INVALID_PACKET);

    return 1;
}

static int test_discovery_boundaries(void)
{
    unsigned char bytes[16];
    struct discovery_packet dummy;
    size_t wr = 0;
    struct discovery_packet input = {DISCOVERY_REQUEST, 8080, 999};

    CHECK(discovery_encode(&input, bytes, 15, &wr) == DISCOVERY_TOO_LARGE);
    CHECK(discovery_encode(&input, bytes, 16, &wr) == DISCOVERY_OK);

    // truncated lengths from 0 to 15
    for (size_t len = 0; len < 16; len++) {
        CHECK(discovery_decode(bytes, len, &dummy) == DISCOVERY_INVALID_PACKET);
    }

    // bad version
    bytes[4] = 99;
    CHECK(discovery_decode(bytes, 16, &dummy) == DISCOVERY_INVALID_PACKET);

    // bad kind
    bytes[4] = DISCOVERY_VERSION;
    bytes[5] = 99;
    CHECK(discovery_decode(bytes, 16, &dummy) == DISCOVERY_INVALID_PACKET);

    return 1;
}

static int test_discovery_datagram_exchange(void)
{
    int dgram_fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, dgram_fds) == 0);

    for (int i = 0; i < 50; i++) {
        uint64_t nonce = 0xAA000000ULL | (uint32_t)i;
        struct discovery_packet req = {DISCOVERY_REQUEST, 0, nonce};

        unsigned char tx_buf[32];
        size_t tx_len = 0;
        CHECK(discovery_encode(&req, tx_buf, sizeof(tx_buf), &tx_len) == DISCOVERY_OK);
        CHECK(send(dgram_fds[0], tx_buf, tx_len, 0) == 16);

        unsigned char rx_buf[64];
        ssize_t n = recv(dgram_fds[1], rx_buf, sizeof(rx_buf), 0);
        CHECK(n == 16);

        struct discovery_packet decoded_req;
        CHECK(discovery_decode(rx_buf, (size_t)n, &decoded_req) == DISCOVERY_OK);
        CHECK(decoded_req.kind == DISCOVERY_REQUEST);
        CHECK(decoded_req.nonce == nonce);

        // reply with response
        struct discovery_packet rep = {DISCOVERY_RESPONSE, 8080, nonce};
        CHECK(discovery_encode(&rep, tx_buf, sizeof(tx_buf), &tx_len) == DISCOVERY_OK);
        CHECK(send(dgram_fds[1], tx_buf, tx_len, 0) == 16);

        n = recv(dgram_fds[0], rx_buf, sizeof(rx_buf), 0);
        CHECK(n == 16);

        struct discovery_packet decoded_rep;
        CHECK(discovery_decode(rx_buf, (size_t)n, &decoded_rep) == DISCOVERY_OK);
        CHECK(decoded_rep.kind == DISCOVERY_RESPONSE);
        CHECK(decoded_rep.port == 8080);
        CHECK(decoded_rep.nonce == nonce);
    }

    close(dgram_fds[0]);
    close(dgram_fds[1]);
    return 1;
}

int main(void)
{
    int passed = 0;
    if (test_discovery_encode_decode()) {
        puts("PASS discovery encode/decode");
        passed++;
    }
    if (test_discovery_boundaries()) {
        puts("PASS discovery boundaries & corrupted inputs");
        passed++;
    }
    if (test_discovery_datagram_exchange()) {
        puts("PASS discovery datagram exchange");
        passed++;
    }

    printf("Discovery tests: %d groups passed, %d checks\n", passed, checks);
    return passed == 3 ? 0 : 1;
}

