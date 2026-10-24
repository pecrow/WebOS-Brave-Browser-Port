#include "ui/ozone/platform/webos/webos_canvas.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/vsync_provider.h"
#include "ui/ozone/platform/webos/webos_connection.h"

namespace ui {

WebOSCanvas::WebOSCanvas(gfx::AcceleratedWidget widget)
    : widget_(widget), socket_(ConnectWebOSDisplay()) {}

WebOSCanvas::~WebOSCanvas() {
  ReleaseBuffer();
}

void WebOSCanvas::ReleaseBuffer() {
  surface_.reset();
  if (pixels_) {
    PCHECK(munmap(pixels_, bytes_) == 0);
    pixels_ = nullptr;
    bytes_ = 0;
  }
  buffer_.reset();
}

SkCanvas* WebOSCanvas::GetCanvas() {
  CHECK(surface_);
  return surface_->getCanvas();
}

void WebOSCanvas::ResizeCanvas(const gfx::Size& size, float scale) {
  CHECK_GT(size.width(), 0);
  CHECK_GT(size.height(), 0);
  CHECK_LE(size.width(), WOS_MAX_WIDTH);
  CHECK_LE(size.height(), WOS_MAX_HEIGHT);
  ReleaseBuffer();
  bytes_ = static_cast<size_t>(size.width()) * size.height() * 4;

  const auto directory = WebOSDisplaySocketPath().DirName().Append("frames");
  const std::string name = directory.Append("frame-XXXXXX").value();
  std::vector<char> temporary(name.begin(), name.end());
  temporary.push_back('\0');
  buffer_.reset(mkostemp(temporary.data(), O_CLOEXEC));
  PCHECK(buffer_.is_valid());
  PCHECK(unlink(temporary.data()) == 0);
  PCHECK(ftruncate(buffer_.get(), bytes_) == 0);
  pixels_ = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED,
                 buffer_.get(), 0);
  PCHECK(pixels_ != MAP_FAILED);
  surface_ = SkSurfaces::WrapPixels(
      SkImageInfo::Make(size.width(), size.height(), kBGRA_8888_SkColorType,
                        kPremul_SkAlphaType),
      pixels_, static_cast<size_t>(size.width()) * 4);
  CHECK(surface_);
  WosMessage message{};
  message.magic = WOS_MAGIC;
  message.type = WOS_BUFFER;
  message.widget = widget_;
  message.width = size.width();
  message.height = size.height();
  SendWebOSMessage(socket_.get(), message, buffer_.get());
}

void WebOSCanvas::PresentCanvas(const gfx::Rect& damage) {
  WosMessage message{};
  message.magic = WOS_MAGIC;
  message.type = WOS_PRESENT;
  message.widget = widget_;
  SendWebOSMessage(socket_.get(), message);
  WosMessage acknowledgment{};
  int descriptor = -1;
  const int result = wos_receive(socket_.get(), &acknowledgment, &descriptor);
  base::ScopedFD unexpected_descriptor(descriptor);
  CHECK_EQ(result, 1) << "Native display stopped responding";
  CHECK_EQ(descriptor, -1);
  CHECK_EQ(acknowledgment.type, WOS_ACK);
  CHECK_EQ(acknowledgment.widget, widget_);
}

std::unique_ptr<gfx::VSyncProvider> WebOSCanvas::CreateVSyncProvider() {
  return nullptr;
}

WebOSSurfaceFactory::WebOSSurfaceFactory()
    : HeadlessSurfaceFactory(base::FilePath()) {}

std::unique_ptr<SurfaceOzoneCanvas> WebOSSurfaceFactory::CreateCanvasForWidget(
    gfx::AcceleratedWidget widget) {
  return std::make_unique<WebOSCanvas>(widget);
}

}  // namespace ui
