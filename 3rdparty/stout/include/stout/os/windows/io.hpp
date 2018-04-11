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

#ifndef __STOUT_OS_WINDOWS_IO_HPP__
#define __STOUT_OS_WINDOWS_IO_HPP__

#include <climits>

#include <stout/result.hpp>
#include <stout/unreachable.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>

namespace os {

namespace internal {

enum class IOType {
    READ,
    WRITE
};


// Function to handle a synchronous Read/WriteFile result, which can happen
// in both the synchronous and asynchronous IO functions.
inline Try<size_t> process_io_result(bool success, size_t size, IOType type) {
  if (success) {
    return size;
  }

  DWORD error = ::GetLastError();
  if (type == IOType::READ &&
      (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF)) {
    // There are two EOF cases for reads:
    //   1) ERROR_BROKEN_PIPE: The write side closed its end and there no data.
    //   2) ERROR_HANDLE_EOF: We hit the EOF for an asynchronous file handle.
    return 0;
  }

  // In any other error case, we have a fatal error.
  return WindowsError(error);
}


// Asynchronous read/write on a overlapped int_fd. Returns `Error` on fatal
// errors, `None()` on a successful pending IO operation or number of bytes
// read/written on a successful IO operation that finished immediately.
//
// NOTE: The type of `overlapped` is `void*` instead of `OVERLAPPED*`, so that
// the caller can use their custom overlapped struct without having to cast
// it. A common practice in Windows Overlapped IO is to create a new overlapped
// struct that extends the `OVERLAPPED` struct with custom data. For more info,
// see https://blogs.msdn.microsoft.com/oldnewthing/20101217-00/?p=11983.
inline Result<size_t> read_write_async(
    const int_fd& fd,
    void* data,
    size_t size,
    void* overlapped,
    IOType type)
{
  CHECK_LE(size, UINT_MAX);
  CHECK(fd.is_overlapped());
  CHECK(type == IOType::READ || type == IOType::WRITE);

  switch (fd.type()) {
    case WindowsFD::Type::HANDLE: {
      DWORD bytes;
      BOOL result;

      if (type == IOType::READ) {
        result = ::ReadFile(
            fd,
            data,
            static_cast<DWORD>(size),
            &bytes,
            reinterpret_cast<OVERLAPPED*>(overlapped));
      } else {
        result = ::WriteFile(
            fd,
            data,
            static_cast<DWORD>(size),
            &bytes,
            reinterpret_cast<OVERLAPPED*>(overlapped));
      }

      if (result == FALSE && ::GetLastError() == ERROR_IO_PENDING) {
        // Asynchronous operation that is pending.
        return None();
      }

      // The operation finished synchronously with success or error.
      return process_io_result(result == TRUE, bytes, type);
    }
    case WindowsFD::Type::SOCKET: {
      // Note that it's okay to allocate this on the stack, since the WinSock
      // providers must copy the WSABUF to their internal buffers. See
      // https://msdn.microsoft.com/en-us/library/windows/desktop/ms741688(v=vs.85).aspx // NOLINT(whitespace/line_length)
      WSABUF buf = {
        static_cast<u_long>(size),
        static_cast<char*>(data)
      };
      DWORD flags = 0;
      DWORD bytes;
      int result;

      if (type == IOType::READ) {
        result = ::WSARecv(
            fd,
            &buf,
            1,
            &bytes,
            &flags,
            reinterpret_cast<WSAOVERLAPPED*>(overlapped),
            nullptr);
      } else {
        // Note that this takes a `DWORD flags` instead of a `LPDWORD flags`.
        result = ::WSASend(
            fd,
            &buf,
            1,
            &bytes,
            flags,
            reinterpret_cast<WSAOVERLAPPED*>(overlapped),
            nullptr);
      }

      if (result != 0 && ::WSAGetLastError() == WSA_IO_PENDING) {
        // Asynchronous operation that is pending.
        return None();
      }

      // The operation finished synchronously with success or error.
      return process_io_result(result == 0, bytes, type);
    }
  }
  UNREACHABLE();
}


// Synchronous reads/write on any int_fd regardless of the overlapped mode.
// Returns -1 on error and number of bytes read on success.
inline ssize_t read_write_sync(
    const int_fd& fd,
    void* data,
    size_t size,
    IOType type)
{
  CHECK_LE(size, UINT_MAX);
  CHECK(type == IOType::READ || type == IOType::WRITE);

  switch (fd.type()) {
    case WindowsFD::Type::HANDLE: {
      if (!fd.is_overlapped()) {
        // Synchronous handle, so a regular `Read/WriteFile` works. We avoid
        // `read_write_async` here because seekable files need the offset
        // for overlapped IO, but we don't have a way to keep track of that.
        DWORD bytes = 0;
        const BOOL success =
          type == IOType::READ ?
          ::ReadFile(fd, data, static_cast<DWORD>(size), &bytes, nullptr) :
          ::WriteFile(fd, data, static_cast<DWORD>(size), &bytes, nullptr);

        Try<size_t> result = process_io_result(success == TRUE, bytes, type);
        return result.isError() ? -1 : static_cast<ssize_t>(result.get());
      }

      // Asynchronous handle, we can use the `read_write_async` function
      // and then wait on the overlapped object for a synchronous read/write.
      // Creating the event is a defensive measure that only matters when you
      // have multiple pending IO. This never happens in Mesos, since other
      // OSes don't support overlapped IO, but it's nice to be safe.
      OVERLAPPED overlapped = {0};
      overlapped.hEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (overlapped.hEvent == nullptr) {
        return -1;
      }

      // This is a documented (!) feature that prevents an IOCP notification.
      // This is another defensive measure to prevent memory corruption if this
      // function is called when the fd is associated with a completion port.
      // See https://msdn.microsoft.com/en-us/library/windows/desktop/aa364986(v=vs.85).aspx // NOLINT(whitespace/line_length)
      overlapped.hEvent = reinterpret_cast<HANDLE>(
          reinterpret_cast<uintptr_t>(overlapped.hEvent) | 1);

      const Result<size_t> bytes_result =
        read_write_async(fd, data, size, &overlapped, type);

      if (bytes_result.isError()) {
        return -1;
      }

      if (bytes_result.isSome()) {
        return bytes_result.get();
      }

      DWORD bytes;
      const BOOL success
        = ::GetOverlappedResult(fd, &overlapped, &bytes, TRUE);

      Try<size_t> result = process_io_result(success == TRUE, bytes, type);
      return result.isError() ? -1 : static_cast<ssize_t>(result.get());
    }
    case WindowsFD::Type::SOCKET: {
      // The regular `socket` function creates overlapped sockets, so regular
      // `recv` and `send` work fine for overlapped and non-overlapped sockets.
      // We avoid `read_write_async` here, because an overlapped call ignores
      // the non-blocking socket setting, so you can't get WSAWOULDBLOCK
      // for UNIX style non-blocking IO.
      return type == IOType::READ ?
        ::recv(fd, static_cast<char*>(data), static_cast<int>(size), 0) :
        ::send(fd, static_cast<const char*>(data), static_cast<int>(size), 0);
    }
  }
  UNREACHABLE();
}

} // namespace internal {

} // namespace os {

#endif // __STOUT_OS_WINDOWS_IO_HPP__
