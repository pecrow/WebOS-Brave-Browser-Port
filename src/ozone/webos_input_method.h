#ifndef UI_OZONE_PLATFORM_WEBOS_WEBOS_INPUT_METHOD_H_
#define UI_OZONE_PLATFORM_WEBOS_WEBOS_INPUT_METHOD_H_

#include <string>

#include "base/files/scoped_file.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "ui/base/ime/input_method_minimal.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/ozone/platform/webos/protocol.h"

namespace ui {

class WebOSInputMethod : public InputMethodMinimal {
 public:
  WebOSInputMethod(ImeKeyEventDispatcher* dispatcher,
                   gfx::AcceleratedWidget widget);
  ~WebOSInputMethod() override;
  void OnFocus() override;
  void OnBlur() override;
  void OnTextInputTypeChanged(TextInputClient* client) override;
  void OnCaretBoundsChanged(const TextInputClient* client) override;
  void CancelComposition(const TextInputClient* client) override;
  void SetVirtualKeyboardVisibilityIfEnabled(bool should_show) override;
  EventDispatchDetails DispatchKeyEvent(KeyEvent* event) override;

 protected:
  void OnDidChangeFocusedClient(TextInputClient* before,
                                TextInputClient* focused) override;

 private:
  void PublishState(bool new_focus = false, bool reset = false);
  void Poll();
  void ReceiveText(const WosMessage& message);
  const gfx::AcceleratedWidget widget_;
  base::ScopedFD socket_;
  bool focused_ = false;
  bool show_keyboard_ = true;
  WosMessage state_{};
  std::string text_;
  uint32_t pending_type_ = 0;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<WebOSInputMethod> weak_factory_{this};
};

}  // namespace ui
#endif
