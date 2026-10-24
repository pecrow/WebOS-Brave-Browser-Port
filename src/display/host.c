#define _GNU_SOURCE
#include "protocol.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_CLIENTS 32
#define MAX_WINDOWS 16
#define DISPLAY_WIDTH 1280
#define DISPLAY_HEIGHT 720
#define MAX_BUFFER_BYTES (32U * 1024U * 1024U)
#define BACK_HOLD_MS 3000

struct Client {
    int fd;
    uint64_t widget;
    uint32_t role;
};

struct Window {
    uint64_t widget;
    uint64_t order;
    int control;
    int canvas;
    int ime;
    struct WosMessage text_input;
    int x, y, width, height;
    int buffer_width, buffer_height;
    bool visible;
    void *pixels;
    size_t bytes;
    SDL_Texture *texture;
};

static struct Client clients[MAX_CLIENTS];
static struct Window windows[MAX_WINDOWS];
static SDL_Renderer *renderer;
static SDL_Window *display;
static uint64_t text_widget;
static uint32_t text_serial;
static uint32_t text_type;
static uint32_t text_flags;
static bool display_focused = true;
static bool back_down;
static uint64_t back_pressed_at;
static uint64_t sequence;
static size_t buffer_bytes;
static volatile sig_atomic_t stopping;
static volatile sig_atomic_t dump_requested;
static bool dirty;
static struct {
    uint64_t time, widget;
    int x, y, local_x, local_y;
    bool down;
} pointer_history[8];
static unsigned int pointer_count;

static void update_text_input(void);

static void stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void request_dump(int signal_number)
{
    (void)signal_number;
    dump_requested = 1;
}

static void dump_display_state(void)
{
    int width, height;
    if (SDL_GetWindowSize(display, &width, &height))
        fprintf(stderr, "Display state: window=%dx%d focused=%d text_widget=%llu\n",
                width, height, display_focused, (unsigned long long)text_widget);
    else
        fprintf(stderr, "Display size query failed: %s\n", SDL_GetError());
    int pixel_width, pixel_height, output_width, output_height;
    if (SDL_GetWindowSizeInPixels(display, &pixel_width, &pixel_height) &&
        SDL_GetRenderOutputSize(renderer, &output_width, &output_height))
        fprintf(stderr, "Display pixels=%dx%d render_output=%dx%d modifiers=%u\n",
                pixel_width, pixel_height, output_width, output_height,
                (unsigned int)SDL_GetModState());
    else
        fprintf(stderr, "Display output query failed: %s\n", SDL_GetError());
    float mouse_x, mouse_y, render_x, render_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    if (SDL_RenderCoordinatesFromWindow(renderer, mouse_x, mouse_y,
                                         &render_x, &render_y))
        fprintf(stderr, "Display pointer: window=%.2f,%.2f render=%.2f,%.2f\n",
                mouse_x, mouse_y, render_x, render_y);
    else
        fprintf(stderr, "Display pointer query failed: %s\n", SDL_GetError());
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        struct Window *window = &windows[i];
        if (window->widget)
            fprintf(stderr, "Display widget=%llu visible=%d order=%llu "
                    "bounds=%d,%d %dx%d buffer=%dx%d control=%d canvas=%d ime=%d\n",
                    (unsigned long long)window->widget, window->visible,
                    (unsigned long long)window->order, window->x, window->y,
                    window->width, window->height, window->buffer_width,
                    window->buffer_height, window->control, window->canvas, window->ime);
    }
    unsigned int count = SDL_min(pointer_count, SDL_arraysize(pointer_history));
    for (unsigned int offset = count; offset > 0; --offset) {
        unsigned int index = (pointer_count - offset) % SDL_arraysize(pointer_history);
        fprintf(stderr, "Pointer time=%llu down=%d widget=%llu screen=%d,%d local=%d,%d\n",
                (unsigned long long)pointer_history[index].time,
                pointer_history[index].down,
                (unsigned long long)pointer_history[index].widget,
                pointer_history[index].x, pointer_history[index].y,
                pointer_history[index].local_x, pointer_history[index].local_y);
    }
    dump_requested = 0;
}

static void sdl_check(bool result)
{
    if (!result) {
        fprintf(stderr, "Native display: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
}

static struct Window *find_window(uint64_t widget, bool create)
{
    struct Window *empty = NULL;
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        if (windows[i].widget == widget && widget != 0)
            return &windows[i];
        if (windows[i].widget == 0)
            empty = &windows[i];
    }
    if (!create || !empty || !widget)
        return NULL;
    *empty = (struct Window){
        .widget = widget, .control = -1, .canvas = -1, .ime = -1
    };
    return empty;
}

static void release_buffer(struct Window *window)
{
    if (window->texture)
        SDL_DestroyTexture(window->texture);
    if (window->pixels)
        munmap(window->pixels, window->bytes);
    buffer_bytes -= window->bytes;
    window->texture = NULL;
    window->pixels = NULL;
    window->bytes = 0;
    dirty = true;
}

static void disconnect(struct Client *client)
{
    struct Window *window = find_window(client->widget, false);
    if (window) {
        if (window->control == client->fd) {
            window->control = -1;
            window->visible = false;
        }
        if (window->canvas == client->fd) {
            window->canvas = -1;
            release_buffer(window);
        }
        if (window->ime == client->fd) {
            window->ime = -1;
            window->text_input.state = 0;
        }
        if (window->control < 0 && window->canvas < 0 && window->ime < 0)
            window->widget = 0;
    }
    close(client->fd);
    *client = (struct Client){.fd = -1};
    dirty = true;
    update_text_input();
}

static bool accept_client(int listener, uid_t browser_uid)
{
    int connection = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (connection < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return true;
        perror("Display accept");
        return false;
    }
    struct ucred peer;
    socklen_t size = sizeof(peer);
    if (getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
        peer.uid == browser_uid) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].fd < 0) {
                clients[i] = (struct Client){.fd = connection};
                return true;
            }
        }
    }
    fprintf(stderr, "Rejected display peer or exceeded client limit\n");
    close(connection);
    return true;
}

static bool receive_message(struct Client *client)
{
    struct WosMessage message;
    int descriptor;
    int result = wos_receive(client->fd, &message, &descriptor);
    if (result <= 0) {
        if (result < 0)
            fprintf(stderr, "Display IPC receive: %s\n", strerror(errno));
        return false;
    }
    bool accepted = false;
    struct Window *window = NULL;
    if (client->widget && client->widget != message.widget)
        goto done;
    window = find_window(message.widget, true);
    if (!window)
        goto done;
    client->widget = message.widget;
    if (message.type == WOS_WINDOW && descriptor < 0 &&
        (client->role == 0 || client->role == WOS_WINDOW) &&
        (window->control < 0 || window->control == client->fd) &&
        message.width > 0 && message.width <= WOS_MAX_WIDTH &&
        message.height > 0 && message.height <= WOS_MAX_HEIGHT &&
        message.x >= -WOS_MAX_WIDTH && message.x <= WOS_MAX_WIDTH &&
        message.y >= -WOS_MAX_HEIGHT && message.y <= WOS_MAX_HEIGHT) {
        client->role = WOS_WINDOW;
        window->control = client->fd;
        window->x = message.x;
        window->y = message.y;
        window->width = message.width;
        window->height = message.height;
        if (!window->visible && message.state)
            window->order = ++sequence;
        window->visible = message.state != 0;
        dirty = true;
        if (stopping && window->visible) {
            struct WosMessage close_request = {
                .magic = WOS_MAGIC, .type = WOS_CLOSE, .widget = window->widget
            };
            if (wos_send(client->fd, &close_request, -1) < 0) {
                perror("Close browser window created during shutdown");
                goto done;
            }
        }
        accepted = true;
    } else if (message.type == WOS_TEXT_INPUT && descriptor < 0 &&
               (client->role == 0 || client->role == WOS_TEXT_INPUT) &&
               (window->ime < 0 || window->ime == client->fd) &&
               message.state <= 1 && message.code <= WOS_TEXT_NUMBER) {
        client->role = WOS_TEXT_INPUT;
        window->ime = client->fd;
        window->text_input = message;
        window->text_input.reserved &= ~WOS_TEXT_RESET;
        accepted = true;
        update_text_input();
        if ((message.reserved & WOS_TEXT_RESET) && text_widget == window->widget)
            sdl_check(SDL_ClearComposition(display));
    } else if (message.type == WOS_BUFFER && descriptor >= 0 &&
               (client->role == 0 || client->role == WOS_BUFFER) &&
               (window->canvas < 0 || window->canvas == client->fd) &&
               message.width > 0 && message.width <= WOS_MAX_WIDTH &&
               message.height > 0 && message.height <= WOS_MAX_HEIGHT) {
        size_t bytes = (size_t)message.width * message.height * 4;
        struct stat information;
        if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
            information.st_size != (off_t)bytes ||
            buffer_bytes - window->bytes + bytes > MAX_BUFFER_BYTES)
            goto done;
        void *pixels = mmap(NULL, bytes, PROT_READ, MAP_SHARED, descriptor, 0);
        if (pixels == MAP_FAILED) {
            perror("Display buffer mmap");
            goto done;
        }
        SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32,
                                                SDL_TEXTUREACCESS_STREAMING,
                                                message.width, message.height);
        if (!texture) {
            munmap(pixels, bytes);
            fprintf(stderr, "Display texture: %s\n", SDL_GetError());
            goto done;
        }
        sdl_check(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED));
        sdl_check(SDL_UpdateTexture(texture, NULL, pixels, message.width * 4));
        release_buffer(window);
        window->canvas = client->fd;
        client->role = WOS_BUFFER;
        window->pixels = pixels;
        window->bytes = bytes;
        window->texture = texture;
        window->buffer_width = message.width;
        window->buffer_height = message.height;
        buffer_bytes += bytes;
        accepted = true;
    } else if (message.type == WOS_PRESENT && descriptor < 0 &&
               window->canvas == client->fd && window->pixels) {
        sdl_check(SDL_UpdateTexture(window->texture, NULL, window->pixels,
                                   window->buffer_width * 4));
        struct WosMessage acknowledgment = {
            .magic = WOS_MAGIC, .type = WOS_ACK, .widget = message.widget
        };
        if (wos_send(client->fd, &acknowledgment, -1) < 0) {
            perror("Display frame acknowledgment");
            goto done;
        }
        dirty = true;
        accepted = true;
    }
done:
    if (descriptor >= 0)
        close(descriptor);
    if (!accepted)
        fprintf(stderr, "Rejected display message type %u\n", message.type);
    else if (message.type == WOS_WINDOW)
        update_text_input();
    return accepted;
}

static struct Window *text_window(void)
{
    struct Window *top = NULL;
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        struct Window *window = &windows[i];
        if (window->widget && window->visible && window->control >= 0 &&
            window->ime >= 0 && window->text_input.state &&
            (!top || top->order < window->order))
            top = window;
    }
    return top;
}

static void update_text_input(void)
{
    if (!display)
        return;
    struct Window *window = display_focused && !stopping ? text_window() : NULL;
    if (!window) {
        if (SDL_TextInputActive(display))
            sdl_check(SDL_StopTextInput(display));
        text_widget = 0;
        return;
    }
    struct WosMessage *context = &window->text_input;
    SDL_Rect caret = {
        .x = SDL_clamp(context->x, 0, DISPLAY_WIDTH - 1),
        .y = SDL_clamp(context->y, 0, DISPLAY_HEIGHT - 1),
        .w = SDL_clamp(context->width, 1, DISPLAY_WIDTH),
        .h = SDL_clamp(context->height, 1, DISPLAY_HEIGHT)
    };
    sdl_check(SDL_SetTextInputArea(display, &caret, 0));
    if (text_widget == window->widget && text_serial == context->extra &&
        text_type == context->code && text_flags == context->reserved)
        return;
    if (SDL_TextInputActive(display))
        sdl_check(SDL_StopTextInput(display));
    SDL_FlushEvents(SDL_EVENT_TEXT_EDITING, SDL_EVENT_TEXT_INPUT);
    SDL_PropertiesID properties = SDL_CreateProperties();
    sdl_check(properties != 0);
    SDL_TextInputType type = SDL_TEXTINPUT_TYPE_TEXT;
    switch (context->code) {
    case WOS_TEXT_PASSWORD:
        type = SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN;
        break;
    case WOS_TEXT_PASSWORD_VISIBLE:
        type = SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_VISIBLE;
        break;
    case WOS_TEXT_EMAIL:
        type = SDL_TEXTINPUT_TYPE_TEXT_EMAIL;
        break;
    case WOS_TEXT_NUMBER:
        type = SDL_TEXTINPUT_TYPE_NUMBER;
        break;
    }
    SDL_Capitalization capitalization = SDL_CAPITALIZE_NONE;
    if (context->reserved & WOS_TEXT_CAP_LETTERS)
        capitalization = SDL_CAPITALIZE_LETTERS;
    else if (context->reserved & WOS_TEXT_CAP_WORDS)
        capitalization = SDL_CAPITALIZE_WORDS;
    else if (context->reserved & WOS_TEXT_CAP_SENTENCES)
        capitalization = SDL_CAPITALIZE_SENTENCES;
    sdl_check(SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_TYPE_NUMBER, type));
    sdl_check(SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER,
                                   capitalization));
    sdl_check(SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN,
                                    (context->reserved & WOS_TEXT_AUTOCORRECT) != 0));
    sdl_check(SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN,
                                    (context->reserved & WOS_TEXT_MULTILINE) != 0));
    sdl_check(SDL_StartTextInputWithProperties(display, properties));
    SDL_DestroyProperties(properties);
    text_widget = window->widget;
    text_serial = context->extra;
    text_type = context->code;
    text_flags = context->reserved;
}

static void text_event(const char *text, uint32_t type, int start, int length)
{
    struct Window *window = find_window(text_widget, false);
    if (!window || window->ime < 0 || !text)
        return;
    size_t bytes = strlen(text);
    if (bytes > WOS_MAX_TEXT_BYTES || start < -1 ||
        start > WOS_MAX_TEXT_BYTES || length < -1 || length > WOS_MAX_TEXT_BYTES) {
        fprintf(stderr, "Native keyboard text exceeds protocol limits\n");
        return;
    }
    size_t offset = 0;
    do {
        struct WosTextMessage fragment = {
            .magic = WOS_MAGIC, .type = type, .widget = window->widget,
            .serial = text_serial, .start = start, .length = length
        };
        size_t chunk = SDL_min(bytes - offset, sizeof(fragment.text));
        memcpy(fragment.text, text + offset, chunk);
        fragment.flags = (uint32_t)chunk << WOS_TEXT_LENGTH_SHIFT;
        if (!offset)
            fragment.flags |= WOS_TEXT_FIRST;
        offset += chunk;
        if (offset == bytes)
            fragment.flags |= WOS_TEXT_LAST;
        struct WosMessage message;
        memcpy(&message, &fragment, sizeof(message));
        if (wos_send(window->ime, &message, -1) < 0) {
            perror("Native keyboard text forwarding");
            return;
        }
    } while (offset < bytes);
}

static struct Window *input_window(int x, int y, bool hit_test)
{
    struct Window *top = NULL;
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        struct Window *window = &windows[i];
        if (!window->widget || !window->visible || window->control < 0)
            continue;
        if (hit_test && (x < window->x || y < window->y ||
                        x >= window->x + window->width ||
                        y >= window->y + window->height))
            continue;
        if (!top || top->order < window->order)
            top = window;
    }
    return top;
}

static void consume_clients(struct pollfd *descriptors)
{
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        short events = descriptors[i].revents;
        if (clients[i].fd < 0 || descriptors[i].fd != clients[i].fd)
            continue;
        if (events & POLLIN) {
            if (!receive_message(&clients[i]))
                disconnect(&clients[i]);
        } else if (events & (POLLHUP | POLLERR | POLLNVAL)) {
            disconnect(&clients[i]);
        }
    }
}

enum BrowserWaitResult {
    BROWSER_EXITED,
    BROWSER_EXITED_ABNORMALLY,
    BROWSER_TIMED_OUT,
    BROWSER_NOT_CHILD,
    BROWSER_WAIT_FAILED
};

static enum BrowserWaitResult wait_for_browser(pid_t browser, uint64_t timeout_ms,
                                               int listener, uid_t browser_uid)
{
    uint64_t started = SDL_GetTicks();
    for (;;) {
        int child_status;
        pid_t result = waitpid(browser, &child_status, WNOHANG);
        if (result == browser) {
            if (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0)
                return BROWSER_EXITED;
            fprintf(stderr, "Browser exited during shutdown, status=%d\n", child_status);
            return BROWSER_EXITED_ABNORMALLY;
        }
        if (result < 0) {
            if (errno == EINTR)
                continue;
            bool not_child = errno == ECHILD;
            perror("Wait for browser shutdown");
            return not_child ? BROWSER_NOT_CHILD : BROWSER_WAIT_FAILED;
        }
        uint64_t elapsed = SDL_GetTicks() - started;
        if (elapsed >= timeout_ms)
            return BROWSER_TIMED_OUT;
        struct pollfd pending[MAX_CLIENTS + 1] = {
            {.fd = listener, .events = POLLIN}
        };
        for (int i = 0; i < MAX_CLIENTS; ++i)
            pending[i + 1] = (struct pollfd){.fd = clients[i].fd, .events = POLLIN};
        int delay = (int)SDL_min(UINT64_C(10), timeout_ms - elapsed);
        if (poll(pending, MAX_CLIENTS + 1, delay) < 0) {
            if (errno == EINTR)
                continue;
            perror("Display shutdown poll");
            return BROWSER_WAIT_FAILED;
        }
        if ((pending[0].revents & POLLIN) && !accept_client(listener, browser_uid))
            return BROWSER_WAIT_FAILED;
        consume_clients(&pending[1]);
    }
}

static void forward_input(struct WosMessage *message, SDL_Keymod modifiers,
                          bool pointer)
{
    struct Window *window =
        !pointer && text_widget ? find_window(text_widget, false) : NULL;
    if (!window || window->control < 0)
        window = input_window(message->x, message->y, pointer);
    if (!window)
        return;
    message->reserved = ((modifiers & SDL_KMOD_SHIFT) ? WOS_SHIFT : 0) |
                        ((modifiers & SDL_KMOD_CTRL) ? WOS_CONTROL : 0) |
                        ((modifiers & SDL_KMOD_ALT) ? WOS_ALT : 0) |
                        ((modifiers & SDL_KMOD_GUI) ? WOS_META : 0) |
                        ((modifiers & SDL_KMOD_CAPS) ? WOS_CAPS_LOCK : 0) |
                        ((modifiers & SDL_KMOD_NUM) ? WOS_NUM_LOCK : 0);
    message->widget = window->widget;
    if (message->type == WOS_MOUSE_BUTTON) {
        unsigned int index = pointer_count++ % SDL_arraysize(pointer_history);
        pointer_history[index].time = SDL_GetTicks();
        pointer_history[index].widget = window->widget;
        pointer_history[index].x = message->x;
        pointer_history[index].y = message->y;
        pointer_history[index].local_x = message->x - window->x;
        pointer_history[index].local_y = message->y - window->y;
        pointer_history[index].down = message->state != 0;
    }
    message->x -= window->x;
    message->y -= window->y;
    if (wos_send(window->control, message, -1) < 0)
        perror("Display input forwarding");
}

static void check_back_hold(void)
{
    if (back_down && SDL_GetTicks() - back_pressed_at >= BACK_HOLD_MS)
        stopping = 1;
}

static void input_event(SDL_Event *event)
{
    struct WosMessage message = {.magic = WOS_MAGIC};
    SDL_Keymod modifiers = SDL_GetModState();
    bool pointer = false;
    if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) &&
        event->key.scancode == SDL_SCANCODE_AC_BACK) {
        if (!display_focused) {
            back_down = false;
            return;
        }
        if (event->key.down) {
            if (!back_down && !event->key.repeat) {
                back_down = true;
                back_pressed_at = SDL_GetTicks();
            }
        } else if (back_down) {
            back_down = false;
            if (SDL_GetTicks() - back_pressed_at >= BACK_HOLD_MS) {
                stopping = 1;
            } else {
                message.type = WOS_KEY;
                message.code = SDL_SCANCODE_AC_BACK;
                message.state = 1;
                forward_input(&message, event->key.mod, false);
                message.x = message.y = 0;
                message.state = 0;
                forward_input(&message, event->key.mod, false);
            }
        }
        return;
    }
    switch (event->type) {
    case SDL_EVENT_TEXT_INPUT:
        text_event(event->text.text, WOS_TEXT_COMMIT, 0, 0);
        return;
    case SDL_EVENT_TEXT_EDITING:
        text_event(event->edit.text, WOS_TEXT_PREEDIT,
                   event->edit.start, event->edit.length);
        return;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        message.type = WOS_FOCUS;
        message.state = event->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        display_focused = message.state != 0;
        if (!display_focused)
            back_down = false;
        forward_input(&message, modifiers, false);
        update_text_input();
        return;
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        stopping = 1;
        return;
    case SDL_EVENT_MOUSE_MOTION:
        message.type = WOS_MOUSE_MOVE;
        message.x = (int)event->motion.x;
        message.y = (int)event->motion.y;
        pointer = true;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        message.type = WOS_MOUSE_BUTTON;
        message.x = (int)event->button.x;
        message.y = (int)event->button.y;
        message.code = event->button.button;
        message.state = event->button.down;
        pointer = true;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        message.type = WOS_MOUSE_WHEEL;
        message.x = (int)event->wheel.mouse_x;
        message.y = (int)event->wheel.mouse_y;
        message.width = (int)(event->wheel.x * 120);
        message.height = (int)(event->wheel.y * 120);
        if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            message.width = -message.width;
            message.height = -message.height;
        }
        pointer = true;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (event->key.scancode == SDL_SCANCODE_CLEAR) {
            struct Window *window = find_window(text_widget, false);
            if (window && window->ime >= 0 && event->key.down) {
                message.type = WOS_TEXT_CLEAR;
                message.widget = window->widget;
                message.extra = text_serial;
                if (wos_send(window->ime, &message, -1) < 0)
                    perror("Native keyboard clear forwarding");
            }
            return;
        }
        message.type = WOS_KEY;
        message.code = event->key.scancode;
        message.state = event->key.down;
        message.extra = event->key.repeat;
        modifiers = event->key.mod;
        break;
    default:
        return;
    }
    forward_input(&message, modifiers, pointer);
}

static void draw(void)
{
    sdl_check(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255));
    sdl_check(SDL_RenderClear(renderer));
    uint64_t previous = 0;
    for (int count = 0; count < MAX_WINDOWS; ++count) {
        struct Window *next = NULL;
        for (int i = 0; i < MAX_WINDOWS; ++i) {
            struct Window *window = &windows[i];
            if (window->widget && window->visible && window->texture &&
                window->order > previous && (!next || window->order < next->order))
                next = window;
        }
        if (!next)
            break;
        SDL_FRect bounds = {(float)next->x, (float)next->y,
                            (float)next->width, (float)next->height};
        sdl_check(SDL_RenderTexture(renderer, next->texture, NULL, &bounds));
        previous = next->order;
    }
    sdl_check(SDL_RenderPresent(renderer));
    dirty = false;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--probe") == 0) {
        sdl_check(SDL_SetHintWithPriority(SDL_HINT_WEBOS_REGISTER_APP, "0",
                                          SDL_HINT_OVERRIDE));
        sdl_check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS));
        printf("Native display driver initialized: %s\n", SDL_GetCurrentVideoDriver());
        SDL_Quit();
        return EXIT_SUCCESS;
    }
    if (argc < 2 || (argc > 3 && (argc < 5 || strcmp(argv[3], "--") != 0))) {
        fprintf(stderr, "Usage: webos-display PRIVATE_SOCKET_PATH [BROWSER_UID [-- PROGRAM ARGUMENTS...]]\n");
        return EXIT_FAILURE;
    }
    uid_t browser_uid = getuid();
    gid_t browser_gid = getgid();
    if (argc >= 3) {
        char *end;
        errno = 0;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (errno || !*argv[2] || *end || value > (uid_t)-1) {
            fprintf(stderr, "Invalid browser UID\n");
            return EXIT_FAILURE;
        }
        browser_uid = (uid_t)value;
        struct passwd *account = getpwuid(browser_uid);
        if (!account || (browser_uid != getuid() && geteuid() != 0)) {
            fprintf(stderr, "Cannot use requested browser identity\n");
            return EXIT_FAILURE;
        }
        browser_gid = account->pw_gid;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t length = strlen(argv[1]);
    if (length >= sizeof(address.sun_path)) {
        fprintf(stderr, "Display socket path is too long\n");
        return EXIT_FAILURE;
    }
    memcpy(address.sun_path, argv[1], length + 1);
    char parent[sizeof(address.sun_path)];
    memcpy(parent, address.sun_path, length + 1);
    char *separator = strrchr(parent, '/');
    if (!separator || separator == parent) {
        fprintf(stderr, "Display requires a private socket directory\n");
        return EXIT_FAILURE;
    }
    *separator = '\0';
    int parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat directory;
    if (parent_fd < 0 || fstat(parent_fd, &directory) ||
        directory.st_uid != getuid() ||
        (browser_uid == getuid() ? (directory.st_mode & 0077) != 0 :
         (directory.st_mode & 0777) != 0710 || directory.st_gid != browser_gid)) {
        fprintf(stderr, "Display directory has unsafe ownership or permissions\n");
        return EXIT_FAILURE;
    }
    umask(0077);
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address))) {
        perror("Display listener");
        return EXIT_FAILURE;
    }
    if (fchownat(parent_fd, separator + 1, browser_uid, browser_gid,
                  AT_SYMLINK_NOFOLLOW) || listen(listener, MAX_CLIENTS)) {
        perror("Display socket ownership or listen");
        return EXIT_FAILURE;
    }
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGUSR1, request_dump);
    for (int i = 0; i < MAX_CLIENTS; ++i)
        clients[i].fd = -1;
    for (int i = 0; i < MAX_WINDOWS; ++i)
        windows[i].control = windows[i].canvas = windows[i].ime = -1;

    sdl_check(SDL_SetHint(SDL_HINT_WEBOS_REGISTER_APP, "1"));
    sdl_check(SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "1"));
    sdl_check(SDL_SetHintWithPriority(SDL_HINT_RETURN_KEY_HIDES_IME, "0",
                                     SDL_HINT_OVERRIDE));
    sdl_check(SDL_SetAppMetadata("Brave B9", "experimental", "org.webosbrew.brave.b9"));
    sdl_check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS));
    sdl_check(SDL_CreateWindowAndRenderer("Brave", DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                         SDL_WINDOW_FULLSCREEN, &display, &renderer));
    sdl_check(SDL_SetRenderLogicalPresentation(renderer, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                              SDL_LOGICAL_PRESENTATION_LETTERBOX));
    dirty = true;
    fprintf(stderr, "Native display ready\n");
    int status = EXIT_SUCCESS;
    pid_t browser = -1;
    if (argc >= 5) {
        browser = fork();
        if (browser < 0) {
            perror("Start browser");
            status = EXIT_FAILURE;
            stopping = 1;
        } else if (browser == 0) {
            close(listener);
            close(parent_fd);
            execv(argv[4], &argv[4]);
            perror("Execute browser launcher");
            _exit(127);
        }
    }
    while (!stopping) {
        uint64_t iteration_started = SDL_GetTicks();
        struct pollfd descriptors[MAX_CLIENTS + 1] = {{.fd = listener, .events = POLLIN}};
        for (int i = 0; i < MAX_CLIENTS; ++i)
            descriptors[i + 1] = (struct pollfd){.fd = clients[i].fd, .events = POLLIN};
        if (poll(descriptors, MAX_CLIENTS + 1, 8) < 0 && errno != EINTR) {
            perror("Display poll");
            status = EXIT_FAILURE;
            break;
        }
        if (descriptors[0].revents & POLLIN) {
            if (!accept_client(listener, browser_uid)) {
                status = EXIT_FAILURE;
                break;
            }
        }
        uint64_t clients_started = SDL_GetTicks();
        consume_clients(&descriptors[1]);
        uint64_t input_started = SDL_GetTicks();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            sdl_check(SDL_ConvertEventToRenderCoordinates(renderer, &event));
            input_event(&event);
        }
        check_back_hold();
        uint64_t render_started = SDL_GetTicks();
        if (dirty)
            draw();
        uint64_t finished = SDL_GetTicks();
        if (finished - iteration_started >= 500)
            fprintf(stderr, "Slow display iteration: clients=%llu input=%llu render=%llu ms\n",
                    (unsigned long long)(input_started - clients_started),
                    (unsigned long long)(render_started - input_started),
                    (unsigned long long)(finished - render_started));
        if (dump_requested)
            dump_display_state();
        if (browser > 0) {
            int result;
            pid_t exited = waitpid(browser, &result, WNOHANG);
            if (exited == browser) {
                if (!WIFEXITED(result) || WEXITSTATUS(result) != 0) {
                    fprintf(stderr, "Browser exited abnormally, status=%d\n", result);
                    status = EXIT_FAILURE;
                }
                browser = -1;
                stopping = 1;
            } else if (exited < 0 && errno != EINTR) {
                perror("Wait for browser");
                status = EXIT_FAILURE;
                break;
            }
        }
    }
    dump_display_state();
    stopping = 1;
    if (SDL_TextInputActive(display) && !SDL_StopTextInput(display))
        fprintf(stderr, "Native keyboard shutdown: %s\n", SDL_GetError());
    if (!SDL_HideWindow(display))
        fprintf(stderr, "Native display hide: %s\n", SDL_GetError());
    if (browser > 0) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].role == WOS_WINDOW && clients[i].fd >= 0) {
                struct WosMessage close_request = {
                    .magic = WOS_MAGIC, .type = WOS_CLOSE, .widget = clients[i].widget
                };
                if (wos_send(clients[i].fd, &close_request, -1) < 0)
                    perror("Request browser window close");
            }
        }
        enum BrowserWaitResult result =
            wait_for_browser(browser, 10000, listener, browser_uid);
        if (result == BROWSER_EXITED) {
            fprintf(stderr, "Native browser completed graceful shutdown\n");
            browser = -1;
        } else if (result == BROWSER_NOT_CHILD ||
                   result == BROWSER_EXITED_ABNORMALLY) {
            status = EXIT_FAILURE;
            browser = -1;
        } else {
            status = EXIT_FAILURE;
        }
        if (browser > 0) {
            dump_display_state();
            fprintf(stderr, result == BROWSER_TIMED_OUT ?
                    "Browser did not close after 10 seconds; sending SIGTERM\n" :
                    "Browser shutdown wait failed; sending SIGTERM\n");
            if (kill(browser, SIGTERM) != 0 && errno != ESRCH)
                perror("Stop browser");
            result = wait_for_browser(browser, 2000, listener, browser_uid);
            if (result == BROWSER_EXITED) {
                browser = -1;
            } else if (result == BROWSER_NOT_CHILD ||
                       result == BROWSER_EXITED_ABNORMALLY) {
                status = EXIT_FAILURE;
                browser = -1;
            }
        }
        if (browser > 0) {
            fprintf(stderr, "Terminating unresponsive browser and waiting for its exit\n");
            if (kill(browser, SIGKILL) != 0 && errno != ESRCH)
                perror("Terminate unresponsive browser");
            pid_t result;
            do {
                result = waitpid(browser, NULL, 0);
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
                perror("Reap terminated browser");
                status = EXIT_FAILURE;
            }
        }
    }
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].fd >= 0)
            disconnect(&clients[i]);
    for (int i = 0; i < MAX_WINDOWS; ++i)
        release_buffer(&windows[i]);
    close(listener);
    if (unlinkat(parent_fd, separator + 1, 0) != 0)
        perror("Display socket cleanup");
    close(parent_fd);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(display);
    SDL_Quit();
    if (argc >= 5) {
        char child[sizeof(address.sun_path) + 8];
        snprintf(child, sizeof(child), "%s/frames", parent);
        if (rmdir(child))
            perror("Preserved nonempty browser frame directory");
        snprintf(child, sizeof(child), "%s/user", parent);
        if (rmdir(child))
            perror("Preserved nonempty browser runtime directory");
        if (rmdir(parent))
            perror("Preserved browser runtime parent");
    }
    return status;
}
