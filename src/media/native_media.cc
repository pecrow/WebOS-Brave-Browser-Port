#include "webos_media_protocol.h"

#include <AcbAPI.h>
#include <StarfishMediaAPIs.h>
#include <glib.h>

#include <cerrno>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

extern int acb_video_data(long, const char *, long *)
    __asm__("AcbAPI_setMediaVideoData");

namespace {

volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
std::atomic<long> active_acb{0};
std::atomic<bool> acb_failed{false};

void acb_event(long acb, long, long event, long app, long play, const char *reply)
{
    std::fprintf(stderr, "native-media: ACB event=%ld app=%ld play=%ld %s\n",
                 event, app, play, reply ? reply : "");
    if (acb == active_acb.load() && event < 0)
        acb_failed.store(true);
}

int LaunchNativeApp()
{
    const char *id = std::getenv("APPID");
    if (!id || !*id || std::strlen(id) > 127 ||
        std::strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789.-") != std::strlen(id)) {
        std::fprintf(stderr, "native-media: native application identity is missing or invalid\n");
        return EXIT_FAILURE;
    }
    const std::string app_id(id);
    const std::string app = "/media/developer/apps/usr/palm/applications/" + app_id;
    const std::string launcher = app + "/launch.sh";
    execl("/bin/sh", "sh", launcher.c_str(), static_cast<char *>(nullptr));
    std::perror("native-media: execute application launcher");
    return EXIT_FAILURE;
}

class Player {
public:
    explicit Player(const std::string &app_id) : app_id_(app_id) {}
    ~Player() { Unload(); }

    int Configure(const WmMessage &message, const void *extra)
    {
        if ((message.args[0] != WM_H264 && message.args[0] != WM_VP9) ||
            message.args[1] < 1 || message.args[1] > 3840 ||
            message.args[2] < 1 || message.args[2] > 2160 ||
            message.args[3] < 1 || message.args[3] > 60 ||
            (message.args[5] != WM_CODEC_NONE && message.args[5] != WM_AAC &&
             message.args[5] != WM_OPUS) ||
            message.args[6] < 8000 || message.args[6] > 96000 ||
            message.args[7] < 1 || message.args[7] > 2 ||
            message.size > 65536)
            return WM_UNSUPPORTED;
        config_ = message;
        extra_.clear();
        if (message.size) {
            const auto *bytes = static_cast<const unsigned char *>(extra);
            extra_.assign(bytes, bytes + message.size);
        }
        if (message.args[5] == WM_OPUS &&
            (extra_.size() < 19 || std::memcmp(extra_.data(), "OpusHead", 8) ||
             extra_[9] != message.args[7] || extra_[18] != 0))
            return WM_UNSUPPORTED;
        geometry_valid_ = geometry_applied_ = false;
        return WM_OK;
    }

    int Start(int64_t time_us)
    {
        Unload();
        if (time_us < 0 || time_us > INT64_MAX / 1000)
            return WM_ERROR;
        {
            std::lock_guard<std::mutex> guard(lock_);
            flags_ = error_ = 0;
            position_us_ = time_us;
            video_info_.clear();
            load_event_ = false;
        }
        video_count_ = audio_count_ = 0;
        sink_attached_ = false;
        play_applied_ = false;
        geometry_applied_ = false;
        std::fprintf(stderr, "native-media: creating ACB binding\n");
        acb_ = AcbAPI_create();
        active_acb.store(acb_);
        acb_failed.store(false);
        if (!acb_ || !AcbAPI_initialize(acb_, PLAYER_TYPE_MSE, app_id_.c_str(), acb_event)) {
            std::fprintf(stderr, "native-media: ACB initialization failed\n");
            Unload();
            return WM_ERROR;
        }
        std::fprintf(stderr, "native-media: creating Starfish player\n");
        api_.reset(new StarfishMediaAPIs(nullptr));
        std::fprintf(stderr, "native-media: binding Starfish media identity\n");
        context_.reset(new Context{this, generation_});
        if (!api_->notifyForeground() ||
            !AcbAPI_setMediaId(acb_, api_->getMediaID())) {
            std::fprintf(stderr, "native-media: foreground/media binding failed\n");
            Unload();
            return WM_ERROR;
        }
        std::ostringstream payload;
        payload << "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{"
                << "\"appId\":\"" << app_id_ << "\",\"queryPosition\":false,"
                << "\"externalStreamingInfo\":{\"contents\":{\"format\":\"RAW\","
                << "\"provider\":\"Chrome\",\"codec\":{\"video\":\""
                << (config_.args[0] == WM_H264 ? "H264" : "VP9") << "\"";
        if (config_.args[5] != WM_CODEC_NONE)
            payload << ",\"audio\":\"" << (config_.args[5] == WM_AAC ? "AAC" : "OPUS") << "\"";
        payload << "},\"esInfo\":{\"pauseAtDecodeTime\":true,\"ptsToDecode\":"
                << time_us * 1000 << ",\"seperatedPTS\":true,\"videoFpsValue\":"
                << config_.args[3] << ",\"videoFpsScale\":1}";
        if (config_.args[5] == WM_AAC)
            payload << ",\"aacInfo\":{\"channels\":" << config_.args[7]
                    << ",\"format\":\"adts\",\"frequency\":"
                    << config_.args[6] / 1000.0 << ",\"profile\":2}";
        if (config_.args[5] == WM_OPUS) {
            gchar *header = g_base64_encode(extra_.data(), extra_.size());
            payload << ",\"opusInfo\":{\"channels\":" << config_.args[7]
                    << ",\"sampleRate\":" << config_.args[6] / 1000.0
                    << ",\"streamHeader\":\"" << header << "\"}";
            g_free(header);
        }
        payload << "},\"restartStreaming\":false,\"audioSync\":true,"
                << "\"streamQualityInfo\":true,\"bufferingCtrInfo\":{"
                << "\"srcBufferLevelAudio\":{\"minimum\":1024,\"maximum\":1048576},"
                << "\"srcBufferLevelVideo\":{\"minimum\":1024,\"maximum\":8388608}}},"
                << "\"transmission\":{\"contentsType\":\"LIVE\"},\"lowDelayMode\":false,"
                << "\"adaptiveStreaming\":{\"audioOnly\":false,\"maxWidth\":"
                << config_.args[1] << ",\"maxHeight\":" << config_.args[2]
                << ",\"maxFrameRate\":60}}}]}";
        if (!api_->Load(payload.str().c_str(), Event, context_.get())) {
            std::fprintf(stderr, "native-media: Load refused\n");
            Unload();
            return WM_ERROR;
        }
        std::fprintf(stderr, "native-media: started codec=%d audio=%d %dx%d at %lldus\n",
                     config_.args[0], config_.args[5], config_.args[1], config_.args[2],
                     static_cast<long long>(time_us));
        return WM_OK;
    }

    int Feed(bool video, int64_t time_us, const void *data, uint32_t size)
    {
        if (!api_ || !size || time_us < -1000000 || time_us > INT64_MAX / 1000)
            return WM_ERROR;
        char payload[256];
        std::snprintf(payload, sizeof(payload),
            "{\"bufferAddr\":\"%p\",\"bufferSize\":%u,\"pts\":%lld,\"esData\":%d}",
            data, size, static_cast<long long>(time_us * 1000), video ? 1 : 2);
        const std::string reply = api_->Feed(payload);
        if (reply.find("Ok") != std::string::npos) {
            if (video)
                ++video_count_;
            else
                ++audio_count_;
            return WM_OK;
        }
        if (reply.find("BufferFull") != std::string::npos ||
            reply.find("Pending") != std::string::npos)
            return WM_RETRY;
        std::fprintf(stderr, "native-media: Feed failed: %s\n", reply.c_str());
        return WM_ERROR;
    }

    int Rate(int rate_milli)
    {
        if (rate_milli < 0 || rate_milli > 4000)
            return WM_UNSUPPORTED;
        rate_ = rate_milli;
        if (!api_ || !sink_attached_)
            return WM_OK;
        bool ok;
        if (!rate_) {
            ok = api_->Pause();
        } else {
            char payload[128];
            std::snprintf(payload, sizeof(payload),
                          "{\"playRate\":%.3f,\"audioOutput\":true}", rate_ / 1000.0);
            ok = api_->SetPlayRate(payload) && api_->Play();
        }
        play_applied_ = ok;
        if (!ok)
            std::fprintf(stderr, "native-media: playback rate %d refused\n", rate_);
        return ok ? WM_OK : WM_ERROR;
    }

    int Volume(int volume)
    {
        if (volume < 0 || volume > 100)
            return WM_UNSUPPORTED;
        volume_ = volume;
        if (!api_)
            return WM_OK;
        char payload[128];
        std::snprintf(payload, sizeof(payload),
                      "{\"volume\":%d,\"duration\":0,\"easeType\":\"Linear\"}", volume_);
        if (!api_->setVolume(payload)) {
            std::fprintf(stderr, "native-media: volume change refused\n");
            return WM_ERROR;
        }
        return WM_OK;
    }

    int Eos()
    {
        if (!api_ || !api_->pushEOS()) {
            std::fprintf(stderr, "native-media: end-of-stream refused\n");
            return WM_ERROR;
        }
        return WM_OK;
    }

    int Geometry(const WmMessage &message)
    {
        const int x = message.args[0], y = message.args[1];
        const int width = message.args[2], height = message.args[3];
        const int viewport_width = message.args[4], viewport_height = message.args[5];
        if (x < 0 || y < 0 || width < 1 || height < 1 ||
            viewport_width < 1 || viewport_width > 1920 ||
            viewport_height < 1 || viewport_height > 1080 ||
            x > viewport_width - width || y > viewport_height - height)
            return WM_UNSUPPORTED;
        if (geometry_applied_ && std::memcmp(geometry_.args, message.args, 6 * sizeof(int32_t)) == 0)
            return WM_OK;
        geometry_ = message;
        geometry_valid_ = true;
        if (!api_ || !acb_ || !sink_attached_)
            return WM_OK;
        if (AcbAPI_setDisplayWindow(acb_,
                x * 1920 / viewport_width, y * 1080 / viewport_height,
                width * 1920 / viewport_width, height * 1080 / viewport_height,
                false, &task_) <= 0) {
            std::fprintf(stderr, "native-media: display geometry refused\n");
            return WM_ERROR;
        }
        geometry_applied_ = true;
        return WM_OK;
    }

    void Update()
    {
        bool loaded;
        std::string info;
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (flags_ & WM_FAILED)
                return;
            loaded = load_event_;
            info.swap(video_info_);
        }
        if (!api_)
            return;
        if (acb_failed.load()) {
            Fail("asynchronous video-sink request failed");
            return;
        }
        if (loaded && !sink_attached_) {
            // This firmware returns false from setSinkType even on successful playback.
            AcbAPI_setSinkType(acb_, SINK_TYPE_MAIN);
            int result = AcbAPI_setState(acb_, APPSTATE_FOREGROUND,
                                         PLAYSTATE_LOADED, &task_);
            std::fprintf(stderr, "native-media: sink state=%d task=%ld\n", result, task_);
            if (result <= 0) {
                Fail("video sink attachment failed");
                return;
            }
            sink_attached_ = true;
            if (Volume(volume_) != WM_OK)
                Fail("initial volume control failed");
        }
        if (!info.empty() && acb_video_data(acb_, info.c_str(), &task_) <= 0)
            Fail("video geometry metadata refused");
        if (geometry_valid_ && !geometry_applied_ && Geometry(geometry_) != WM_OK)
            Fail("video plane positioning failed");
        if (sink_attached_ && !play_applied_) {
            if (Rate(rate_) != WM_OK)
                Fail("playback control failed");
            else if (rate_ && AcbAPI_setState(acb_, APPSTATE_FOREGROUND,
                                             PLAYSTATE_PLAYING, &task_) <= 0)
                Fail("video sink playback failed");
        }
    }

    void Reply(WmMessage *message, int result)
    {
        std::lock_guard<std::mutex> guard(lock_);
        message->type = WM_REPLY;
        message->size = 0;
        message->time_us = position_us_;
        std::memset(message->args, 0, sizeof(message->args));
        message->args[0] = result;
        message->args[1] = flags_;
        message->args[2] = error_;
        message->args[3] = video_count_;
        message->args[4] = audio_count_;
    }

    void Unload()
    {
        {
            std::lock_guard<std::mutex> guard(lock_);
            ++generation_;
            flags_ = 0;
        }
        if (api_) {
            if (!api_->Unload())
                std::fprintf(stderr, "native-media: Unload refused\n");
            api_.reset();
        }
        if (context_)
            retired_.push_back(std::move(context_));
        if (acb_) {
            active_acb.store(0);
            AcbAPI_setState(acb_, APPSTATE_FOREGROUND, PLAYSTATE_UNLOADED, &task_);
            AcbAPI_finalize(acb_);
            AcbAPI_destroy(acb_);
            acb_ = 0;
        }
        std::fprintf(stderr, "native-media: unloaded video=%d audio=%d\n",
                     video_count_, audio_count_);
    }

private:
    struct Context { Player *player; unsigned generation; };

    static void Event(int type, int64_t number, const char *text, void *opaque)
    {
        auto *context = static_cast<Context *>(opaque);
        Player *player = context->player;
        std::lock_guard<std::mutex> guard(player->lock_);
        if (context->generation != player->generation_)
            return;
        switch (type) {
        case PF_EVENT_TYPE_FRAMEREADY:
            player->position_us_ = number / 1000;
            player->flags_ |= WM_HAS_FRAME;
            break;
        case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:
            player->load_event_ = true;
            player->flags_ |= WM_LOADED;
            break;
        case PF_EVENT_TYPE_STR_VIDEO_INFO:
            player->video_info_ = text ? text : "";
            break;
        case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:
            player->flags_ |= WM_ENDED;
            std::fprintf(stderr, "native-media: end of stream\n");
            break;
        case PF_EVENT_TYPE_INT_ERROR:
        case PF_EVENT_TYPE_STR_ERROR:
            player->flags_ |= WM_FAILED;
            player->error_ = static_cast<int>(number);
            std::fprintf(stderr, "native-media: pipeline error %lld %s\n",
                         static_cast<long long>(number), text ? text : "");
            break;
        default:
            break;
        }
    }

    void Fail(const char *reason)
    {
        std::fprintf(stderr, "native-media: %s\n", reason);
        std::lock_guard<std::mutex> guard(lock_);
        flags_ |= WM_FAILED;
        error_ = EIO;
    }

    std::string app_id_;
    WmMessage config_{};
    std::vector<unsigned char> extra_;
    std::unique_ptr<StarfishMediaAPIs> api_;
    std::unique_ptr<Context> context_;
    std::vector<std::unique_ptr<Context>> retired_;
    std::mutex lock_;
    unsigned generation_ = 0;
    int flags_ = 0, error_ = 0;
    int64_t position_us_ = 0;
    int video_count_ = 0, audio_count_ = 0;
    bool load_event_ = false, sink_attached_ = false, play_applied_ = false;
    std::string video_info_;
    int rate_ = 0, volume_ = 100;
    WmMessage geometry_{};
    bool geometry_valid_ = false, geometry_applied_ = false;
    long acb_ = 0, task_ = 0;
};

int listen_socket(const char *path, int type, uid_t uid)
{
    if (std::strlen(path) >= sizeof(sockaddr_un::sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
        chmod(path, 0600) || (geteuid() == 0 && chown(path, uid, -1)) ||
        (type == SOCK_STREAM && listen(fd, 4))) {
        std::perror("native-media: socket setup");
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && argv[1][0] == '{'))
        return LaunchNativeApp();
    if (argc != 4) {
        std::fprintf(stderr, "Usage: webos-media SOCKET_PATH APP_ID BROWSER_UID\n");
        return EXIT_FAILURE;
    }
    std::string app_id(argv[2]);
    if (app_id.empty() || app_id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") !=
                              std::string::npos) {
        std::fprintf(stderr, "native-media: invalid app id\n");
        return EXIT_FAILURE;
    }
    char *end = nullptr;
    unsigned long uid_value = std::strtoul(argv[3], &end, 10);
    if (!argv[3][0] || *end || uid_value > UINT32_MAX) {
        std::fprintf(stderr, "native-media: invalid browser uid\n");
        return EXIT_FAILURE;
    }
    uid_t uid = static_cast<uid_t>(uid_value);
    pid_t parent = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) {
        std::fprintf(stderr, "native-media: cannot supervise parent lifetime\n");
        return EXIT_FAILURE;
    }
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    const std::string geometry_path = std::string(argv[1]) + ".geometry";
    int server = listen_socket(argv[1], SOCK_STREAM, uid);
    int geometry = listen_socket(geometry_path.c_str(), SOCK_DGRAM, uid);
    if (server < 0 || geometry < 0)
        return EXIT_FAILURE;
    Player player(app_id);
    std::vector<pollfd> clients{{server, POLLIN, 0}, {geometry, POLLIN, 0}};
    int owner = -1;
    uint64_t token_high = 0, token_low = 0;
    std::fprintf(stderr, "native-media: ready %s app=%s\n", argv[1], argv[2]);
    while (!stopping) {
        for (int dispatched = 0; dispatched < 32 &&
             g_main_context_iteration(nullptr, FALSE); ++dispatched) {}
        if (poll(clients.data(), clients.size(), 20) < 0) {
            if (errno == EINTR)
                continue;
            std::perror("native-media: poll");
            break;
        }
        player.Update();
        if (clients[0].revents & POLLIN) {
            int fd = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) {
                std::perror("native-media: accept");
            } else {
                ucred credentials{};
                socklen_t length = sizeof(credentials);
                if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) ||
                    (credentials.uid != uid && credentials.uid != 0) || clients.size() >= 6) {
                    std::fprintf(stderr, "native-media: rejected connection\n");
                    close(fd);
                } else {
                    timeval timeout{5, 0};
                    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                    clients.push_back({fd, POLLIN, 0});
                }
            }
        }
        if (clients[1].revents & POLLIN) {
            WmMessage message{};
            ssize_t size = recv(geometry, &message, sizeof(message), 0);
            if (size == sizeof(message) && message.magic == WM_MAGIC &&
                message.version == WM_VERSION && message.type == WM_GEOMETRY &&
                message.token_high == token_high && message.token_low == token_low &&
                owner >= 0) {
                player.Geometry(message);
            } else {
                std::fprintf(stderr, "native-media: rejected geometry packet\n");
            }
        }
        for (size_t i = 2; i < clients.size();) {
            int fd = clients[i].fd;
            bool closed = clients[i].revents & (POLLERR | POLLHUP | POLLNVAL);
            if (!closed && (clients[i].revents & POLLIN)) {
                WmMessage message{};
                void *payload = nullptr;
                if (wm_receive(fd, &message, &payload) != 1) {
                    std::fprintf(stderr, "native-media: client disconnected or invalid packet\n");
                    closed = true;
                } else {
                    int result = WM_ERROR;
                    if (message.type == WM_OPEN && owner < 0) {
                        result = player.Configure(message, payload);
                        if (result == WM_OK) {
                            owner = fd;
                            token_high = message.token_high;
                            token_low = message.token_low;
                        }
                    } else if (owner == fd && message.token_high == token_high &&
                               message.token_low == token_low) {
                        switch (message.type) {
                        case WM_START: result = player.Start(message.time_us); break;
                        case WM_VIDEO: case WM_AUDIO:
                            result = player.Feed(message.type == WM_VIDEO, message.time_us,
                                                 payload, message.size); break;
                        case WM_RATE: result = player.Rate(message.args[0]); break;
                        case WM_VOLUME: result = player.Volume(message.args[0]); break;
                        case WM_STATUS: result = WM_OK; break;
                        case WM_EOS: result = player.Eos(); break;
                        case WM_FLUSH: player.Unload(); result = WM_OK; break;
                        case WM_CLOSE: player.Unload(); result = WM_OK; closed = true; break;
                        default: std::fprintf(stderr, "native-media: invalid command\n"); break;
                        }
                    } else {
                        result = WM_UNSUPPORTED;
                    }
                    player.Reply(&message, result);
                    if (wm_send(fd, &message, nullptr) != 1) {
                        std::perror("native-media: reply");
                        closed = true;
                    }
                }
                std::free(payload);
            }
            if (closed) {
                if (owner == fd) {
                    player.Unload();
                    owner = -1;
                }
                close(fd);
                clients.erase(clients.begin() + i);
            } else {
                ++i;
            }
        }
    }
    player.Unload();
    for (const auto &client : clients)
        close(client.fd);
    unlink(argv[1]);
    unlink(geometry_path.c_str());
    return EXIT_SUCCESS;
}
