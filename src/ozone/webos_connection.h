#ifndef UI_OZONE_PLATFORM_WEBOS_WEBOS_CONNECTION_H_
#define UI_OZONE_PLATFORM_WEBOS_WEBOS_CONNECTION_H_

#include "base/files/file_path.h"
#include "base/files/scoped_file.h"
#include "ui/ozone/platform/webos/protocol.h"

namespace ui {

base::FilePath WebOSDisplaySocketPath();
base::ScopedFD ConnectWebOSDisplay();
void SendWebOSMessage(int socket,
                      const WosMessage& message,
                      int descriptor = -1);

}  // namespace ui
#endif
