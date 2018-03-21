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

#ifndef __STOUT_OS_WINDOWS_FD_HPP__
#define __STOUT_OS_WINDOWS_FD_HPP__

#include <array>
#include <memory>
#include <ostream>

#include <stout/check.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/windows.hpp> // For `WinSock2.h`.

namespace os {

class HandleFD
{
public:
  HandleFD(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}

  bool is_valid() const
  {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  operator HANDLE() const { return handle_; }

private:
  HANDLE handle_;
};


class SocketFD
{
public:
  SocketFD(SOCKET socket = INVALID_SOCKET) : socket_(socket) {}

  // On Windows, libevent's `evutil_socket_t` is set to `intptr_t`.
  SocketFD(intptr_t socket) : socket_(static_cast<SOCKET>(socket)) {}

  bool is_valid() const { return socket_ != INVALID_SOCKET; }

  operator SOCKET() const { return socket_; }

  operator intptr_t() const { return static_cast<intptr_t>(socket_); }

private:
  SOCKET socket_;
};


// The `WindowsFD` class exists to provide an common interface with the POSIX
// file descriptor. While the bare `int` representation of the POSIX file
// descriptor API is undesirable, we rendezvous there in order to maintain the
// existing code in Mesos.
//
// In the platform-agnostic code paths, the `int_fd` type is aliased to
// `WindowsFD`. The `os::*` functions return a type appropriate to the platform,
// which allows us to write code like this:
//
//   Try<int_fd> fd = os::open(...);
//
// The `WindowsFD` constructs off one of:
//   (1) `HANDLE` - from the Win32 API
//   (2) `SOCKET` - from the WinSock API
//
// The `os::*` functions then take an instance of `WindowsFD`, examines
// the state and dispatches to the appropriate API.

class WindowsFD
{
public:
  enum class Type
  {
    HANDLE,
    SOCKET
  };

  WindowsFD() = default;

  // IMPORTANT: The `HANDLE` here is expected to be file handles. Specifically,
  //            `HANDLE`s returned by file API such as `CreateFile`. There are
  //            APIs that return `HANDLE`s with different error values, and
  //            therefore must be handled accordingly. For example, a thread API
  //            such as `CreateThread` returns `NULL` as the error value, rather
  //            than `INVALID_HANDLE_VALUE`.
  // TODO(mpark): Consider adding a second parameter which tells us what the
  //              error values are.
  WindowsFD(HANDLE handle) : type_(Type::HANDLE), handle_(handle) {}

  WindowsFD(SOCKET socket) : type_(Type::SOCKET), socket_(socket) {}

  // On Windows, libevent's `evutil_socket_t` is set to `intptr_t`.
  WindowsFD(intptr_t socket)
    : type_(Type::SOCKET), socket_(static_cast<SOCKET>(socket))
  {}

  WindowsFD(const WindowsFD&) = default;
  WindowsFD(WindowsFD&&) = default;

  ~WindowsFD() = default;

  WindowsFD& operator=(const WindowsFD&) = default;
  WindowsFD& operator=(WindowsFD&&) = default;

  bool is_valid() const
  {
    switch (type()) {
    case Type::HANDLE:
      return handle_.is_valid();
    case Type::SOCKET:
      return socket_.is_valid();
    }
  }

  // NOTE: This allocates a C run-time file descriptor and associates
  // it with the handle. At this point, the `HANDLE` should no longer
  // be closed via `CloseHandle`, but instead close the returned `int`
  // with `_close`. This method should almost never be used, and
  // exists only for compatibility with 3rdparty dependencies.
  int crt() const
  {
    CHECK_EQ(Type::HANDLE,type());
    return ::_open_osfhandle(
        reinterpret_cast<intptr_t>(static_cast<HANDLE>(handle_)), O_RDWR);
  }

  // NOTE: This conversion is provided only for checking validity.
  // TODO(andschwa): Fix all uses of this conversion to use `is_valid`
  // directly instead, then remove the comparison operators. This
  // would require writing an `int_fd` class for POSIX too, instead of
  // just using `int`.
  operator int() const
  {
    if (is_valid()) {
      return 0;
    } else {
      return -1;
    }
  }

  operator HANDLE() const
  {
    CHECK_EQ(Type::HANDLE, type());
    return handle_;
  }

  operator SOCKET() const
  {
    CHECK_EQ(Type::SOCKET, type());
    return socket_;
  }

  operator intptr_t() const
  {
    CHECK_EQ(Type::SOCKET, type());
    return static_cast<intptr_t>(socket_);
  }

  Type type() const { return type_; }

private:
  Type type_;

  union
  {
    HandleFD handle_;
    SocketFD socket_;
  };
};


inline std::ostream& operator<<(std::ostream& stream, const WindowsFD::Type& fd)
{
  switch (fd) {
    case WindowsFD::Type::HANDLE: {
      stream << "WindowsFD::Type::HANDLE";
      break;
    }
    case WindowsFD::Type::SOCKET: {
      stream << "WindowsFD::Type::SOCKET";
      break;
    }
  }
  return stream;
}


inline std::ostream& operator<<(std::ostream& stream, const WindowsFD& fd)
{
  switch (fd.type()) {
    case WindowsFD::Type::HANDLE: {
      stream << static_cast<HANDLE>(fd);
      break;
    }
    case WindowsFD::Type::SOCKET: {
      stream << static_cast<SOCKET>(fd);
      break;
    }
  }
  return stream;
}


inline bool operator<(const WindowsFD& left, const WindowsFD& right)
{
  CHECK_EQ(left.type(), right.type());

  switch (left.type()) {
    case WindowsFD::Type::HANDLE: {
      return static_cast<HANDLE>(left) < static_cast<HANDLE>(right);
    }
    case WindowsFD::Type::SOCKET: {
      return static_cast<SOCKET>(left) < static_cast<SOCKET>(right);
    }
  }
}


inline bool operator<(int left, const WindowsFD& right)
{
  return left < static_cast<int>(right);
}


inline bool operator<(const WindowsFD& left, int right)
{
  return static_cast<int>(left) < right;
}


inline bool operator>(const WindowsFD& left, const WindowsFD& right)
{
  return right < left;
}


inline bool operator>(int left, const WindowsFD& right)
{
  return left > static_cast<int>(right);
}


inline bool operator>(const WindowsFD& left, int right)
{
  return static_cast<int>(left) > right;
}


inline bool operator<=(const WindowsFD& left, const WindowsFD& right)
{
  return !(left > right);
}


inline bool operator<=(int left, const WindowsFD& right)
{
  return left <= static_cast<int>(right);
}


inline bool operator<=(const WindowsFD& left, int right)
{
  return static_cast<int>(left) <= right;
}


inline bool operator>=(const WindowsFD& left, const WindowsFD& right)
{
  return !(left < right);
}


inline bool operator>=(int left, const WindowsFD& right)
{
  return left >= static_cast<int>(right);
}


inline bool operator>=(const WindowsFD& left, int right)
{
  return static_cast<int>(left) >= right;
}


inline bool operator==(const WindowsFD& left, const WindowsFD& right)
{
  if (left.type() != right.type()) {
    return false;
  }

  switch (left.type()) {
    case WindowsFD::Type::HANDLE: {
      return static_cast<HANDLE>(left) == static_cast<HANDLE>(right);
    }
    case WindowsFD::Type::SOCKET: {
      return static_cast<SOCKET>(left) == static_cast<SOCKET>(right);
    }
  }
}


inline bool operator==(int left, const WindowsFD& right)
{
  return left == static_cast<int>(right);
}


inline bool operator==(const WindowsFD& left, int right)
{
  return static_cast<int>(left) == right;
}


inline bool operator!=(const WindowsFD& left, const WindowsFD& right)
{
  return !(left == right);
}


inline bool operator!=(int left, const WindowsFD& right)
{
  return left != static_cast<int>(right);
}


inline bool operator!=(const WindowsFD& left, int right)
{
  return static_cast<int>(left) != right;
}

} // namespace os {

namespace std {

template <>
struct hash<os::WindowsFD>
{
  using argument_type = os::WindowsFD;
  using result_type = size_t;

  result_type operator()(const argument_type& fd) const
  {
    switch (fd.type()) {
      case os::WindowsFD::Type::HANDLE: {
        return reinterpret_cast<result_type>(static_cast<HANDLE>(fd));
      }
      case os::WindowsFD::Type::SOCKET: {
        return static_cast<result_type>(static_cast<SOCKET>(fd));
      }
    }
  }
};

} // namespace std {

#endif // __STOUT_OS_WINDOWS_FD_HPP__
