#ifndef UI_OZONE_PLATFORM_WEBOS_WEBOS_CANVAS_H_
#define UI_OZONE_PLATFORM_WEBOS_WEBOS_CANVAS_H_

#include "base/files/scoped_file.h"
#include "base/memory/raw_ptr.h"
#include "third_party/skia/include/core/SkRefCnt.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/ozone/platform/headless/headless_surface_factory.h"
#include "ui/ozone/public/surface_ozone_canvas.h"

class SkSurface;

namespace ui {

class WebOSCanvas : public SurfaceOzoneCanvas {
 public:
  explicit WebOSCanvas(gfx::AcceleratedWidget widget);
  ~WebOSCanvas() override;
  SkCanvas* GetCanvas() override;
  void ResizeCanvas(const gfx::Size& size, float scale) override;
  void PresentCanvas(const gfx::Rect& damage) override;
  std::unique_ptr<gfx::VSyncProvider> CreateVSyncProvider() override;

 private:
  void ReleaseBuffer();
  gfx::AcceleratedWidget widget_;
  base::ScopedFD socket_;
  base::ScopedFD buffer_;
  raw_ptr<void> pixels_ = nullptr;
  size_t bytes_ = 0;
  sk_sp<SkSurface> surface_;
};

class WebOSSurfaceFactory : public HeadlessSurfaceFactory {
 public:
  WebOSSurfaceFactory();
  std::unique_ptr<SurfaceOzoneCanvas> CreateCanvasForWidget(
      gfx::AcceleratedWidget widget) override;
};

}  // namespace ui
#endif
