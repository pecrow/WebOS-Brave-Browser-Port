#ifndef BRAVE_WEBOS_MEDIA_PROTOCOL_H
#define BRAVE_WEBOS_MEDIA_PROTOCOL_H

#include <stdint.h>

#define WM_MAGIC UINT32_C(0x574d4544)
#define WM_VERSION 1U
#define WM_MAX_PAYLOAD (4U * 1024U * 1024U)

enum WmType {
    WM_OPEN = 1,
    WM_START,
    WM_VIDEO,
    WM_AUDIO,
    WM_RATE,
    WM_VOLUME,
    WM_STATUS,
    WM_EOS,
    WM_FLUSH,
    WM_CLOSE,
    WM_GEOMETRY,
    WM_REPLY
};

enum WmCodec {
    WM_CODEC_NONE = 0,
    WM_H264 = 1,
    WM_VP9 = 2,
    WM_AAC = 3,
    WM_OPUS = 4
};

enum WmState {
    WM_LOADED = 1,
    WM_HAS_FRAME = 2,
    WM_ENDED = 4,
    WM_FAILED = 8
};

enum WmResult {
    WM_OK = 0,
    WM_RETRY = 1,
    WM_UNSUPPORTED = 2,
    WM_ERROR = -1
};

struct WmMessage {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t size;
    uint64_t token_high;
    uint64_t token_low;
    int64_t time_us;
    int32_t args[10];
};

#ifdef __cplusplus
static_assert(sizeof(WmMessage) == 80, "Native media wire ABI changed");
extern "C" {
#else
_Static_assert(sizeof(struct WmMessage) == 80, "Native media wire ABI changed");
#endif

int wm_send(int socket, const struct WmMessage *message, const void *payload);
int wm_receive(int socket, struct WmMessage *message, void **payload);

#ifdef __cplusplus
}
#endif
#endif
