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

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

namespace os {

// Create pipes for interprocess communication. Since the pipes cannot
// be used directly by Posix `read/write' functions they are wrapped
// in file descriptors, a process-local concept.
inline Try<std::array<WindowsFD, 2>> pipe()
{
  // TODO: maybe call the windows RPC server for this?
  std::wstring uuid = wide_stringify(id::UUID::random().toString());
  std::wstring pipe_name = L"\\\\.\\pipe\\mesos-pipe-" + uuid;

  DWORD pipe_flags =
    PIPE_ACCESS_INBOUND |
    FILE_FLAG_FIRST_PIPE_INSTANCE |
    FILE_FLAG_OVERLAPPED;

  HANDLE read_handle =
    ::CreateNamedPipeW(
        pipe_name.c_str(),
        pipe_flags,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,
        0,
        0,
        0,
        NULL);

	if (read_handle == INVALID_HANDLE_VALUE) {
		return WindowsError();
	}

	HANDLE write_handle = 
    ::CreateFileW(
        pipe_name.c_str(),
        GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, 
        NULL);

	if (write_handle == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
		CloseHandle(read_handle);
    return WindowsError(error);
	}

  return std::array<WindowsFD, 2>{read_handle, write_handle};
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_PIPE_HPP__
