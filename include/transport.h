/**
 * @file transport.h
 * @brief TCP transport helpers are implemented in this header file.These should be nonblocking
 */

#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stddef.h>
#include <sys/types.h>

#include "protocol.h"
/** To maintain standard error by subfunctions response mechanism we will define enum for error messages */
enum transport_error
{
    TRANSPORT_OK = 0,
    TRANSPORT_BAD_INPUT,
    TRANSPORT_NO_MEMORY,
    TRANSPORT_WOULD_BLOCK,
    TRANSPORT_CLOSED,
    TRANSPORT_IO,
    TRANSPORT_PROTOCOL
};

/** callback received for every tcp frame received*/
typedef int (*transport_message_function)(const struct protocol_message *message, void *arg);

/** We will define an struct for non blocking socket  */
struct transport
{
    int fd; // used for socket
    int fd_flag; // used for distinguishing the closing call
    char* pending; // for short writes
     size_t pending_len; 
    size_t pending_offset;
    size_t pending_capacity;
    struct protocol_decode decoder; // protocol decoder decodes what it receives
};

/** Initialize a transport around an existing socket descriptor */
int transport_init(struct transport *transport, int fd, int owns_fd);
/** Close the owned descriptor and release all buffers. */
void transport_free(struct transport *transport);
/** Set the descriptor to nonblocking mode. */
int transport_set_nonblocking(struct transport *transport);
/** Queue one encoded frame without blocking. */
int transport_queue(struct transport *transport,const struct protocol_message *message);
/** Flush queued bytes; returns TRANSPORT_WOULD_BLOCK if incomplete. */
int transport_flush(struct transport *transport);
/** Read available bytes and deliver complete messages to @p callback. */
int transport_read(struct transport *transport, transport_message_function callback, void *arg);
/** Whether a write poll event is currently needed. */
int transport_wants_write(const struct transport *transport);
/** Convert a transport error to a diagnostic string. */
const char *transport_error_name(int error);

#endif
