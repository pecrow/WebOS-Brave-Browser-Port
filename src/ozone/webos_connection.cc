#include "ui/ozone/platform/webos/webos_connection.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/logging.h"

namespace ui {

base::FilePath WebOSDisplaySocketPath() {
  const char* path = std::getenv("BRAVE_WEBOS_DISPLAY_SOCKET");
  CHECK(path && *path) << "BRAVE_WEBOS_DISPLAY_SOCKET is required";
  return base::FilePath(path);
}

base::ScopedFD ConnectWebOSDisplay() {
  const base::FilePath path = WebOSDisplaySocketPath();
  struct sockaddr_un address{};
  address.sun_family = AF_UNIX;
  CHECK_LT(path.value().size(), sizeof(address.sun_path));
  base::span(address.sun_path).copy_prefix_from(base::span(path.value()));

  struct stat directory{};
  PCHECK(lstat(path.DirName().value().c_str(), &directory) == 0);
  CHECK(S_ISDIR(directory.st_mode));
  const bool private_directory =
      directory.st_uid == getuid() && (directory.st_mode & 0077) == 0;
  const bool root_managed_directory = directory.st_uid == 0 &&
                                      directory.st_gid == getgid() &&
                                      (directory.st_mode & 0777) == 0710;
  CHECK(private_directory || root_managed_directory);

  base::ScopedFD socket(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  PCHECK(socket.is_valid());
  struct timeval timeout{.tv_sec = 3, .tv_usec = 0};
  PCHECK(setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                    sizeof(timeout)) == 0);
  PCHECK(setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                    sizeof(timeout)) == 0);
  PCHECK(connect(socket.get(), reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) == 0);
  struct ucred peer{};
  socklen_t size = sizeof(peer);
  PCHECK(getsockopt(socket.get(), SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0);
  CHECK_EQ(peer.uid, root_managed_directory ? 0U : getuid());
  return socket;
}

void SendWebOSMessage(int socket, const WosMessage& message, int descriptor) {
  PCHECK(wos_send(socket, &message, descriptor) == 0);
}

}  // namespace ui
