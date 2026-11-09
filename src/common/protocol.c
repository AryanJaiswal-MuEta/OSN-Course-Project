/**
 * @file protocol.c
 * @brief This file implements the functions defined in protocol.h
 */

#include <stdlib.h>
#include <string.h>
#include "protocol.h"
/** put16/put32 helps in encoding header and payload easily */
static void put16(char *p, uint16_t value)
{
    p[0] = (char)(value >> 8);
    p[1] = (char)value;
}

static void put32(char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

/** get 16 / get 32 helps in decoding header and payload easily*/

static uint16_t get32(char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t get16(char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/**  validate using enum defined in header file */
int protocol_validate_type(uint16_t type)
{
    return type >= WORKER_REGISTER && type <= ERROR;
}

/** Protocol encoding will happen to load the header and payload into packets for tcp/udp */
int protocol_encode(const struct protocol_message *message, unsigned char *buffer, size_t capacity, size_t *written)
{
    size_t total;
    if (!message || !buffer || !written || message->payload_len > PROTOCOL_MAX_PAYLOAD)
        return PROTOCOL_BAD_ARGUMENT;
    if (!protocol_validate_type(message->type))
        return UNKNOWN_MESSAGE_TYPE;
    if (message->payload_len > 0 && !message->payload)
        return PROTOCOL_BAD_ARGUMENT;
    if (message->request_id > UINT32_MAX)
        return PROTOCOL_BAD_ARGUMENT;
    total = PROTOCOL_HEADER_SIZE + message->payload_len;
    if (capacity < total)
        return PROTOCOL_TOO_LARGE;

    put32(buffer, PROTOCOL_MAGIC_NUMBER);
    put16(buffer + 4, PROTOCOL_VERSION);
    put16(buffer + 6, message->type);
    put32(buffer + 8, (uint32_t)message->payload_len);
    put32(buffer + 12, (uint32_t)message->request_id);
    if (message->payload_len > 0)
    {
        memcpy(buffer + PROTOCOL_HEADER_SIZE, message->payload, message->payload_len);
    }
    *written = total;
    return PROTOCOL_COMPLETE;
}

/** we use switch - case pattern to return error messsages */
const char *protocol_error_name(int error)
{
    switch (error)
    {
    case SUCCESS:
        return "success";
    case INVALID_MESSAGE:
        return "invalid message";
    case UNSUPPORTED_VERSION:
        return "unsupported version";
    case UNKNOWN_MESSAGE_TYPE:
        return "unknown message type";
    case INVALID_JOB_ID:
        return "invalid job id";
    case WORKER_UNAVAILABLE:
        return "worker unavailable";
    case PERMISSION_DENIED:
        return "permission denied";
    default:
        return "unknown error";
    }
}

void protocol_decoder_init(struct protocol_decode *decoder)
{
    if (decoder)
        memset(decoder, 0, sizeof(*decoder));
}

void protocol_decoder_free(struct protocol_decode *decoder)
{
    if (!decoder)
        return;
    free(decoder->buffer);
    free(decoder->message_payload);
    protocol_decoder_init(decoder);
}

int protocol_decoder_feed(struct protocol_decode *decoder, const void *data, size_t length)
{
    char *grown;
    size_t needed, capacity;
    if (!decoder || (!data && length > 0))
        return PROTOCOL_BAD_ARGUMENT;
    if (length == 0)
        return PROTOCOL_COMPLETE;
    if (length > SIZE_MAX - decoder->length)
        return PROTOCOL_TOO_LARGE;
    needed = decoder->length + length;
    if (needed <= decoder->capacity)
    {
        memcpy(decoder->buffer + decoder->length, data, length);
        decoder->length = needed;
        return PROTOCOL_COMPLETE;
    }
    capacity = decoder->capacity ? decoder->capacity : PROTOCOL_HEADER_SIZE;
    while (capacity < needed)
    {
        if (capacity > SIZE_MAX / 2)
        {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    grown = realloc(decoder->buffer, capacity);
    if (!grown)
        return PROTOCOL_NO_MEMORY;
    decoder->buffer = grown;
    decoder->capacity = capacity;
    memcpy(decoder->buffer + decoder->length, data, length);
    decoder->length = needed;
    return PROTOCOL_COMPLETE;
}

int protocol_decoder_next(struct protocol_decode *decoder, struct protocol_message *message)
{
    uint16_t type;
    uint64_t request_id;
    uint32_t payload_len;
    size_t total;
    unsigned char *payload;
    if (!decoder || !message)
        return PROTOCOL_BAD_ARGUMENT;
    if (decoder->length < PROTOCOL_HEADER_SIZE)
        return PROTOCOL_INCOMPLETE;
    if (get32(decoder->buffer) != PROTOCOL_MAGIC_NUMBER ||
        get16(decoder->buffer + 4) != PROTOCOL_VERSION)
        return INVALID_MESSAGE;
    type = get16(decoder->buffer + 6);
    if (!protocol_validate_type(type))
        return UNKNOWN_MESSAGE_TYPE;
    payload_len = get32(decoder->buffer + 8);
    if (payload_len > PROTOCOL_MAX_PAYLOAD)
        return INVALID_MESSAGE;
    total = PROTOCOL_HEADER_SIZE + (size_t)payload_len;
    if (decoder->length < total)
        return PROTOCOL_INCOMPLETE;

    request_id = get32(decoder->buffer + 12);
    payload = NULL;
    if (payload_len)
    {
        payload = malloc(payload_len);
        if (!payload)
            return PROTOCOL_NO_MEMORY;
        memcpy(payload, decoder->buffer + PROTOCOL_HEADER_SIZE, payload_len);
    }
    free(decoder->message_payload);
    decoder->message_payload = payload;
    decoder->length -= total;
    memmove(decoder->buffer, decoder->buffer + total, decoder->length);
    message->type = type;
    message->request_id = request_id;
    message->payload = decoder->message_payload;
    message->payload_len = payload_len;
    return PROTOCOL_COMPLETE;
}