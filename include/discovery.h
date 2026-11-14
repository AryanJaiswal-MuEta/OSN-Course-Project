/**
 * @file discovery.h
 * @brief Small UDP scheduler discovery packet helpers.
 */
#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define DISCOVERY_MAGIC 0x4f53444eu
#define DISCOVERY_VERSION 1
#define DISCOVERY_MAX_PACKET 64

enum discovery_kind {
    DISCOVERY_REQUEST = 1,
    DISCOVERY_RESPONSE = 2
};

enum discovery_error {
    DISCOVERY_OK = 0,
    DISCOVERY_BAD_INPUT,
    DISCOVERY_TOO_LARGE,
    DISCOVERY_INVALID_PACKET,
    DISCOVERY_IO,
    DISCOVERY_TIMEOUT
};

/** Decoded discovery packet. */
struct discovery_packet {
    uint8_t kind;
    uint16_t port;
    uint64_t nonce;
};

/** Encode a request or response */
int discovery_encode(const struct discovery_packet *packet, unsigned char *buffer, size_t capacity, size_t *written);
/** Decode and validate one discovery packet. */
int discovery_decode(const void *buffer, size_t length,
                     struct discovery_packet *packet);
/** Open a UDP socket suitable for broadcast discovery. */
int discovery_socket(int reuse_address, int broadcast);
/** Send a discovery packet to an IPv4 address and port. */
int discovery_send(int fd, const char *address, uint16_t port, const struct discovery_packet *packet);
/** Convert a discovery error to a human readable string. */
const char *discovery_error_name(int error);

#endif
