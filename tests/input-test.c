#define SDL_GetTicks test_ticks
#define SDL_PollEvent test_poll_event
#define main native_display_main
#include "../src/display/host.c"
#undef main
#undef SDL_GetTicks
#undef SDL_PollEvent

#include <assert.h>
#include <time.h>

static uint64_t ticks;
static int event_polls;
static bool quit_sent;
static bool real_clock;

Uint64 SDLCALL test_ticks(void)
{
    if (real_clock) {
        struct timespec time;
        assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
        return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
    }
    return ticks;
}

bool SDLCALL test_poll_event(SDL_Event *event)
{
    ++event_polls;
    if (event_polls == 3 && !quit_sent) {
        *event = (SDL_Event){.type = SDL_EVENT_QUIT};
        quit_sent = true;
        return true;
    }
    return false;
}

static void empty_socket(int fd)
{
    char byte;
    assert(recv(fd, &byte, 1, MSG_DONTWAIT) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

static struct WosMessage read_message(int fd, uint32_t type)
{
    struct WosMessage message;
    int descriptor;
    assert(wos_receive(fd, &message, &descriptor) == 1);
    assert(descriptor == -1 && message.type == type && message.widget == 42);
    return message;
}

static void back_key(bool down, bool repeat)
{
    SDL_Event event = {0};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = SDL_SCANCODE_AC_BACK;
    event.key.down = down;
    event.key.repeat = repeat;
    input_event(&event);
}

int main(void)
{
    assert(SDL_Init(SDL_INIT_EVENTS));
    int control[2], ime[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, control) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, ime) == 0);
    windows[0] = (struct Window){
        .widget = 42, .order = 1, .control = control[0], .ime = ime[0],
        .canvas = -1, .width = DISPLAY_WIDTH, .height = DISPLAY_HEIGHT,
        .visible = true
    };

    ticks = 1000;
    back_key(true, false);
    assert(back_down && !stopping);
    empty_socket(control[1]);
    ticks = 3999;
    back_key(true, true);
    check_back_hold();
    assert(back_pressed_at == 1000 && !stopping);
    back_key(false, false);
    assert(!back_down && !stopping);
    struct WosMessage message = read_message(control[1], WOS_KEY);
    assert(message.code == SDL_SCANCODE_AC_BACK && message.state == 1);
    message = read_message(control[1], WOS_KEY);
    assert(message.code == SDL_SCANCODE_AC_BACK && message.state == 0);
    empty_socket(control[1]);

    ticks = 5000;
    back_key(true, false);
    ticks = 7999;
    check_back_hold();
    assert(!stopping);
    ticks = 8000;
    check_back_hold();
    assert(stopping);
    empty_socket(control[1]);
    stopping = 0;
    back_key(false, false);
    assert(stopping && !back_down);
    empty_socket(control[1]);

    stopping = 0;
    ticks = 9000;
    back_key(true, false);
    SDL_Event focus = {.type = SDL_EVENT_WINDOW_FOCUS_LOST};
    input_event(&focus);
    message = read_message(control[1], WOS_FOCUS);
    assert(!message.state && !back_down);
    ticks = 13000;
    check_back_hold();
    back_key(false, false);
    assert(!stopping);
    empty_socket(control[1]);
    back_key(true, true);
    back_key(false, false);
    assert(!back_down && !stopping);
    empty_socket(control[1]);

    text_widget = 42;
    text_serial = 7;
    const char sample[] = "123456789012345\xf0\x9f\x98\x80" "end";
    text_event(sample, WOS_TEXT_COMMIT, 0, 0);
    char assembled[sizeof(sample)] = {0};
    size_t offset = 0;
    for (int index = 0; index < 2; ++index) {
        message = read_message(ime[1], WOS_TEXT_COMMIT);
        struct WosTextMessage fragment;
        memcpy(&fragment, &message, sizeof(fragment));
        assert(fragment.serial == 7);
        assert(!!(fragment.flags & WOS_TEXT_FIRST) == (index == 0));
        assert(!!(fragment.flags & WOS_TEXT_LAST) == (index == 1));
        size_t length = fragment.flags >> WOS_TEXT_LENGTH_SHIFT;
        assert(length <= sizeof(fragment.text));
        assert(offset + length < sizeof(assembled));
        memcpy(assembled + offset, fragment.text, length);
        offset += length;
    }
    assert(strcmp(sample, assembled) == 0);
    empty_socket(ime[1]);

    text_event("", WOS_TEXT_PREEDIT, -1, -1);
    message = read_message(ime[1], WOS_TEXT_PREEDIT);
    struct WosTextMessage fragment;
    memcpy(&fragment, &message, sizeof(fragment));
    assert(fragment.flags == (WOS_TEXT_FIRST | WOS_TEXT_LAST));
    assert(fragment.start == -1 && fragment.length == -1);

    SDL_Event clear = {.type = SDL_EVENT_KEY_DOWN};
    clear.key.scancode = SDL_SCANCODE_CLEAR;
    clear.key.down = true;
    input_event(&clear);
    message = read_message(ime[1], WOS_TEXT_CLEAR);
    assert(message.extra == text_serial);
    clear.type = SDL_EVENT_KEY_UP;
    clear.key.down = false;
    input_event(&clear);
    empty_socket(ime[1]);
    empty_socket(control[1]);
    close(control[0]);
    close(control[1]);
    close(ime[0]);
    close(ime[1]);
    SDL_Quit();
    char directory[] = "/tmp/brave-b9-host-test-XXXXXX";
    assert(mkdtemp(directory));
    char socket_path[sizeof(directory) + 16];
    assert(snprintf(socket_path, sizeof(socket_path), "%s/display.sock",
                    directory) > 0);
    assert(setenv("SDL_VIDEODRIVER", "dummy", 1) == 0);
    assert(setenv("SDL_RENDER_DRIVER", "software", 1) == 0);
    char *arguments[] = {"webos-display", socket_path, NULL};
    stopping = 0;
    text_widget = 0;
    assert(native_display_main(2, arguments) == EXIT_SUCCESS);
    assert(quit_sent && event_polls == 4);
    assert(rmdir(directory) == 0);
    display = NULL;
    real_clock = true;
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        usleep(250000);
        _exit(0);
    }
    int busy[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, busy) == 0);
    clients[0] = (struct Client){.fd = busy[0], .widget = 42,
                                  .role = WOS_TEXT_INPUT};
    windows[0] = (struct Window){.widget = 42, .control = -1, .canvas = -1,
                                  .ime = busy[0]};
    pid_t writer = fork();
    assert(writer >= 0);
    if (!writer) {
        close(busy[0]);
        struct WosMessage state = {
            .magic = WOS_MAGIC, .type = WOS_TEXT_INPUT, .widget = 42
        };
        while (wos_send(busy[1], &state, -1) == 0)
            ;
        _exit(errno == EPIPE || errno == ECONNRESET ? 0 : 1);
    }
    close(busy[1]);
    uint64_t started = test_ticks();
    assert(wait_for_browser(child, 1000, -1, getuid()) == BROWSER_EXITED);
    assert(test_ticks() - started >= 200);
    disconnect(&clients[0]);
    int writer_status;
    assert(waitpid(writer, &writer_status, 0) == writer);
    assert(WIFEXITED(writer_status) && WEXITSTATUS(writer_status) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) {
        sleep(2);
        _exit(0);
    }
    started = test_ticks();
    assert(wait_for_browser(child, 50, -1, getuid()) == BROWSER_TIMED_OUT);
    assert(test_ticks() - started >= 50);
    assert(kill(child, SIGTERM) == 0);
    assert(waitpid(child, NULL, 0) == child);
    char late_directory[] = "/tmp/brave-b9-late-window-XXXXXX";
    assert(mkdtemp(late_directory));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    assert(snprintf(address.sun_path, sizeof(address.sun_path),
                    "%s/display.sock", late_directory) > 0);
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    assert(listener >= 0);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 4) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) {
        close(listener);
        usleep(30000);
        int connection = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        assert(connection >= 0);
        struct timeval timeout = {.tv_sec = 1};
        assert(setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO,
                          &timeout, sizeof(timeout)) == 0);
        assert(connect(connection, (struct sockaddr *)&address, sizeof(address)) == 0);
        struct WosMessage window = {
            .magic = WOS_MAGIC, .type = WOS_WINDOW, .widget = 99,
            .width = 20, .height = 20, .state = 1
        };
        assert(wos_send(connection, &window, -1) == 0);
        int descriptor;
        assert(wos_receive(connection, &window, &descriptor) == 1);
        assert(window.type == WOS_CLOSE && window.widget == 99 && descriptor == -1);
        close(connection);
        _exit(0);
    }
    assert(wait_for_browser(child, 2000, listener, getuid()) == BROWSER_EXITED);
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].fd >= 0)
            disconnect(&clients[i]);
    close(listener);
    assert(unlink(address.sun_path) == 0);
    assert(rmdir(late_directory) == 0);
    puts("Native input tests passed: 2999/3000 ms Back, repeats, focus loss, text, clear.");
    puts("Native display loop stayed running until the explicit quit event.");
    puts("Shutdown grace period survived busy IPC and honored elapsed-time deadlines.");
    puts("Windows arriving during startup shutdown received a close request.");
    return 0;
}
