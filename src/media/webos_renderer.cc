#include "media/renderers/webos_renderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/files/scoped_file.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/sequenced_task_runner_helpers.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "media/base/audio_decoder_config.h"
#include "media/base/decoder_buffer.h"
#include "media/base/media_resource.h"
#include "media/base/renderer.h"
#include "media/base/renderer_client.h"
#include "media/base/video_decoder_config.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/formats/mp4/aac.h"
#include "ui/ozone/platform/webos/webos_media_protocol.h"

namespace media {
namespace {

struct Reply {
  bool transport_ok = false;
  WmMessage message{};
};

class Channel {
 public:
  explicit Channel(std::string path) : path_(std::move(path)) {}

  Reply Call(WmMessage message, std::vector<uint8_t> bytes) {
    Reply reply;
    if (!socket_.is_valid()) {
      if (message.type != WM_OPEN)
        return reply;
      if (path_.size() >= sizeof(sockaddr_un::sun_path)) {
        LOG(ERROR) << "WebOS media socket path is too long";
        return reply;
      }
      socket_.reset(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
      if (!socket_.is_valid()) {
        PLOG(ERROR) << "WebOS media socket";
        return reply;
      }
      timeval timeout{3, 0};
      if (setsockopt(socket_.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout)) ||
          setsockopt(socket_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout))) {
        PLOG(ERROR) << "WebOS media socket timeouts";
        socket_.reset();
        return reply;
      }
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::ranges::copy(path_, address.sun_path);
      if (connect(socket_.get(), reinterpret_cast<sockaddr*>(&address),
                  sizeof(address))) {
        PLOG(ERROR) << "WebOS media connect";
        socket_.reset();
        return reply;
      }
    }
    message.size = bytes.size();
    void* payload = nullptr;
    if (wm_send(socket_.get(), &message, bytes.empty() ? nullptr : bytes.data()) != 1 ||
        wm_receive(socket_.get(), &reply.message, &payload) != 1) {
      PLOG(ERROR) << "WebOS media request";
      std::free(payload);
      socket_.reset();
      return reply;
    }
    std::free(payload);
    reply.transport_ok = reply.message.type == WM_REPLY &&
                         reply.message.size == 0 &&
                         reply.message.token_high == message.token_high &&
                         reply.message.token_low == message.token_low;
    if (!reply.transport_ok) {
      LOG(ERROR) << "Invalid WebOS media reply";
      socket_.reset();
    }
    if (message.type == WM_OPEN && reply.message.args[0] != WM_OK)
      socket_.reset();
    return reply;
  }

 private:
  std::string path_;
  base::ScopedFD socket_;
};

class WebOSRenderer final : public Renderer {
 public:
  WebOSRenderer(std::unique_ptr<Renderer> software,
                scoped_refptr<base::SequencedTaskRunner> runner,
                VideoRendererSink* sink,
                MediaLog* media_log,
                std::string path)
      : software_(std::move(software)),
        media_log_(media_log->Clone()),
        runner_(std::move(runner)),
        sink_(sink),
        worker_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE})),
        channel_(new Channel(std::move(path)),
                 base::OnTaskRunnerDeleter(worker_)),
        token_(base::UnguessableToken::Create()) {}

  ~WebOSRenderer() override = default;

  void Initialize(MediaResource* resource,
                  RendererClient* client,
                  PipelineStatusCallback done) override {
    client_ = client;
    streams_[0] = resource->GetFirstStream(DemuxerStream::VIDEO);
    streams_[1] = resource->GetFirstStream(DemuxerStream::AUDIO);
    WmMessage config = Message(WM_OPEN);
    std::vector<uint8_t> extra;
    if (!Configure(&config, &extra)) {
      LOG(WARNING) << "WebOS native media: unsupported stream, retaining software renderer"
                   << " sink=" << (sink_ != nullptr)
                   << " video="
                   << (streams_[0] ? streams_[0]->video_decoder_config().AsHumanReadableString()
                                   : "none")
                   << " audio="
                   << (streams_[1] ? streams_[1]->audio_decoder_config().AsHumanReadableString()
                                   : "none")
                   << " audio_object_type="
                   << (extra.empty() ? -1 : (extra[0] >> 3));
      software_->Initialize(resource, client, std::move(done));
      return;
    }
    config_ = config;
    Request(config, std::move(extra),
            base::BindOnce(&WebOSRenderer::OnOpen, weak_.GetWeakPtr(),
                           resource, std::move(done)));
  }

  void SetCdm(CdmContext* context, CdmAttachedCB done) override {
    if (!native_)
      software_->SetCdm(context, std::move(done));
    else
      runner_->PostTask(FROM_HERE, base::BindOnce(std::move(done), false));
  }

  void SetLatencyHint(std::optional<base::TimeDelta> hint) override {
    if (!native_)
      software_->SetLatencyHint(hint);
  }

  void Flush(base::OnceClosure done) override {
    if (!native_) {
      software_->Flush(std::move(done));
      return;
    }
    weak_.InvalidateWeakPtrs();
    started_ = ready_ = eos_sent_ = false;
    for (auto& state : state_)
      state = StreamState();
    {
      base::AutoLock guard(clock_lock_);
      clock_running_ = false;
    }
    Request(Message(WM_FLUSH), {},
            base::BindOnce(
                [](base::OnceClosure done, Reply reply) {
                  if (!reply.transport_ok || reply.message.args[0] != WM_OK)
                    LOG(ERROR) << "WebOS native media flush failed";
                  std::move(done).Run();
                },
                std::move(done)));
  }

  void StartPlayingFrom(base::TimeDelta time) override {
    if (!native_) {
      software_->StartPlayingFrom(time);
      return;
    }
    started_ = true;
    ready_ = eos_sent_ = false;
    for (size_t i = 0; i < state_.size(); ++i) {
      state_[i] = StreamState();
      state_[i].eos = !streams_[i];
      state_[i].last_pts = time;
    }
    {
      base::AutoLock guard(clock_lock_);
      clock_ = time;
      clock_at_ = base::TimeTicks::Now();
      clock_limit_ = time;
      last_native_time_ = base::TimeDelta::Min();
      clock_running_ = false;
    }
    WmMessage message = Message(WM_START);
    message.time_us = time.InMicroseconds();
    Request(message, {}, base::BindOnce(&WebOSRenderer::OnStarted,
                                       weak_.GetWeakPtr()));
  }

  void SetPlaybackRate(double rate) override {
    if (!native_) {
      software_->SetPlaybackRate(rate);
      return;
    }
    if (!std::isfinite(rate) || rate < 0 || rate > 4) {
      Fail("unsupported playback rate");
      return;
    }
    {
      base::AutoLock guard(clock_lock_);
      clock_ = ClockLocked();
      clock_at_ = base::TimeTicks::Now();
      rate_ = rate;
    }
    WmMessage message = Message(WM_RATE);
    message.args[0] = rate * 1000;
    Request(message, {}, base::BindOnce(&WebOSRenderer::OnControl,
                                       weak_.GetWeakPtr()));
  }

  void SetVolume(float volume) override {
    if (!native_) {
      software_->SetVolume(volume);
      return;
    }
    WmMessage message = Message(WM_VOLUME);
    message.args[0] = std::clamp(volume, 0.0f, 1.0f) * 100;
    Request(message, {}, base::BindOnce(&WebOSRenderer::OnControl,
                                       weak_.GetWeakPtr()));
  }

  base::TimeDelta GetMediaTime() override {
    if (!native_)
      return software_->GetMediaTime();
    base::AutoLock guard(clock_lock_);
    return ClockLocked();
  }

  RendererType GetRendererType() override {
    return native_ ? RendererType::kContentEmbedderDefined
                   : software_->GetRendererType();
  }

 private:
  struct StreamState {
    bool reading = false;
    bool feeding = false;
    bool eos = false;
    scoped_refptr<DecoderBuffer> buffer;
    base::TimeDelta last_pts;
  };

  WmMessage Message(uint32_t type) const {
    WmMessage message{};
    message.magic = WM_MAGIC;
    message.version = WM_VERSION;
    message.type = type;
    message.token_high = token_.GetHighForSerialization();
    message.token_low = token_.GetLowForSerialization();
    return message;
  }

  bool Configure(WmMessage* config, std::vector<uint8_t>* extra) {
    if (!streams_[0] || !sink_)
      return false;
    const auto video = streams_[0]->video_decoder_config();
    if (video.is_encrypted() || video.coded_size().width() > 3840 ||
        video.coded_size().height() > 2160 ||
        (video.codec() != VideoCodec::kH264 && video.codec() != VideoCodec::kVP9))
      return false;
    natural_size_ = video.natural_size();
    config->args[0] = video.codec() == VideoCodec::kH264 ? WM_H264 : WM_VP9;
    config->args[1] = video.coded_size().width();
    config->args[2] = video.coded_size().height();
    config->args[3] = 30;
    config->args[4] = 1;
    config->args[6] = 48000;
    config->args[7] = 2;
    if (streams_[1]) {
      const auto audio = streams_[1]->audio_decoder_config();
      if (audio.is_encrypted() || audio.channels() > 2 ||
          (audio.codec() != AudioCodec::kAAC && audio.codec() != AudioCodec::kOpus))
        return false;
      config->args[5] = audio.codec() == AudioCodec::kAAC ? WM_AAC : WM_OPUS;
      config->args[6] = audio.samples_per_second();
      config->args[7] = audio.channels();
      *extra = audio.extra_data();
      if (audio.codec() == AudioCodec::kAAC) {
        if (!aac_.Parse(*extra, media_log_.get()) || !aac_.fits_in_adts())
          return false;
        size_t header_size = 0;
        const auto header = aac_.CreateAdtsFromEsds({}, &header_size);
        if (header_size != 7 || header.size() != 7 || (header[2] >> 6) != 1)
          return false;
      }
    }
    return true;
  }

  void OnOpen(MediaResource* resource, PipelineStatusCallback done, Reply reply) {
    if (!reply.transport_ok || reply.message.args[0] != WM_OK) {
      LOG(WARNING) << "WebOS native media unavailable; using software renderer";
      software_->Initialize(resource, client_, std::move(done));
      return;
    }
    native_ = true;
    streams_[0]->EnableBitstreamConverter();
    client_->OnVideoNaturalSizeChange(natural_size_);
    client_->OnVideoOpacityChange(false);
    LOG(INFO) << "WebOS native renderer selected codec=" << config_.args[0]
              << " audio=" << config_.args[5] << " size="
              << config_.args[1] << "x" << config_.args[2];
    std::move(done).Run(PIPELINE_OK);
  }

  void OnStarted(Reply reply) {
    if (!Check(reply))
      return;
    ReadMore(0);
    ReadMore(1);
    Poll();
  }

  void Request(WmMessage message,
               std::vector<uint8_t> bytes,
               base::OnceCallback<void(Reply)> callback) {
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&Channel::Call, base::Unretained(channel_.get()),
                       message, std::move(bytes)),
        std::move(callback));
  }

  void ReadMore(size_t index) {
    auto& state = state_[index];
    if (!started_ || failed_ || state.reading || state.feeding || state.eos)
      return;
    if (state.buffer) {
      Feed(index);
      return;
    }
    if (state.last_pts > GetMediaTime() + base::Milliseconds(750))
      return;
    state.reading = true;
    streams_[index]->Read(
        1, base::BindOnce(&WebOSRenderer::OnRead, weak_.GetWeakPtr(), index));
  }

  void OnRead(size_t index,
              DemuxerStream::Status status,
              DemuxerStream::DecoderBufferVector buffers) {
    auto& state = state_[index];
    state.reading = false;
    if (status != DemuxerStream::kOk || buffers.size() != 1) {
      if (status == DemuxerStream::kConfigChanged) {
        if (index == 0)
          client_->OnVideoConfigChange(streams_[index]->video_decoder_config());
        else
          client_->OnAudioConfigChange(streams_[index]->audio_decoder_config());
      }
      Fail("demuxer aborted, failed, or changed configuration");
      return;
    }
    if (buffers[0]->end_of_stream()) {
      state.eos = true;
      MaybeEos();
      return;
    }
    state.buffer = std::move(buffers[0]);
    Feed(index);
  }

  void Feed(size_t index) {
    auto& state = state_[index];
    if (!state.buffer || state.feeding || failed_)
      return;
    if (state.buffer->size() > WM_MAX_PAYLOAD - 7) {
      Fail("encoded packet exceeds the native media limit");
      return;
    }
    std::vector<uint8_t> bytes;
    const base::span<const uint8_t> data(*state.buffer);
    if (index == 1 && config_.args[5] == WM_AAC &&
        !(state.buffer->size() > 1 && data[0] == 0xff && (data[1] & 0xf0) == 0xf0)) {
      size_t header_size = 0;
      const auto packet = aac_.CreateAdtsFromEsds(data, &header_size);
      if (packet.empty()) {
        Fail("AAC packet cannot be represented in ADTS");
        return;
      }
      bytes.assign(packet.begin(), packet.end());
    } else {
      bytes.assign(state.buffer->begin(), state.buffer->end());
    }
    WmMessage message = Message(index == 0 ? WM_VIDEO : WM_AUDIO);
    message.time_us = state.buffer->timestamp().InMicroseconds();
    state.feeding = true;
    Request(message, std::move(bytes),
            base::BindOnce(&WebOSRenderer::OnFed, weak_.GetWeakPtr(), index));
  }

  void OnFed(size_t index, Reply reply) {
    auto& state = state_[index];
    state.feeding = false;
    if (!Check(reply))
      return;
    if (reply.message.args[0] == WM_RETRY)
      return;
    state.last_pts = state.buffer->timestamp();
    {
      base::AutoLock guard(clock_lock_);
      clock_limit_ = std::max(clock_limit_, state.last_pts);
    }
    state.buffer.reset();
    ReadMore(index);
  }

  void MaybeEos() {
    if (!state_[0].eos || !state_[1].eos || eos_sent_ || failed_)
      return;
    eos_sent_ = true;
    Request(Message(WM_EOS), {},
            base::BindOnce(&WebOSRenderer::OnControl, weak_.GetWeakPtr()));
  }

  void Poll() {
    if (!started_ || failed_)
      return;
    Request(Message(WM_STATUS), {},
            base::BindOnce(&WebOSRenderer::OnStatus, weak_.GetWeakPtr()));
  }

  void OnStatus(Reply reply) {
    if (!Check(reply))
      return;
    const int flags = reply.message.args[1];
    if (flags & WM_HAS_FRAME) {
      {
        base::AutoLock guard(clock_lock_);
        const auto native_time = base::Microseconds(reply.message.time_us);
        if (native_time != last_native_time_) {
          clock_ = std::max(ClockLocked(), native_time);
          clock_at_ = base::TimeTicks::Now();
          last_native_time_ = native_time;
        }
        clock_running_ = true;
      }
      if (!ready_) {
        ready_ = true;
        sink_->PaintSingleFrame(
            VideoFrame::CreateVideoHoleFrame(token_, natural_size_, GetMediaTime()));
        client_->OnBufferingStateChange(BUFFERING_HAVE_ENOUGH,
                                        BUFFERING_CHANGE_REASON_UNKNOWN);
      }
    }
    if (flags & WM_ENDED) {
      started_ = false;
      client_->OnEnded();
      return;
    }
    ReadMore(0);
    ReadMore(1);
    runner_->PostDelayedTask(
        FROM_HERE, base::BindOnce(&WebOSRenderer::Poll, weak_.GetWeakPtr()),
        base::Milliseconds(25));
  }

  void OnControl(Reply reply) { Check(reply); }

  bool Check(const Reply& reply) {
    if (failed_)
      return false;
    if (!reply.transport_ok || reply.message.args[0] < WM_OK ||
        reply.message.args[0] == WM_UNSUPPORTED ||
        (reply.message.args[1] & WM_FAILED)) {
      Fail("native pipeline or transport failed");
      return false;
    }
    return true;
  }

  void Fail(const char* reason) {
    if (failed_)
      return;
    failed_ = true;
    {
      base::AutoLock guard(clock_lock_);
      clock_ = ClockLocked();
      clock_running_ = false;
    }
    LOG(ERROR) << "WebOS native media: " << reason;
    client_->OnError(PIPELINE_ERROR_DECODE);
  }

  base::TimeDelta ClockLocked() const {
    if (!clock_running_ || !rate_)
      return clock_;
    return std::min(clock_limit_,
                    clock_ + (base::TimeTicks::Now() - clock_at_) * rate_);
  }

  std::unique_ptr<Renderer> software_;
  std::unique_ptr<MediaLog> media_log_;
  scoped_refptr<base::SequencedTaskRunner> runner_;
  raw_ptr<VideoRendererSink> sink_;
  scoped_refptr<base::SequencedTaskRunner> worker_;
  std::unique_ptr<Channel, base::OnTaskRunnerDeleter> channel_;
  base::UnguessableToken token_;
  raw_ptr<RendererClient> client_ = nullptr;
  std::array<raw_ptr<DemuxerStream>, 2> streams_{};
  std::array<StreamState, 2> state_;
  WmMessage config_{};
  gfx::Size natural_size_;
  std::atomic<bool> native_{false};
  bool started_ = false, ready_ = false, failed_ = false, eos_sent_ = false;
  mp4::AAC aac_;
  base::Lock clock_lock_;
  base::TimeDelta clock_, clock_limit_, last_native_time_;
  base::TimeTicks clock_at_;
  double rate_ = 0;
  bool clock_running_ = false;
  base::WeakPtrFactory<WebOSRenderer> weak_{this};
};

}  // namespace

std::unique_ptr<Renderer> MaybeWrapWebOSRenderer(
    std::unique_ptr<Renderer> software_renderer,
    scoped_refptr<base::SequencedTaskRunner> media_runner,
    VideoRendererSink* video_sink,
    MediaLog* media_log) {
  const char* enabled = std::getenv("BRAVE_WEBOS_NATIVE_MEDIA");
  const char* socket = std::getenv("BRAVE_WEBOS_MEDIA_SOCKET");
  if (!enabled || std::string_view(enabled) != "1" || !socket || !*socket)
    return software_renderer;
  return std::make_unique<WebOSRenderer>(
      std::move(software_renderer), std::move(media_runner), video_sink, media_log,
      socket);
}

}  // namespace media
