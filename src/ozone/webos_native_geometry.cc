#include "ui/ozone/platform/webos/webos_native_geometry.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>

#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "ui/ozone/platform/webos/webos_media_protocol.h"

namespace ui {

bool WebOSNativeMediaEnabled() {
  const char* enabled = std::getenv("BRAVE_WEBOS_NATIVE_MEDIA");
  const char* socket = std::getenv("BRAVE_WEBOS_MEDIA_SOCKET");
  return enabled && std::string_view(enabled) == "1" && socket && *socket;
}

void UpdateWebOSNativeVideoGeometry(const base::UnguessableToken& token,
                                   const gfx::Rect& bounds,
                                   const gfx::Size& viewport) {
  if (!WebOSNativeMediaEnabled() || bounds.IsEmpty() || viewport.IsEmpty())
    return;
  const std::string path =
      std::string(std::getenv("BRAVE_WEBOS_MEDIA_SOCKET")) + ".geometry";
  if (path.size() >= sizeof(sockaddr_un::sun_path)) {
    LOG(ERROR) << "WebOS native media geometry path too long";
    return;
  }
  base::ScopedFD descriptor(socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (!descriptor.is_valid()) {
    PLOG(ERROR) << "WebOS native media geometry socket";
    return;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::ranges::copy(path, address.sun_path);
  WmMessage message{};
  message.magic = WM_MAGIC;
  message.version = WM_VERSION;
  message.type = WM_GEOMETRY;
  message.token_high = token.GetHighForSerialization();
  message.token_low = token.GetLowForSerialization();
  message.args[0] = bounds.x();
  message.args[1] = bounds.y();
  message.args[2] = bounds.width();
  message.args[3] = bounds.height();
  message.args[4] = viewport.width();
  message.args[5] = viewport.height();
  if (sendto(descriptor.get(), &message, sizeof(message),
             MSG_DONTWAIT | MSG_NOSIGNAL,
             reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
      sizeof(message))
    PLOG(ERROR) << "WebOS native media geometry update";
}

}  // namespace ui
