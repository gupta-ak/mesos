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

#ifndef __STOUT_OS_WINDOWS_OPEN_HPP__
#define __STOUT_OS_WINDOWS_OPEN_HPP__

#include <string>

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp> // For `mode_t`.

#include <stout/os/int_fd.hpp>

#include <stout/internal/windows/longpath.hpp>

// TODO(andschwa): Windows does not support the Linux extension
// O_NONBLOCK, as asynchronous I/O is done through other mechanisms.
// Overlapped I/O will be implemented later.
constexpr int O_NONBLOCK = 0;

// Windows does not support the Linux extension O_SYNC, as buffering
// is done differently.
constexpr int O_SYNC = 0;

// Windows does not support the Linux extension O_CLOEXEC. Instead, by
// default we set all handles to be non-inheritable.
constexpr int O_CLOEXEC = 0;

namespace os {

// TODO(andschwa): Handle specified creation permissions in `mode_t mode`.
inline Try<int_fd> open(const std::string& path, int oflag, mode_t mode = 0)
{
  std::wstring longpath = ::internal::windows::longpath(path);

  // Map the POSIX `oflag` access flags.

  // O_APPEND: Write only appends.
  //
  // NOTE: We choose a `write` flag here because emulating `O_APPEND`
  // requires granting the `FILE_APPEND_DATA` access right, but not
  // the `FILE_WRITE_DATA` access right, which `GENERIC_WRITE` would
  // otherwise grant.
  const DWORD write = (oflag & O_APPEND) ? FILE_APPEND_DATA : GENERIC_WRITE;

  DWORD access;
  if (oflag & O_WRONLY) {
    access = write;
  } else if (oflag & O_RDWR) {
    access = GENERIC_READ | write;
  } else {
    // NOTE: `O_RDONLY` is zero, so it is "always" set and therefore
    // cannot be reliably tested using a bitmask. Thus it's the default.
    access = GENERIC_READ;
  }

  // Map the POSIX `oflag` creation flags.
  DWORD create;
  if (oflag & O_CREAT) { // Create file if it doesn't exist.
    // Fail if file already exists. This is undefined without `O_CREAT`.
    if (oflag & O_EXCL) {
      create = CREATE_NEW;
    }
    // Truncate file if it already exists.
    else if (oflag & O_TRUNC) {
      create = CREATE_ALWAYS;
    }
    // Otherwise, create a new file or open an existing file.
    else {
      create = OPEN_ALWAYS;
    }
  } else { // Only open existing files.
    // Truncate file if it exists, otherwise fail.
    if (oflag & O_TRUNC) {
      create = TRUNCATE_EXISTING;
    }
    // Open file if it exists, otherwise fail.
    else {
      create = OPEN_EXISTING;
    }
  }

  const HANDLE handle = ::CreateFileW(
      longpath.data(),
      access,
      // Share all access so we don't lock the file.
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      // Disable inheritance by default.
      nullptr,
      create,
      FILE_ATTRIBUTE_NORMAL,
      // No template file.
      nullptr);

  if (handle == INVALID_HANDLE_VALUE) {
    return WindowsError();
  }

  // Return an int-like abstraction of the `HANDLE`.
  return int_fd(handle);
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_OPEN_HPP__
