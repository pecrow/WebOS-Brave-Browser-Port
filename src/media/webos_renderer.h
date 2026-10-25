#ifndef MEDIA_RENDERERS_WEBOS_RENDERER_H_
#define MEDIA_RENDERERS_WEBOS_RENDERER_H_

#include <memory>

#include "base/memory/scoped_refptr.h"

namespace base {
class SequencedTaskRunner;
}
namespace media {
class MediaLog;
class Renderer;
class VideoRendererSink;

std::unique_ptr<Renderer> MaybeWrapWebOSRenderer(
    std::unique_ptr<Renderer> software_renderer,
    scoped_refptr<base::SequencedTaskRunner> media_runner,
    VideoRendererSink* video_sink,
    MediaLog* media_log);

}  // namespace media
#endif
