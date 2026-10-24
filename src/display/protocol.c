#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "protocol.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int wos_send(int socket, const struct WosMessage *message, int descriptor)
{
    struct iovec vector = {(void *)message, sizeof(*message)};
    struct msghdr header = {0};
    union {
        struct cmsghdr alignment;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    header.msg_iov = &vector;
    header.msg_iovlen = 1;
    if (descriptor >= 0) {
        header.msg_control = control.bytes;
        header.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *ancillary = CMSG_FIRSTHDR(&header);
        ancillary->cmsg_level = SOL_SOCKET;
        ancillary->cmsg_type = SCM_RIGHTS;
        ancillary->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(ancillary), &descriptor, sizeof(descriptor));
    }
    ssize_t sent;
    do {
        sent = sendmsg(socket, &header, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0)
        return -1;
    if ((size_t)sent != sizeof(*message)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int wos_receive(int socket, struct WosMessage *message, int *descriptor)
{
    struct iovec vector = {message, sizeof(*message)};
    struct msghdr header = {0};
    union {
        struct cmsghdr alignment;
        char bytes[CMSG_SPACE(sizeof(int) * 2)];
    } control = {0};
    header.msg_iov = &vector;
    header.msg_iovlen = 1;
    header.msg_control = control.bytes;
    header.msg_controllen = sizeof(control.bytes);
    *descriptor = -1;
    ssize_t received;
    do {
        received = recvmsg(socket, &header, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received < 0)
        return -1;

    int count = 0;
    for (struct cmsghdr *item = CMSG_FIRSTHDR(&header); item;
         item = CMSG_NXTHDR(&header, item)) {
        if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS &&
            item->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int descriptors[2];
            size_t bytes = item->cmsg_len - CMSG_LEN(0);
            if (bytes > sizeof(descriptors)) {
                header.msg_flags |= MSG_CTRUNC;
                bytes = sizeof(descriptors);
            }
            if (bytes % sizeof(int) != 0)
                header.msg_flags |= MSG_CTRUNC;
            memcpy(descriptors, CMSG_DATA(item), bytes);
            if (count++ == 0)
                *descriptor = descriptors[0];
            else
                close(descriptors[0]);
            if (bytes == sizeof(descriptors)) {
                ++count;
                close(descriptors[1]);
            }
        }
    }
    if (received == 0 && count == 0)
        return 0;
    if (received != (ssize_t)sizeof(*message) ||
        header.msg_flags & (MSG_TRUNC | MSG_CTRUNC) ||
        message->magic != WOS_MAGIC || count > 1) {
        if (*descriptor >= 0)
            close(*descriptor);
        *descriptor = -1;
        errno = EPROTO;
        return -1;
    }
    return 1;
}
