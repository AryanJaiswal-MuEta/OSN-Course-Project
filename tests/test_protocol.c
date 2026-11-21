/* Stress tests for the existing protocol.h / protocol.c API.
 * Compile from the repository root:
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -g \
 *      -Iinclude tests/test_protocol_stress.c src/common/protocol.c -o test_protocol_stress
 *   ./test_protocol_stress
 *
 * This test intentionally does NOT implement any protocol functionality.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"

#define ROUNDS 1000u
#define MAX_BATCH 20u
#define MAX_TEST_PAYLOAD 1024u
#define MAX_FRAME (PROTOCOL_HEADER_SIZE + MAX_TEST_PAYLOAD)
#define STREAM_CAP (MAX_BATCH * MAX_FRAME)

static uint32_t rng_state = 0xA5C3197Bu;
static unsigned long assertions = 0;
static unsigned long messages_checked = 0;

static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

#define REQUIRE(cond) do { \
    ++assertions; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL at %s:%d: %s (seed/state=0x%08X)\n", \
                __FILE__, __LINE__, #cond, (unsigned)rng_state); \
        return 0; \
    } \
} while (0)

struct expected {
    uint16_t type;
    uint32_t request_id;
    size_t len;
    unsigned char data[MAX_TEST_PAYLOAD];
};

static const uint16_t types[] = { WORKER_REGISTER, JOB_SUBMIT, JOB_COMPLETE };

static int check_output(struct protocol_message *actual, const struct expected *exp) {
    REQUIRE(actual->type == exp->type);
    REQUIRE(actual->request_id == exp->request_id);
    REQUIRE(actual->payload_len == exp->len);
    if (exp->len) {
        REQUIRE(actual->payload != NULL);
        REQUIRE(memcmp(actual->payload, exp->data, exp->len) == 0);
    }
    ++messages_checked;
    return 1;
}

static int randomized_streams(void) {
    unsigned char stream[STREAM_CAP];
    struct expected expected[MAX_BATCH];
    const size_t lengths[] = {0, 1, 2, 3, 15, 16, 17, 127, 128, 255, 256, 511, 512, 1023, 1024};

    for (unsigned round = 0; round < ROUNDS; ++round) {
        struct protocol_decode decoder;
        size_t stream_size = 0;
        size_t messages = 1u + (rnd() % MAX_BATCH);
        size_t emitted = 0;
        size_t cursor = 0;
        protocol_decoder_init(&decoder);

        for (size_t i = 0; i < messages; ++i) {
            struct protocol_message input;
            size_t written = 0;
            struct expected *exp = &expected[i];
            exp->type = types[rnd() % (sizeof types / sizeof types[0])];
            exp->request_id = (i == 0 && round % 4 == 0) ? UINT32_MAX : rnd();
            exp->len = (round % 3 == 0)
                ? lengths[rnd() % (sizeof lengths / sizeof lengths[0])]
                : (size_t)(rnd() % (MAX_TEST_PAYLOAD + 1u));
            for (size_t j = 0; j < exp->len; ++j)
                exp->data[j] = (unsigned char)rnd();
            input.type = exp->type;
            input.request_id = exp->request_id;
            input.payload_len = exp->len;
            input.payload = exp->len ? (const char *)exp->data : NULL;
            if (protocol_encode(&input, stream + stream_size,
                                sizeof stream - stream_size, &written) != PROTOCOL_COMPLETE) {
                fprintf(stderr, "FAIL encoding round %u message %zu\n", round, i);
                protocol_decoder_free(&decoder);
                return 0;
            }
            stream_size += written;
        }

        while (cursor < stream_size) {
            size_t remain = stream_size - cursor;
            size_t chunk;
            /* Alternate byte-by-byte, huge coalesced, and randomly fragmented delivery. */
            if (round % 5 == 0) chunk = 1;
            else if (round % 5 == 1) chunk = remain;
            else chunk = 1u + (rnd() % 113u);
            if (chunk > remain) chunk = remain;
            REQUIRE(protocol_decoder_feed(&decoder, stream + cursor, chunk) == PROTOCOL_COMPLETE);
            cursor += chunk;

            for (;;) {
                struct protocol_message output = {0};
                int status = protocol_decoder_next(&decoder, &output);
                if (status == PROTOCOL_INCOMPLETE) break;
                if (status != PROTOCOL_COMPLETE) {
                    fprintf(stderr, "FAIL decode status=%d, round=%u, consumed=%zu/%zu\n",
                            status, round, cursor, stream_size);
                    protocol_decoder_free(&decoder);
                    return 0;
                }
                if (emitted >= messages || !check_output(&output, &expected[emitted])) {
                    protocol_decoder_free(&decoder);
                    return 0;
                }
                ++emitted;
            }
        }
        REQUIRE(emitted == messages);
        {
            struct protocol_message output = {0};
            REQUIRE(protocol_decoder_next(&decoder, &output) == PROTOCOL_INCOMPLETE);
        }
        protocol_decoder_free(&decoder);
    }
    return 1;
}

static int invalid_frames(void) {
    unsigned char frame[PROTOCOL_HEADER_SIZE + 8];
    const unsigned char payload[] = {0x00, 0x80, 0xFF, 0x7F};
    struct protocol_message input = {WORKER_REGISTER, 0xFFFFFFFFu, (const char *)payload, sizeof payload};
    struct protocol_message output = {0};
    struct protocol_decode decoder;
    size_t written = 0;
    REQUIRE(protocol_encode(&input, frame, sizeof frame, &written) == PROTOCOL_COMPLETE);
    REQUIRE(written == PROTOCOL_HEADER_SIZE + sizeof payload);

    /* Reject wrong magic. */
    {
        unsigned char altered[sizeof frame];
        memcpy(altered, frame, written);
        altered[0] ^= 1;
        protocol_decoder_init(&decoder);
        REQUIRE(protocol_decoder_feed(&decoder, altered, written) == PROTOCOL_COMPLETE);
        REQUIRE(protocol_decoder_next(&decoder, &output) == INVALID_MESSAGE);
        protocol_decoder_free(&decoder);
    }
    /* Reject wrong protocol version. Version occupies bytes 4 and 5. */
    {
        unsigned char altered[sizeof frame];
        memcpy(altered, frame, written);
        altered[5] ^= 1;
        protocol_decoder_init(&decoder);
        REQUIRE(protocol_decoder_feed(&decoder, altered, written) == PROTOCOL_COMPLETE);
        REQUIRE(protocol_decoder_next(&decoder, &output) == INVALID_MESSAGE);
        protocol_decoder_free(&decoder);
    }
    /* A truncated valid frame must remain incomplete. */
    protocol_decoder_init(&decoder);
    REQUIRE(protocol_decoder_feed(&decoder, frame, written - 1) == PROTOCOL_COMPLETE);
    REQUIRE(protocol_decoder_next(&decoder, &output) == PROTOCOL_INCOMPLETE);
    protocol_decoder_free(&decoder);

    /* Verify the full-width request ID and high-bit bytes survive a normal round trip. */
    protocol_decoder_init(&decoder);
    REQUIRE(protocol_decoder_feed(&decoder, frame, written) == PROTOCOL_COMPLETE);
    REQUIRE(protocol_decoder_next(&decoder, &output) == PROTOCOL_COMPLETE);
    REQUIRE(output.request_id == UINT32_MAX);
    REQUIRE(output.payload_len == sizeof payload);
    REQUIRE(memcmp(output.payload, payload, sizeof payload) == 0);
    protocol_decoder_free(&decoder);
    return 1;
}

int main(void) {
    if (!randomized_streams()) return 1;
    printf("PASS randomized framing: %lu messages across %u batches\n", messages_checked, ROUNDS);
    if (!invalid_frames()) return 1;
    printf("PASS malformed, truncated and high-bit boundary cases\n");
    printf("PASS total assertions: %lu\n", assertions);
    return 0;
}
