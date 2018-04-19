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

#ifndef __STOUT_OS_WINDOWS_DUP_HPP__
#define __STOUT_OS_WINDOWS_DUP_HPP__

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/windows.hpp> // For `WinSock2.h`.

#include <stout/os/int_fd.hpp>

namespace os {

inline Try<int_fd> dup(const int_fd& fd)
{
  switch (fd.type()) {
    case WindowsFD::FD_CRT:
    case WindowsFD::FD_HANDLE: {
      // TODO(andschwa): Replace this with `::DuplicateHandle` after figuring
      // out how to make it compatible with handles to stdin/stdout/stderr, as
      // well as defining sane inheritance semantics.
      int result = ::_dup(fd.crt());
      if (result == -1) {
        return ErrnoError();
      }

      return result;
    }
    case WindowsFD::FD_SOCKET: {
      WSAPROTOCOL_INFOW info;
      const int result =
        ::WSADuplicateSocketW(fd, ::GetCurrentProcessId(), &info);
      if (result != 0) {
        return SocketError();
      }

      return ::WSASocketW(0, 0, 0, &info, 0, 0);
    }
  }
  UNREACHABLE();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_DUP_HPP__
