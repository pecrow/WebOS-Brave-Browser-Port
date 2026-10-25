#include "webos_media_protocol.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/socket.h>

static int transfer(int socket, void *data, size_t size, int writing)
{
    size_t offset = 0;
    while (offset < size) {
        /* offset is bounded by size, supplied with the backing allocation. */
#if defined(__clang__)
#pragma clang unsafe_buffer_usage begin
#endif
        ssize_t count = writing
            ? send(socket, (const char *)data + offset, size - offset, MSG_NOSIGNAL)
            : recv(socket, (char *)data + offset, size - offset, 0);
#if defined(__clang__)
#pragma clang unsafe_buffer_usage end
#endif
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return -1;
        if (count == 0) {
            if (offset == 0 && !writing)
                return 0;
            errno = EPROTO;
            return -1;
        }
        offset += (size_t)count;
    }
    return 1;
}

int wm_send(int socket, const struct WmMessage *message, const void *payload)
{
    if (message->magic != WM_MAGIC || message->version != WM_VERSION ||
        message->size > WM_MAX_PAYLOAD || (message->size && !payload)) {
        errno = EINVAL;
        return -1;
    }
    if (transfer(socket, (void *)message, sizeof(*message), 1) != 1)
        return -1;
    return message->size
        ? transfer(socket, (void *)payload, message->size, 1) : 1;
}

int wm_receive(int socket, struct WmMessage *message, void **payload)
{
    *payload = NULL;
    int result = transfer(socket, message, sizeof(*message), 0);
    if (result != 1)
        return result;
    if (message->magic != WM_MAGIC || message->version != WM_VERSION ||
        message->size > WM_MAX_PAYLOAD) {
        errno = EPROTO;
        return -1;
    }
    if (!message->size)
        return 1;
    void *data = malloc(message->size);
    if (!data)
        return -1;
    if (transfer(socket, data, message->size, 0) != 1) {
        free(data);
        errno = EPROTO;
        return -1;
    }
    *payload = data;
    return 1;
}
