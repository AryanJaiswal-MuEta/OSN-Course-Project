#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "transport.h"
/** this function  takes care if the transport socket needs more space or not */
int reserve(struct transport *transport, size_t extra)
{
    unsigned char *grown;
    size_t needed, capacity;
    if (extra > PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD -
                    transport->pending_len)
        return TRANSPORT_NO_MEMORY;
    needed = transport->pending_len + extra;
    if (needed <= transport->pending_capacity)
        return TRANSPORT_OK;
    capacity = transport->pending_capacity ? transport->pending_capacity : 256;
    while (capacity < needed)
    {
        if (capacity > PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD / 2)
        {
            capacity = PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD;
            break;
        }
        capacity *= 2;
    }
    grown = realloc((char*)transport->pending, capacity);
    if (!grown)
        return TRANSPORT_NO_MEMORY;
    transport->pending = (char*)grown;
    transport->pending_capacity = capacity;
    return TRANSPORT_OK;
}
/** init transport give sock fd */
int transport_init(struct transport *transport, int fd, int owns_fd)
{
    if (!transport || fd < 0 || (owns_fd != 0 && owns_fd != 1))
        return TRANSPORT_BAD_INPUT;
    memset(transport, 0, sizeof(*transport));
    transport->fd = fd;
    transport->fd_flag = owns_fd;
    protocol_decoder_init(&transport->decoder);
    return TRANSPORT_OK;
}
/** free the transport of fd_flag == 1 */
void transport_free(struct transport *transport)
{
    if (!transport)
        return;
    if (transport->fd_flag && transport->fd >= 0)
        close(transport->fd);
    free(transport->pending);
    protocol_decoder_free(&transport->decoder);
    memset(transport, 0, sizeof(*transport));
    transport->fd = -1;
}
/** use non blocking mode */
int transport_set_nonblocking(struct transport *transport)
{
    int flags;
    if (!transport || transport->fd < 0)
        return TRANSPORT_BAD_INPUT;
    flags = fcntl(transport->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(transport->fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return TRANSPORT_IO;
    return TRANSPORT_OK;
}
/** maintining a queue for messsage in non blocking fashion */
int transport_queue(struct transport *transport,const struct protocol_message *message)
{
    unsigned char encoded[PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD];
    size_t written;
    int result;
    if (!transport || !message)
        return TRANSPORT_BAD_INPUT;
    result = protocol_encode(message, encoded, sizeof(encoded), &written);
    if (result != PROTOCOL_COMPLETE)
        return TRANSPORT_PROTOCOL;
    result = reserve(transport, written);
    if (result != TRANSPORT_OK)
        return result;
    memcpy(transport->pending + transport->pending_len, encoded, written);
    transport->pending_len += written;
    return TRANSPORT_OK;
}

int flush_pending(struct transport *transport)
{
    ssize_t count;
    while (transport->pending_offset < transport->pending_len)
    {
        count = send(transport->fd, transport->pending + transport->pending_offset,
                     transport->pending_len - transport->pending_offset, 0);
        if (count > 0)
            transport->pending_offset += (size_t)count;
        else if (count == 0)
            return TRANSPORT_WOULD_BLOCK;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return TRANSPORT_WOULD_BLOCK;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            return TRANSPORT_IO;
    }
    transport->pending_len = 0;
    transport->pending_offset = 0;
    return TRANSPORT_OK;
}

int transport_flush(struct transport *transport)
{
    if (!transport || transport->fd < 0)
        return TRANSPORT_BAD_INPUT;
    return flush_pending(transport);
}

int transport_read(struct transport *transport, transport_message_function callback,void *arg)
{
    unsigned char buffer[4096];
    ssize_t count;
    int result;
    struct protocol_message message;
    if (!transport || transport->fd < 0 || !callback)
        return TRANSPORT_BAD_INPUT;
    for (;;)
    {
        count = recv(transport->fd, buffer, sizeof(buffer), 0);
        if (count > 0)
        {
            result = protocol_decoder_feed(&transport->decoder, buffer, (size_t)count);
            if (result != PROTOCOL_COMPLETE)
                return TRANSPORT_PROTOCOL;
            for (;;)
            {
                result = protocol_decoder_next(&transport->decoder, &message);
                if (result == PROTOCOL_INCOMPLETE)
                    break;
                if (result != PROTOCOL_COMPLETE)
                    return TRANSPORT_PROTOCOL;
                if (callback(&message, arg) != 0)
                    return TRANSPORT_IO;
            }
        }
        else if (count == 0)
            return TRANSPORT_CLOSED;
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            return TRANSPORT_WOULD_BLOCK;
        else if (errno == EINTR)
            continue;
        else
            return TRANSPORT_IO;
    }
}

int transport_wants_write(const struct transport *transport)
{
    return transport && transport->pending_offset < transport->pending_len;
}
/**using switch base to  convert the errror codes in protocol to simple meaning for abstraction */
const char *transport_error_name(int error)
{
    switch (error)
    {
    case TRANSPORT_OK:
        return "ok";
    case TRANSPORT_BAD_INPUT:
        return "bad input";
    case TRANSPORT_NO_MEMORY:
        return "out of memory";
    case TRANSPORT_WOULD_BLOCK:
        return "would block";
    case TRANSPORT_CLOSED:
        return "closed";
    case TRANSPORT_IO:
        return "I/O error";
    case TRANSPORT_PROTOCOL:
        return "protocol error";
    default:
        return "unknown error";
    }
}
