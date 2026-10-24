#include "ui/ozone/platform/webos/webos_input_method.h"

#include <array>
#include <bit>
#include <cerrno>
#include <fcntl.h>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/base/ime/composition_text.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/base/ime/text_input_flags.h"
#include "ui/events/event.h"
#include "ui/ozone/platform/webos/webos_connection.h"

namespace ui {
namespace {

size_t UTF16Offset(const std::u16string& text, int32_t characters) {
  if (characters < 0) {
    return text.size();
  }
  size_t offset = 0;
  while (offset < text.size() && characters-- > 0) {
    const char16_t first = text[offset++];
    if (first >= 0xd800 && first <= 0xdbff && offset < text.size() &&
        text[offset] >= 0xdc00 && text[offset] <= 0xdfff) {
      ++offset;
    }
  }
  return offset;
}

}  // namespace

WebOSInputMethod::WebOSInputMethod(ImeKeyEventDispatcher* dispatcher,
                                 gfx::AcceleratedWidget widget)
    : InputMethodMinimal(dispatcher),
      widget_(widget),
      socket_(ConnectWebOSDisplay()) {
  const int flags = fcntl(socket_.get(), F_GETFL);
  PCHECK(flags >= 0);
  PCHECK(fcntl(socket_.get(), F_SETFL, flags | O_NONBLOCK) == 0);
  PublishState(true);
  timer_.Start(FROM_HERE, base::Milliseconds(8),
               base::BindRepeating(&WebOSInputMethod::Poll,
                                  base::Unretained(this)));
}

WebOSInputMethod::~WebOSInputMethod() = default;

void WebOSInputMethod::OnFocus() {
  InputMethodMinimal::OnFocus();
  focused_ = true;
  show_keyboard_ = true;
  PublishState();
}

void WebOSInputMethod::OnBlur() {
  focused_ = false;
  PublishState();
  InputMethodMinimal::OnBlur();
}

void WebOSInputMethod::OnDidChangeFocusedClient(TextInputClient* before,
                                              TextInputClient* focused) {
  show_keyboard_ = true;
  PublishState(true);
}

void WebOSInputMethod::OnTextInputTypeChanged(TextInputClient* client) {
  InputMethodMinimal::OnTextInputTypeChanged(client);
  if (client == GetTextInputClient()) {
    PublishState();
  }
}

void WebOSInputMethod::OnCaretBoundsChanged(const TextInputClient* client) {
  if (client == GetTextInputClient()) {
    PublishState();
    NotifyTextInputCaretBoundsChanged(client);
  }
}

void WebOSInputMethod::CancelComposition(const TextInputClient* client) {
  if (client == GetTextInputClient()) {
    text_.clear();
    pending_type_ = 0;
    PublishState(false, true);
  }
}

void WebOSInputMethod::SetVirtualKeyboardVisibilityIfEnabled(bool should_show) {
  show_keyboard_ = should_show;
  PublishState(should_show);
}

EventDispatchDetails WebOSInputMethod::DispatchKeyEvent(KeyEvent* event) {
  if (state_.state && event->GetCharacter() >= 0x20 && !event->IsControlDown() &&
      !event->IsAltDown() && !event->IsCommandDown()) {
    return DispatchKeyEventPostIME(event);
  }
  return InputMethodMinimal::DispatchKeyEvent(event);
}

void WebOSInputMethod::PublishState(bool new_focus, bool reset) {
  WosMessage message{};
  message.magic = WOS_MAGIC;
  message.type = WOS_TEXT_INPUT;
  message.widget = widget_;
  TextInputClient* client = GetTextInputClient();
  if (focused_ && show_keyboard_ && client &&
      client->GetTextInputType() != TEXT_INPUT_TYPE_NONE) {
    message.state = 1;
    message.code = WOS_TEXT_NORMAL;
    const int flags = client->GetTextInputFlags();
    const TextInputType type = client->GetTextInputType();
    if (type == TEXT_INPUT_TYPE_PASSWORD) {
      message.code = WOS_TEXT_PASSWORD;
    } else if (flags & (TEXT_INPUT_FLAG_HAS_BEEN_PASSWORD |
                        TEXT_INPUT_FLAG_HAS_BEEN_CUSTOM_PASSWORD)) {
      message.code = WOS_TEXT_PASSWORD_VISIBLE;
    } else if (type == TEXT_INPUT_TYPE_EMAIL) {
      message.code = WOS_TEXT_EMAIL;
    } else if (type == TEXT_INPUT_TYPE_NUMBER) {
      message.code = WOS_TEXT_NUMBER;
    }
    if (type == TEXT_INPUT_TYPE_TEXT_AREA ||
        type == TEXT_INPUT_TYPE_CONTENT_EDITABLE) {
      message.reserved |= WOS_TEXT_MULTILINE;
    }
    if (message.code != WOS_TEXT_PASSWORD &&
        message.code != WOS_TEXT_PASSWORD_VISIBLE &&
        type != TEXT_INPUT_TYPE_URL) {
      if (flags & TEXT_INPUT_FLAG_AUTOCORRECT_ON) {
        message.reserved |= WOS_TEXT_AUTOCORRECT;
      }
      if (flags & TEXT_INPUT_FLAG_AUTOCAPITALIZE_CHARACTERS) {
        message.reserved |= WOS_TEXT_CAP_LETTERS;
      } else if (flags & TEXT_INPUT_FLAG_AUTOCAPITALIZE_WORDS) {
        message.reserved |= WOS_TEXT_CAP_WORDS;
      } else if (flags & TEXT_INPUT_FLAG_AUTOCAPITALIZE_SENTENCES) {
        message.reserved |= WOS_TEXT_CAP_SENTENCES;
      }
    }
    const gfx::Rect caret = client->GetCaretBounds();
    message.x = caret.x();
    message.y = caret.y();
    message.width = caret.width();
    message.height = caret.height();
  }
  message.extra = state_.extra;
  if (new_focus || message.state != state_.state ||
      message.code != state_.code || message.reserved != state_.reserved) {
    ++message.extra;
    text_.clear();
    pending_type_ = 0;
  }
  state_ = message;
  if (reset) {
    message.reserved |= WOS_TEXT_RESET;
  }
  SendWebOSMessage(socket_.get(), message);
}

void WebOSInputMethod::Poll() {
  for (int count = 0; count < 64; ++count) {
    WosMessage message{};
    int descriptor = -1;
    const int result = wos_receive(socket_.get(), &message, &descriptor);
    base::ScopedFD unexpected_descriptor(descriptor);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    CHECK_EQ(result, 1) << "Native keyboard connection closed";
    CHECK_EQ(descriptor, -1);
    CHECK_EQ(message.widget, widget_);
    const auto alive = weak_factory_.GetWeakPtr();
    ReceiveText(message);
    if (!alive) {
      return;
    }
  }
}

void WebOSInputMethod::ReceiveText(const WosMessage& message) {
  TextInputClient* client = GetTextInputClient();
  if (!client || !state_.state) {
    return;
  }
  if (message.type == WOS_TEXT_CLEAR) {
    if (message.extra != state_.extra) {
      return;
    }
    const auto alive = weak_factory_.GetWeakPtr();
    const uint32_t serial = state_.extra;
    std::array<KeyEvent, 4> events = {
        KeyEvent(EventType::kKeyPressed, VKEY_A, EF_CONTROL_DOWN),
        KeyEvent(EventType::kKeyReleased, VKEY_A, EF_CONTROL_DOWN),
        KeyEvent(EventType::kKeyPressed, VKEY_BACK, EF_NONE),
        KeyEvent(EventType::kKeyReleased, VKEY_BACK, EF_NONE)};
    for (auto& event : events) {
      InputMethodMinimal::DispatchKeyEvent(&event);
      if (!alive || GetTextInputClient() != client || state_.extra != serial) {
        return;
      }
    }
    return;
  }
  CHECK(message.type == WOS_TEXT_COMMIT || message.type == WOS_TEXT_PREEDIT);
  const auto fragment = std::bit_cast<WosTextMessage>(message);
  if (fragment.serial != state_.extra) {
    return;
  }
  const size_t length = fragment.flags >> WOS_TEXT_LENGTH_SHIFT;
  CHECK_LE(length, sizeof(fragment.text));
  CHECK_GE(fragment.start, -1);
  CHECK_LE(fragment.start, WOS_MAX_TEXT_BYTES);
  CHECK_GE(fragment.length, -1);
  CHECK_LE(fragment.length, WOS_MAX_TEXT_BYTES);
  if (fragment.flags & WOS_TEXT_FIRST) {
    text_.clear();
    pending_type_ = fragment.type;
  }
  if (pending_type_ != fragment.type ||
      text_.size() + length > WOS_MAX_TEXT_BYTES) {
    LOG(ERROR) << "Invalid native keyboard text sequence";
    text_.clear();
    pending_type_ = 0;
    return;
  }
  const auto chunk = base::span(fragment.text).first(length);
  text_.append(chunk.begin(), chunk.end());
  if (!(fragment.flags & WOS_TEXT_LAST)) {
    return;
  }
  pending_type_ = 0;
  if (!base::IsStringUTF8(text_)) {
    LOG(ERROR) << "Native keyboard supplied invalid UTF-8";
    text_.clear();
    return;
  }
  std::u16string text = base::UTF8ToUTF16(text_);
  text_.clear();
  if (fragment.type == WOS_TEXT_COMMIT) {
    if (!text.empty()) {
      client->InsertText(text, TextInputClient::InsertTextCursorBehavior::
                                  kMoveCursorAfterText);
    }
  } else if (text.empty()) {
    if (client->HasCompositionText()) {
      client->ClearCompositionText();
    }
  } else {
    CompositionText composition;
    composition.text = std::move(text);
    const size_t start = UTF16Offset(composition.text, fragment.start);
    const size_t end =
        fragment.length < 0
            ? start
            : UTF16Offset(composition.text, fragment.start + fragment.length);
    composition.selection = gfx::Range(start, end);
    client->SetCompositionText(composition);
  }
}

}  // namespace ui
