#ifndef UI_OZONE_PLATFORM_WEBOS_WEBOS_WINDOW_H_
#define UI_OZONE_PLATFORM_WEBOS_WEBOS_WINDOW_H_

#include "base/files/scoped_file.h"
#include "base/memory/raw_ptr.h"
#include "base/timer/timer.h"
#include "ui/ozone/platform/headless/headless_window.h"
#include "ui/ozone/platform/headless/headless_window_manager.h"
#include "ui/ozone/platform/webos/protocol.h"

namespace ui {

class WebOSWindow;

class WebOSWindowManager : public HeadlessWindowManager {
 public:
  void SetCapture(WebOSWindow* window);
  WebOSWindow* capture_window() const { return capture_window_; }

 private:
  friend class WebOSWindow;
  raw_ptr<WebOSWindow> capture_window_ = nullptr;
};

class WebOSWindow : public HeadlessWindow {
 public:
  WebOSWindow(PlatformWindowDelegate* delegate,
              WebOSWindowManager* manager,
              const gfx::Rect& bounds);
  ~WebOSWindow() override;
  void Show(bool inactive) override;
  void Hide() override;
  void SetCapture() override;
  void ReleaseCapture() override;
  bool HasCapture() const override;
  void Deactivate() override;

 private:
  friend class WebOSWindowManager;
  void Poll();
  void Synchronize();
  void DispatchInput(const WosMessage& message);
  raw_ptr<WebOSWindowManager> manager_;
  base::ScopedFD socket_;
  base::RepeatingTimer timer_;
  gfx::Rect reported_bounds_;
  bool reported_visible_ = false;
  int modifiers_ = 0;
  int buttons_ = 0;
};

}  // namespace ui
#endif
