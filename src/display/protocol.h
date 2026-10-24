#ifndef BRAVE_WEBOS_DISPLAY_PROTOCOL_H
#define BRAVE_WEBOS_DISPLAY_PROTOCOL_H

#include <stdint.h>

#define WOS_MAGIC UINT32_C(0x42574f53)
#define WOS_MAX_WIDTH 1920
#define WOS_MAX_HEIGHT 1080

enum WosType {
    WOS_WINDOW = 1,
    WOS_BUFFER = 2,
    WOS_PRESENT = 3,
    WOS_CLOSE = 4,
    WOS_ACK = 5,
    WOS_MOUSE_MOVE = 6,
    WOS_MOUSE_BUTTON = 7,
    WOS_MOUSE_WHEEL = 8,
    WOS_KEY = 9,
    WOS_FOCUS = 10,
    WOS_TEXT_INPUT = 11,
    WOS_TEXT_COMMIT = 12,
    WOS_TEXT_PREEDIT = 13,
    WOS_TEXT_CLEAR = 14
};

enum WosTextType {
    WOS_TEXT_NONE,
    WOS_TEXT_NORMAL,
    WOS_TEXT_PASSWORD,
    WOS_TEXT_PASSWORD_VISIBLE,
    WOS_TEXT_EMAIL,
    WOS_TEXT_NUMBER
};

enum WosTextFlag {
    WOS_TEXT_MULTILINE = 1,
    WOS_TEXT_AUTOCORRECT = 2,
    WOS_TEXT_CAP_LETTERS = 4,
    WOS_TEXT_CAP_WORDS = 8,
    WOS_TEXT_CAP_SENTENCES = 16,
    WOS_TEXT_RESET = 32
};

#define WOS_TEXT_FIRST 1U
#define WOS_TEXT_LAST 2U
#define WOS_TEXT_LENGTH_SHIFT 8
#define WOS_MAX_TEXT_BYTES 65536

enum WosModifier {
    WOS_SHIFT = 1,
    WOS_CONTROL = 2,
    WOS_ALT = 4,
    WOS_META = 8,
    WOS_CAPS_LOCK = 16,
    WOS_NUM_LOCK = 32
};

struct WosMessage {
    uint32_t magic;
    uint32_t type;
    uint64_t widget;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t code;
    uint32_t state;
    uint32_t extra;
    uint32_t reserved;
};

struct WosTextMessage {
    uint32_t magic;
    uint32_t type;
    uint64_t widget;
    uint32_t serial;
    uint32_t flags;
    int32_t start;
    int32_t length;
    char text[16];
};

#ifdef __cplusplus
static_assert(sizeof(WosMessage) == 48);
static_assert(sizeof(WosTextMessage) == sizeof(WosMessage));
extern "C" {
#else
_Static_assert(sizeof(struct WosMessage) == 48, "Wire ABI changed");
_Static_assert(sizeof(struct WosTextMessage) == sizeof(struct WosMessage),
               "Text wire ABI changed");
#endif

int wos_send(int socket, const struct WosMessage *message, int descriptor);
int wos_receive(int socket, struct WosMessage *message, int *descriptor);

#ifdef __cplusplus
}
#endif
#endif
