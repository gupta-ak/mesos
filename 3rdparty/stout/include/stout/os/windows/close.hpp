// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_WINDOWS_CLOSE_HPP__
#define __STOUT_OS_WINDOWS_CLOSE_HPP__

#include <errno.h>

#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows/error.hpp>

#include <stout/os/int_fd.hpp>

#include <stout/os/windows/socket.hpp>

namespace os {

inline Try<Nothing> close(const int_fd& fd)
{
  switch (fd.type()) {
    case WindowsFD::Type::HANDLE: {
      if (::CloseHandle(fd) == FALSE) {
        return WindowsError();
      }
      return Nothing();
    }
    case WindowsFD::Type::SOCKET: {
      // NOTE: Since closing an unconnected socket is not an error in POSIX,
      // we simply ignore it here.
      if (::shutdown(fd, SD_BOTH) == SOCKET_ERROR &&
          WSAGetLastError() != WSAENOTCONN) {
        return WindowsSocketError("Failed to shutdown a socket");
      }
      if (::closesocket(fd) == SOCKET_ERROR) {
        return WindowsSocketError("Failed to close a socket");
      }
      return Nothing();
    }
  }
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_CLOSE_HPP__
