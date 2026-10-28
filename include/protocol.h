/**
 * @file protocol.h
 * @brief The header file defines the functions that help in communction from scheduler and TCP and UDP bytes scheme
 * @note Byte order will be in big endian
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
/** 4 bytes magic number for makibg sure it doesnt connect to any device */
#define PROTOCOL_MAGIC_NUMBER 0x47505436
/** protocol version defination */
#define PROTOCOL_VERSION 1
/** define the header size for packets */
#define PROTOCOL_HEADER_SIZE 16u
/** define the max payload acceptable */
#define PROTOCOL_MAX_PAYLOAD (1024u * 1024u)

enum protocol_type
{
    WORKER_REGISTER = 1,
    JOB_SUBMIT = 2,
    JOB_ASSIGN = 3,
    JOB_ACCEPTED = 4,
    JOB_COMPLETE = 5,
    JOB_FAILED = 6,
    ERROR = 7
};

/** Standardized error codes carried in protocol error responses. */
enum protocol_error
{
    SUCCESS = 0,
    INVALID_MESSAGE = 1,
    UNSUPPORTED_VERSION = 2,
    UNKNOWN_MESSAGE_TYPE = 3,
    INVALID_JOB_ID = 4,
    WORKER_UNAVAILABLE = 5,
    PERMISSION_DENIED = 6
};

/** sometimes while calling function returning errors as -1 in all cases makes degiing hard so simplified in here */
enum protocol_result
{
    PROTOCOL_COMPLETE = 0,
    PROTOCOL_BAD_ARGUMENT = -1,
    PROTOCOL_NO_MEMORY = -2,
    PROTOCOL_INCOMPLETE = -3,
    PROTOCOL_TOO_LARGE = -4,
    PROTOCOL_INVALID_FRAME = -5
};

/** A message before we encode or decode it we define is semantics  */
struct protocol_message
{
    uint64_t type;
    uint64_t request_id;
    const char *payload;
    size_t payload_len;
};

/** since we are implementing short writes  */
struct protocol_decode
{
    char *buffer;
    size_t length;
    size_t capacity;
};

/** Encode one message into a provided buffer. */
int protocol_encode(const struct protocol_message *message,unsigned char *buffer, size_t capacity, size_t *written);
/** Return type if it is validated if not raise error. */
int protocol_validate_type(uint16_t type);
/** Convert a protocol error to string (using struct definations) */
const char *protocol_error_name(int error);
/** Initialize an decoder. */
void protocol_decoder_init(struct protocol_decoder *decoder);
/** Release decoder once all the  buffer is used*/
void protocol_decoder_free(struct protocol_decoder *decoder);
/** Append bytes received from the network. */
int protocol_decoder_feed(struct protocol_decoder *decoder, const void *data, size_t length);
/** Extract one complete message, or return PROTOCOL_INCOMPLETE. */
int protocol_decoder_next(struct protocol_decoder *decoder,struct protocol_message *message);
                                               
#endif