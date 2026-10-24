#include <memory>

#include "base/files/file_path.h"
#include "ui/display/types/native_display_delegate.h"
#include "ui/events/ozone/layout/keyboard_layout_engine_manager.h"
#include "ui/events/ozone/layout/stub/stub_keyboard_layout_engine.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/ozone/common/bitmap_cursor_factory.h"
#include "ui/ozone/common/stub_client_native_pixmap_factory.h"
#include "ui/ozone/common/stub_overlay_manager.h"
#include "ui/ozone/platform/headless/headless_screen.h"
#include "ui/ozone/platform/headless/headless_window_manager.h"
#include "ui/ozone/platform/headless/ozone_platform_headless.h"
#include "ui/ozone/platform/webos/webos_canvas.h"
#include "ui/ozone/platform/webos/webos_input_method.h"
#include "ui/ozone/platform/webos/webos_window.h"
#include "ui/ozone/public/gpu_platform_support_host.h"
#include "ui/ozone/public/stub_input_controller.h"
#include "ui/ozone/public/system_input_injector.h"
#include "ui/platform_window/platform_window_init_properties.h"

namespace ui {
namespace {

class WebOSEventSource : public PlatformEventSource {};

class OzonePlatformWebOS : public OzonePlatformHeadless {
 public:
  HeadlessWindowManager* GetHeadlessWindowManager() override {
    return windows_.get();
  }
  SurfaceFactoryOzone* GetSurfaceFactoryOzone() override {
    return surfaces_.get();
  }
  OverlayManagerOzone* GetOverlayManager() override { return overlays_.get(); }
  CursorFactory* GetCursorFactory() override { return cursors_.get(); }
  InputController* GetInputController() override { return input_.get(); }
  GpuPlatformSupportHost* GetGpuPlatformSupportHost() override {
    return gpu_host_.get();
  }
  std::unique_ptr<SystemInputInjector> CreateSystemInputInjector() override {
    return nullptr;
  }
  std::unique_ptr<display::NativeDisplayDelegate> CreateNativeDisplayDelegate()
      override {
    return nullptr;
  }
  bool IsWindowCompositingSupported() const override { return true; }
  std::unique_ptr<PlatformWindow> CreatePlatformWindow(
      PlatformWindowDelegate* delegate,
      PlatformWindowInitProperties properties) override {
    return std::make_unique<WebOSWindow>(delegate, windows_.get(),
                                         properties.bounds);
  }
  std::unique_ptr<PlatformScreen> CreateScreen() override {
    return std::make_unique<HeadlessScreen>();
  }
  void InitScreen(PlatformScreen* screen) override {}
  std::unique_ptr<InputMethod> CreateInputMethod(
      ImeKeyEventDispatcher* dispatcher,
      gfx::AcceleratedWidget widget) override {
    return std::make_unique<WebOSInputMethod>(dispatcher, widget);
  }
  bool InitializeUI(const InitParams& params) override {
    windows_ = std::make_unique<WebOSWindowManager>();
    surfaces_ = std::make_unique<WebOSSurfaceFactory>();
    if (!PlatformEventSource::GetInstance()) {
      events_ = std::make_unique<WebOSEventSource>();
    }
    keyboard_ = std::make_unique<StubKeyboardLayoutEngine>();
    KeyboardLayoutEngineManager::SetKeyboardLayoutEngine(keyboard_.get());
    overlays_ = std::make_unique<StubOverlayManager>();
    input_ = std::make_unique<StubInputController>();
    cursors_ = std::make_unique<BitmapCursorFactory>();
    gpu_host_.reset(CreateStubGpuPlatformSupportHost());
    return true;
  }
  void InitializeGPU(const InitParams& params) override {
    if (!surfaces_) {
      surfaces_ = std::make_unique<WebOSSurfaceFactory>();
    }
  }

 private:
  std::unique_ptr<WebOSWindowManager> windows_;
  std::unique_ptr<WebOSSurfaceFactory> surfaces_;
  std::unique_ptr<WebOSEventSource> events_;
  std::unique_ptr<StubKeyboardLayoutEngine> keyboard_;
  std::unique_ptr<StubOverlayManager> overlays_;
  std::unique_ptr<StubInputController> input_;
  std::unique_ptr<BitmapCursorFactory> cursors_;
  std::unique_ptr<GpuPlatformSupportHost> gpu_host_;
};

}  // namespace

OzonePlatform* CreateOzonePlatformWebos() {
  return new OzonePlatformWebOS();
}

gfx::ClientNativePixmapFactory* CreateClientNativePixmapFactoryWebos() {
  return CreateStubClientNativePixmapFactory();
}

}  // namespace ui
