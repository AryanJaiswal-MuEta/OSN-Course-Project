#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "discovery.h"

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
    int i;
    for (i = 7; i >= 0; i--)
    {
        p[i] = (unsigned char)value;
        value >>= 8;
    }
}

static uint16_t get16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

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

int discovery_encode(const struct discovery_packet *packet, unsigned char *buffer, size_t capacity, size_t *written)
{
    if (!packet || !buffer || !written || packet->kind < DISCOVERY_REQUEST ||
        packet->kind > DISCOVERY_RESPONSE)
        return DISCOVERY_BAD_INPUT;
    if (capacity < 16)
        return DISCOVERY_TOO_LARGE;
    put32(buffer, DISCOVERY_MAGIC);
    buffer[4] = DISCOVERY_VERSION;
    buffer[5] = packet->kind;
    put16(buffer + 6, packet->port);
    put64(buffer + 8, packet->nonce);
    *written = 16;
    return DISCOVERY_OK;
}

int discovery_decode(const void *buffer, size_t length, struct discovery_packet *packet)
{
    const unsigned char *bytes = buffer;
    if (!bytes || !packet)
        return DISCOVERY_BAD_INPUT;
    if (length != 16 || get32(bytes) != DISCOVERY_MAGIC ||
        bytes[4] != DISCOVERY_VERSION ||
        bytes[5] < DISCOVERY_REQUEST || bytes[5] > DISCOVERY_RESPONSE)
        return DISCOVERY_INVALID_PACKET;
    packet->kind = bytes[5];
    packet->port = get16(bytes + 6);
    packet->nonce = get64(bytes + 8);
    return DISCOVERY_OK;
}

int discovery_socket(int reuse_address, int broadcast)
{
    int fd, yes = 1;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    if (reuse_address && setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                                    &yes, sizeof(yes)) < 0)
    {
        close(fd);
        return -1;
    }
    if (broadcast && setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
                                &yes, sizeof(yes)) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int discovery_send(int fd, const char *address, uint16_t port,const struct discovery_packet *packet)
{
    struct sockaddr_in destination;
    unsigned char bytes[DISCOVERY_MAX_PACKET];
    size_t length;
    ssize_t sent;
    if (fd < 0 || !address || !packet)
        return DISCOVERY_BAD_INPUT;
    if (discovery_encode(packet, bytes, sizeof(bytes), &length) != DISCOVERY_OK)
        return DISCOVERY_BAD_INPUT;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &destination.sin_addr) != 1)
        return DISCOVERY_BAD_INPUT;
    sent = sendto(fd, bytes, length, 0, (struct sockaddr *)&destination,
                  sizeof(destination));
    return sent == (ssize_t)length ? DISCOVERY_OK : DISCOVERY_IO;
}

const char *discovery_error_name(int error)
{
    switch (error)
    {
    case DISCOVERY_OK:
        return "ok";
    case DISCOVERY_BAD_INPUT:
        return "bad input";
    case DISCOVERY_TOO_LARGE:
        return "too large";
    case DISCOVERY_INVALID_PACKET:
        return "invalid packet";
    case DISCOVERY_IO:
        return "I/O error";
    case DISCOVERY_TIMEOUT:
        return "timeout";
    default:
        return "unknown error";
    }
}
