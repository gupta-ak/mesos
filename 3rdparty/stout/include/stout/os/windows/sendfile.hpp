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

#ifndef __STOUT_OS_WINDOWS_SENDFILE_HPP__
#define __STOUT_OS_WINDOWS_SENDFILE_HPP__

#include <stout/error.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp> // For `winioctl.h`.

#include <stout/os/int_fd.hpp>

namespace os {

inline Result<size_t> sendfile_async(
    const int_fd& s,
    const int_fd& fd,
    size_t length,
    void* overlapped)
{
  // TransmitFile can only send `INT_MAX - 1` bytes.
  CHECK_LE(length, INT_MAX - 1);

  const BOOL res = ::TransmitFile(
      s,
      fd,
      static_cast<DWORD>(length),
      0,
      reinterpret_cast<OVERLAPPED*>(overlapped),
      nullptr,
      0);

  int error = ::WSAGetLastError();
  if (res == FALSE && (error == WSA_IO_PENDING || error == ERROR_IO_PENDING)) {
    return None();
  }

  if (res == FALSE) {
    return WindowsSocketError(error);
  }

  return length;
}

// Returns the amount of bytes written from the input file
// descriptor to the output socket.
// On error, `Try<ssize_t, SocketError>` contains the error.
inline Try<ssize_t, SocketError> sendfile(
    const int_fd& s, const int_fd& fd, off_t offset, size_t length)
{
  if (offset < 0) {
    return SocketError(WSAEINVAL);
  }

  // NOTE: We convert the `offset` here to avoid potential data loss
  // in the type casting and bitshifting below.
  uint64_t offset_ = offset;

  OVERLAPPED from = {};
  from.Offset = static_cast<DWORD>(offset_);
  from.OffsetHigh = static_cast<DWORD>(offset_ >> 32);

  // Asynchronous handle, we can use the `read_write_async` function
  // and then wait on the overlapped object for a synchronous read/write.
  // Creating the event is a defensive measure that only matters when you
  // have multiple pending IO. This never happens in Mesos, since other
  // OSes don't support overlapped IO, but it's nice to be safe.
  from.hEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (from.hEvent == nullptr) {
    return SocketError(::GetLastError());
  }

  // This is a documented (!) feature that prevents an IOCP notification.
  // This is another defensive measure to prevent memory corruption if this
  // function is called when the fd is associated with a completion port.
  // See https://msdn.microsoft.com/en-us/library/windows/desktop/aa364986(v=vs.85).aspx // NOLINT(whitespace/line_length)
  from.hEvent =
    reinterpret_cast<HANDLE>(reinterpret_cast<uintptr_t>(from.hEvent) | 1);

  Result<size_t> result = sendfile_async(s, fd, length, &from);
  if (result.isError()) {
    return SocketError();
  }

  if (result.isSome()) {
    return result.get();
  }

  DWORD sent = 0;
  DWORD flags = 0;
  if (::WSAGetOverlappedResult(s, &from, &sent, TRUE, &flags) == TRUE) {
    return sent;
  }

  return SocketError();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_SENDFILE_HPP__
