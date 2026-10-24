#define _GNU_SOURCE
#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
    struct WosMessage sent = {.magic = WOS_MAGIC, .type = WOS_WINDOW, .widget = 42};
    struct WosMessage received;
    int descriptor;
    assert(wos_send(sockets[0], &sent, -1) == 0);
    assert(wos_receive(sockets[1], &received, &descriptor) == 1);
    assert(descriptor == -1 && received.widget == 42);

    int original = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(original >= 0);
    assert(wos_send(sockets[0], &sent, original) == 0);
    assert(wos_receive(sockets[1], &received, &descriptor) == 1);
    assert(descriptor >= 0);
    assert(fcntl(descriptor, F_GETFD) & FD_CLOEXEC);
    close(descriptor);
    struct iovec vector = {&sent, sizeof(sent)};
    union {
        struct cmsghdr alignment;
        char bytes[CMSG_SPACE(sizeof(int) * 2)];
    } control = {0};
    struct msghdr header = {0};
    header.msg_iov = &vector;
    header.msg_iovlen = 1;
    header.msg_control = control.bytes;
    header.msg_controllen = sizeof(control.bytes);
    struct cmsghdr *rights = CMSG_FIRSTHDR(&header);
    rights->cmsg_level = SOL_SOCKET;
    rights->cmsg_type = SCM_RIGHTS;
    rights->cmsg_len = CMSG_LEN(sizeof(int) * 2);
    int duplicated[2] = {original, original};
    memcpy(CMSG_DATA(rights), duplicated, sizeof(duplicated));
    assert(sendmsg(sockets[0], &header, 0) == (ssize_t)sizeof(sent));
    assert(wos_receive(sockets[1], &received, &descriptor) == -1);
    assert(errno == EPROTO && descriptor == -1);

    union {
        struct cmsghdr alignment;
        char bytes[CMSG_SPACE(sizeof(int) * 4)];
    } excess_control = {0};
    header.msg_control = excess_control.bytes;
    header.msg_controllen = sizeof(excess_control.bytes);
    rights = CMSG_FIRSTHDR(&header);
    rights->cmsg_level = SOL_SOCKET;
    rights->cmsg_type = SCM_RIGHTS;
    rights->cmsg_len = CMSG_LEN(sizeof(int) * 4);
    int excess[4] = {original, original, original, original};
    memcpy(CMSG_DATA(rights), excess, sizeof(excess));
    int next_descriptor = dup(original);
    assert(next_descriptor >= 0);
    close(next_descriptor);
    assert(sendmsg(sockets[0], &header, 0) == (ssize_t)sizeof(sent));
    assert(wos_receive(sockets[1], &received, &descriptor) == -1);
    assert(errno == EPROTO && descriptor == -1);
    int available_descriptor = dup(original);
    assert(available_descriptor == next_descriptor);
    close(available_descriptor);
    close(original);

    char oversized[sizeof(sent) + 1] = {0};
    memcpy(oversized, &sent, sizeof(sent));
    assert(send(sockets[0], oversized, sizeof(oversized), 0) == (ssize_t)sizeof(oversized));
    assert(wos_receive(sockets[1], &received, &descriptor) == -1);
    assert(errno == EPROTO && descriptor == -1);

    sent.magic = 0;
    assert(wos_send(sockets[0], &sent, -1) == 0);
    assert(wos_receive(sockets[1], &received, &descriptor) == -1);
    assert(errno == EPROTO && descriptor == -1);

    assert(send(sockets[0], "short", 5, 0) == 5);
    assert(wos_receive(sockets[1], &received, &descriptor) == -1);
    assert(errno == EPROTO);
    close(sockets[0]);
    assert(wos_receive(sockets[1], &received, &descriptor) == 0);
    close(sockets[1]);
    puts("Display protocol tests passed.");
    return 0;
}
