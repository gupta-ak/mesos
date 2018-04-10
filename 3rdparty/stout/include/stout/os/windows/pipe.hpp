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

#ifndef __STOUT_OS_WINDOWS_PIPE_HPP__
#define __STOUT_OS_WINDOWS_PIPE_HPP__

#include <array>
#include <string>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>
#include <stout/windows.hpp> // For `windows.h`.

#include <stout/os/int_fd.hpp>

namespace os {

// Returns an "anonymous" pipe that can be used for interprocess communication.
// Since anonymous pipes do not support overlapped IO, we emulate it with an
// uniquely named pipe.
//
// NOTE: Passing overlapped pipes to child processes behave weirdly, so we have
// the ability to create non overlapped pipe handles.
inline Try<std::array<int_fd, 2>> pipe(
    bool read_overlapped = true,
    bool write_overlapped = true)
{
  const DWORD read_flags = read_overlapped ? FILE_FLAG_OVERLAPPED : 0;
  const DWORD write_flags = write_overlapped ? FILE_FLAG_OVERLAPPED : 0;
  std::wstring name;
  bool name_found = false;

  // Try at most 10 times to create an unique pipe name.
  for (int i = 0;  i < 10; i++) {
    const std::string random_name =
      "\\\\.\\pipe\\mesos-pipe-" + id::UUID::random().toString();

    if (!os::exists(random_name)) {
      name = wide_stringify(random_name);
      name_found = true;
      break;
    }
  }

  if (!name_found) {
    return Error("Failed to generate unique pipe name for os::pipe");
  }

  // The named pipe name must be at most 256 characters [1]. It doesn't say if
  // it includes the null terminator, so we limit to 255 to be safe. Since the
  // UUID name is fixed length, this is just a sanity check.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa365150(v=vs.85).aspx // NOLINT(whitespace/line_length)
  CHECK_LE(name.size(), 255);

  // Create the named pipe. To avoid the time-of-check vs time-of-use attack,
  // we have the `FILE_FLAG_FIRST_PIPE_INSTANCE` flag to fail if the pipe
  // already exists.
  //
  // TODO(akagup): The buffer size is currently required, since we don't have
  // IOCP yet. When testing IOCP, it should be 0, but after that, we can try
  // restoring the buffer for an optimization.
  const HANDLE read_handle = ::CreateNamedPipeW(
      name.c_str(),
      PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | read_flags,
      PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1,        // Max pipe instances.
      4096,     // Inbound buffer size.
      4096,     // Outbound buffer size.
      0,        // Pipe timeout for connections.
      nullptr); // Security attributes.

  if (read_handle == INVALID_HANDLE_VALUE) {
    return WindowsError();
  }

  const HANDLE write_handle = ::CreateFileW(
      name.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | write_flags,
      nullptr);

  if (write_handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(read_handle);
    return WindowsError(error);
  }

  return std::array<int_fd, 2>{
    WindowsFD(read_handle, read_overlapped),
    WindowsFD(write_handle, write_overlapped)
  };
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_PIPE_HPP__
