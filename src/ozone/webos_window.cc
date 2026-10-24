#include "ui/ozone/platform/webos/webos_window.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/ozone/platform/webos/webos_connection.h"

namespace ui {

void WebOSWindowManager::SetCapture(WebOSWindow* window) {
  if (capture_window_ == window) {
    return;
  }
  WebOSWindow* previous = capture_window_;
  capture_window_ = window;
  if (previous) {
    previous->buttons_ = 0;
    previous->delegate()->OnLostCapture();
  }
}

WebOSWindow::WebOSWindow(PlatformWindowDelegate* delegate,
                         WebOSWindowManager* manager,
                         const gfx::Rect& bounds)
    : HeadlessWindow(delegate, manager, bounds),
      manager_(manager),
      socket_(ConnectWebOSDisplay()) {
  const int flags = fcntl(socket_.get(), F_GETFL);
  PCHECK(flags >= 0);
  PCHECK(fcntl(socket_.get(), F_SETFL, flags | O_NONBLOCK) == 0);
  Synchronize();
  timer_.Start(FROM_HERE, base::Milliseconds(8),
               base::BindRepeating(&WebOSWindow::Poll, base::Unretained(this)));
}

WebOSWindow::~WebOSWindow() {
  if (HasCapture()) {
    manager_->capture_window_ = nullptr;
  }
}

void WebOSWindow::Show(bool inactive) {
  HeadlessWindow::Show(inactive);
  Synchronize();
  if (!inactive) {
    Activate();
  }
}

void WebOSWindow::Hide() {
  HeadlessWindow::Hide();
  Synchronize();
  ReleaseCapture();
}

void WebOSWindow::SetCapture() {
  manager_->SetCapture(this);
}

void WebOSWindow::ReleaseCapture() {
  if (HasCapture()) {
    manager_->SetCapture(nullptr);
  }
}

bool WebOSWindow::HasCapture() const {
  return manager_->capture_window() == this;
}

void WebOSWindow::Deactivate() {
  const auto alive = GetWeakPtr();
  HeadlessWindow::Deactivate();
  if (alive) {
    ReleaseCapture();
  }
}

void WebOSWindow::Synchronize() {
  const gfx::Rect bounds = GetBoundsInPixels();
  if (bounds.IsEmpty()) {
    return;
  }
  WosMessage message{};
  message.magic = WOS_MAGIC;
  message.type = WOS_WINDOW;
  message.widget = widget();
  message.x = bounds.x();
  message.y = bounds.y();
  message.width = bounds.width();
  message.height = bounds.height();
  message.state = IsVisible();
  SendWebOSMessage(socket_.get(), message);
  reported_bounds_ = bounds;
  reported_visible_ = IsVisible();
}

void WebOSWindow::Poll() {
  if (reported_bounds_ != GetBoundsInPixels() ||
      reported_visible_ != IsVisible()) {
    Synchronize();
  }
  for (int count = 0; count < 64; ++count) {
    WosMessage message{};
    int descriptor = -1;
    const int result = wos_receive(socket_.get(), &message, &descriptor);
    base::ScopedFD unexpected_descriptor(descriptor);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    CHECK_EQ(result, 1) << "Native display connection closed";
    CHECK_EQ(descriptor, -1);
    CHECK_EQ(message.widget, widget());
    const auto alive = GetWeakPtr();
    WebOSWindow* target = this;
    if ((message.type == WOS_MOUSE_MOVE ||
         message.type == WOS_MOUSE_BUTTON ||
         message.type == WOS_MOUSE_WHEEL) && manager_->capture_window()) {
      target = manager_->capture_window();
    }
    if (target != this) {
      const gfx::Vector2d offset = GetBoundsInPixels().origin() -
                                  target->GetBoundsInPixels().origin();
      message.x += offset.x();
      message.y += offset.y();
      message.widget = target->widget();
    }
    target->DispatchInput(message);
    if (!alive) {
      return;
    }
  }
}

void WebOSWindow::DispatchInput(const WosMessage& message) {
  modifiers_ = ((message.reserved & WOS_SHIFT) ? EF_SHIFT_DOWN : 0) |
               ((message.reserved & WOS_CONTROL) ? EF_CONTROL_DOWN : 0) |
               ((message.reserved & WOS_ALT) ? EF_ALT_DOWN : 0) |
               ((message.reserved & WOS_META) ? EF_COMMAND_DOWN : 0) |
               ((message.reserved & WOS_CAPS_LOCK) ? EF_CAPS_LOCK_ON : 0) |
               ((message.reserved & WOS_NUM_LOCK) ? EF_NUM_LOCK_ON : 0);
  const auto now = base::TimeTicks::Now();
  const gfx::PointF local(message.x, message.y);
  const gfx::Rect bounds = GetBoundsInPixels();
  const gfx::PointF root(message.x + bounds.x(), message.y + bounds.y());
  if (message.type == WOS_MOUSE_MOVE) {
    MouseEvent event(
        buttons_ ? EventType::kMouseDragged : EventType::kMouseMoved, local,
        root, now, modifiers_ | buttons_, 0);
    delegate()->DispatchEvent(&event);
  } else if (message.type == WOS_MOUSE_BUTTON) {
    int changed = 0;
    switch (message.code) {
      case 1:
        changed = EF_LEFT_MOUSE_BUTTON;
        break;
      case 2:
        changed = EF_MIDDLE_MOUSE_BUTTON;
        break;
      case 3:
        changed = EF_RIGHT_MOUSE_BUTTON;
        break;
      default:
        return;
    }
    if (message.state) {
      buttons_ |= changed;
    }
    const int flags = modifiers_ | buttons_;
    if (!message.state) {
      buttons_ &= ~changed;
    }
    MouseEvent event(
        message.state ? EventType::kMousePressed : EventType::kMouseReleased,
        local, root, now, flags, changed);
    delegate()->DispatchEvent(&event);
  } else if (message.type == WOS_MOUSE_WHEEL) {
    MouseWheelEvent event(gfx::Vector2d(message.width, message.height), local,
                          root, now, modifiers_ | buttons_, 0);
    delegate()->DispatchEvent(&event);
  } else if (message.type == WOS_KEY) {
    DomCode code =
        message.code == 282
            ? DomCode::BROWSER_BACK
            : KeycodeConverter::UsbKeycodeToDomCode(0x00070000 | message.code);
    DomKey key;
    KeyboardCode keyboard_code;
    if (!DomCodeToUsLayoutDomKey(code, modifiers_, &key, &keyboard_code)) {
      LOG(WARNING) << "Unmapped native keyboard scancode " << message.code;
      return;
    }
    const int flags = modifiers_ | (message.extra ? EF_IS_REPEAT : 0);
    KeyEvent event(
        message.state ? EventType::kKeyPressed : EventType::kKeyReleased,
        keyboard_code, code, flags, key, now);
    delegate()->DispatchEvent(&event);
  } else if (message.type == WOS_FOCUS) {
    if (message.state) {
      Activate();
    } else {
      Deactivate();
    }
  } else if (message.type == WOS_CLOSE) {
    delegate()->OnCloseRequest();
  } else {
    LOG(FATAL) << "Unexpected native display input type " << message.type;
  }
}

}  // namespace ui
