#include "webos_media_protocol.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct WmMessage sent = {
        .magic = WM_MAGIC, .version = WM_VERSION, .type = WM_VIDEO,
        .size = 5, .token_high = UINT64_C(0x123456789abcdef0),
        .token_low = UINT64_C(0xfedcba9876543210),
        .time_us = INT64_C(1234567890123)
    };
    assert(wm_send(sockets[0], &sent, "frame") == 1);
    struct WmMessage received;
    void *payload = NULL;
    assert(wm_receive(sockets[1], &received, &payload) == 1);
    assert(memcmp(&sent, &received, sizeof(sent)) == 0);
    assert(memcmp(payload, "frame", 5) == 0);
    free(payload);

    sent.size = WM_MAX_PAYLOAD + 1;
    assert(wm_send(sockets[0], &sent, "frame") == -1 && errno == EINVAL);
    sent.size = 1;
    assert(wm_send(sockets[0], &sent, NULL) == -1 && errno == EINVAL);
    sent.size = 0;
    sent.type = WM_STATUS;
    assert(wm_send(sockets[0], &sent, NULL) == 1);
    assert(wm_receive(sockets[1], &received, &payload) == 1 && payload == NULL);

    sent.magic = 0;
    assert(send(sockets[0], &sent, sizeof(sent), 0) == sizeof(sent));
    assert(wm_receive(sockets[1], &received, &payload) == -1 && errno == EPROTO);
    close(sockets[0]);
    assert(wm_receive(sockets[1], &received, &payload) == 0);
    close(sockets[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    sent.magic = WM_MAGIC;
    sent.size = 5;
    assert(send(sockets[0], &sent, sizeof(sent), 0) == sizeof(sent));
    assert(send(sockets[0], "fr", 2, 0) == 2);
    close(sockets[0]);
    assert(wm_receive(sockets[1], &received, &payload) == -1 && errno == EPROTO);
    assert(payload == NULL);
    close(sockets[1]);
    return 0;
}
