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

#include <io.h> // For `_open_osfhandle`.
#include <fcntl.h> // For `O_RDWR`.

#include <array>
#include <memory>
#include <ostream>

#include <stout/check.hpp>
#include <stout/unreachable.hpp>
#include <stout/windows.hpp> // For `WinSock2.h`.

namespace os {

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

  // IMPORTANT: The `HANDLE` here is expected to be file handles. Specifically,
  //            `HANDLE`s returned by file API such as `CreateFile`. There are
  //            APIs that return `HANDLE`s with different error values, and
  //            therefore must be handled accordingly. For example, a thread API
  //            such as `CreateThread` returns `NULL` as the error value, rather
  //            than `INVALID_HANDLE_VALUE`.
  // TODO(mpark): Consider adding a second parameter which tells us what the
  //              error values are.
  WindowsFD(
      HANDLE handle, bool overlapped = false, Option<uintptr_t> id = None())
    : type_(Type::HANDLE),
      handle_(handle),
      overlapped_(overlapped),
      id_(id.getOrElse(reinterpret_cast<uintptr_t>(handle))) {}

  // Note that socket handles should almost always be overlapped. We do provide
  // a way in stout to create non-overlapped sockets, so for completeness, we
  // have a overlapped parameter in the constructor.
  WindowsFD(
      SOCKET socket, bool overlapped = true, Option<uintptr_t> id = None())
    : type_(Type::SOCKET),
      socket_(socket),
      overlapped_(overlapped),
      id_(id.getOrElse(static_cast<uintptr_t>(socket))) {}

  WindowsFD(int crt) : WindowsFD(INVALID_HANDLE_VALUE)
  {
    if (crt == 0) {
      handle_ = ::GetStdHandle(STD_INPUT_HANDLE);
    } else if (crt == 1) {
      handle_ = ::GetStdHandle(STD_OUTPUT_HANDLE);
    } else if (crt == 2) {
      handle_ = ::GetStdHandle(STD_ERROR_HANDLE);
    }
  }

  // On Windows, libevent's `evutil_socket_t` is set to `intptr_t`.
  WindowsFD(intptr_t socket) : WindowsFD(static_cast<SOCKET>(socket)) {}

  WindowsFD(const WindowsFD&) = default;
  WindowsFD(WindowsFD&&) = default;

  // Default construct with invalid handle semantics.
  WindowsFD() : WindowsFD(INVALID_HANDLE_VALUE) {}

  ~WindowsFD() = default;

  WindowsFD& operator=(const WindowsFD&) = default;
  WindowsFD& operator=(WindowsFD&&) = default;

  bool is_valid() const
  {
    switch (type()) {
      case Type::HANDLE:
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
      case Type::SOCKET:
        return socket_ != INVALID_SOCKET;
      default:
        return false;
    }
  }

  // NOTE: This allocates a C run-time file descriptor and associates
  // it with the handle. At this point, the `HANDLE` should no longer
  // be closed via `CloseHandle`, but instead close the returned `int`
  // with `_close`. This method should almost never be used, and
  // exists only for compatibility with 3rdparty dependencies.
  int crt() const
  {
    CHECK_EQ(Type::HANDLE, type());
    return ::_open_osfhandle(reinterpret_cast<intptr_t>(handle_), O_RDWR);
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

  // On Windows, libevent's `evutil_socket_t` is set to `intptr_t`.
  operator intptr_t() const
  {
    CHECK_EQ(Type::SOCKET, type());
    return static_cast<intptr_t>(socket_);
  }

  Type type() const { return type_; }

  bool is_overlapped() const { return overlapped_; }

  uintptr_t id() const { return id_; }

private:
  Type type_;

  union {
    HANDLE handle_;
    SOCKET socket_;
  };

  bool overlapped_;

  // Unique ID for the underlying `FILE_OBJECT` pointed to by the HANDLE or
  // SOCKET. When the handle is duplicated with `DuplicateHandle` or
  // `WSADuplicateSocket`, the `FILE_OBJECT`is still the same, so this id
  // should also stay the same in the new handle. We need this information,
  // because some operations, like associating an HANDLE to an IO completion
  // port work on a per `FILE_OBJECT` basis, so if we associate two handles
  // that point to the same `FILE_OBJECT`, we get an error.
  uintptr_t id_;

  // NOTE: This function is provided only for checking validity, thus
  // it is private. It provides a view of a `WindowsFD` as an `int`.
  //
  // TODO(andschwa): Fix all uses of this conversion to use `is_valid`
  // directly instead, then remove the comparison operators. This
  // would require writing an `int_fd` class for POSIX too, instead of
  // just using `int`.
  int get_valid() const
  {
    if (is_valid()) {
      return 0;
    } else {
      return -1;
    }
  }

  // NOTE: These operators are used soley to support checking a
  // `WindowsFD` against e.g. -1 or 0 for validity. Nothing else
  // should have access to `get_valid()`.
  friend bool operator<(int left, const WindowsFD& right);
  friend bool operator<(const WindowsFD& left, int right);
  friend bool operator>(int left, const WindowsFD& right);
  friend bool operator>(const WindowsFD& left, int right);
  friend bool operator<=(int left, const WindowsFD& right);
  friend bool operator<=(const WindowsFD& left, int right);
  friend bool operator>=(int left, const WindowsFD& right);
  friend bool operator>=(const WindowsFD& left, int right);
  friend bool operator==(int left, const WindowsFD& right);
  friend bool operator==(const WindowsFD& left, int right);
  friend bool operator!=(int left, const WindowsFD& right);
  friend bool operator!=(const WindowsFD& left, int right);
};


inline std::ostream& operator<<(std::ostream& stream, const WindowsFD::Type& fd)
{
  switch (fd) {
    case WindowsFD::Type::HANDLE: {
      stream << "WindowsFD::Type::HANDLE";
      return stream;
    }
    case WindowsFD::Type::SOCKET: {
      stream << "WindowsFD::Type::SOCKET";
      return stream;
    }
    default: {
      stream << "WindowsFD::Type::UNKNOWN";
      return stream;
    }
  }
}


inline std::ostream& operator<<(std::ostream& stream, const WindowsFD& fd)
{
  stream << fd.type() << ": ";
  switch (fd.type()) {
    case WindowsFD::Type::HANDLE: {
      stream << static_cast<HANDLE>(fd);
      return stream;
    }
    case WindowsFD::Type::SOCKET: {
      stream << static_cast<SOCKET>(fd);
      return stream;
    }
    default: {
      stream << "UNKNOWN";
      return stream;
    }
  }
}


// NOTE: The following operators implement all the comparisons
// possible between two `WindowsFD` types or a `WindowsFD` type and an
// `int`. The point of this is that the `WindowsFD` type must act like
// an `int` for compatibility reasons (e.g. checking validity through
// `fd < 0`), without actually being castable to an `int` to avoid
// ambiguous types.
inline bool operator<(const WindowsFD& left, const WindowsFD& right)
{
  // These comparisons must come first because a default-constructed
  // `WindowsFD` is invalid, but has `HANDLE` type, and a `WindowFD`
  // constructed from `-1` is invalid (and has `HANDLE` type), but
  // should be comparable against both `HANDLE` and `SOCKET` types.

  // -1 < -1
  if (!left.is_valid() && !right.is_valid()) {
    return false;
  }

  // -1 < N_0
  if (!left.is_valid() && right.is_valid()) {
    return true;
  }

  // N_0 < -1
  if (left.is_valid() && !right.is_valid()) {
    return false;
  }

  // At this point, both `left` and `right` are valid, so the
  // comparison only makes sense if they are of the same type.
  CHECK_EQ(left.type(), right.type());

  switch (left.type()) {
    case WindowsFD::Type::HANDLE: {
      return static_cast<HANDLE>(left) < static_cast<HANDLE>(right);
    }
    case WindowsFD::Type::SOCKET: {
      return static_cast<SOCKET>(left) < static_cast<SOCKET>(right);
    }
  }

  UNREACHABLE();
}


inline bool operator<(int left, const WindowsFD& right)
{
  return left < right.get_valid();
}


inline bool operator<(const WindowsFD& left, int right)
{
  return left.get_valid() < right;
}


inline bool operator>(const WindowsFD& left, const WindowsFD& right)
{
  return right < left;
}


inline bool operator>(int left, const WindowsFD& right)
{
  return left > right.get_valid();
}


inline bool operator>(const WindowsFD& left, int right)
{
  return left.get_valid() > right;
}


inline bool operator<=(const WindowsFD& left, const WindowsFD& right)
{
  return !(left > right);
}


inline bool operator<=(int left, const WindowsFD& right)
{
  return left <= right.get_valid();
}


inline bool operator<=(const WindowsFD& left, int right)
{
  return left.get_valid() <= right;
}


inline bool operator>=(const WindowsFD& left, const WindowsFD& right)
{
  return !(left < right);
}


inline bool operator>=(int left, const WindowsFD& right)
{
  return left >= right.get_valid();
}


inline bool operator>=(const WindowsFD& left, int right)
{
  return left.get_valid() >= right;
}


inline bool operator==(const WindowsFD& left, const WindowsFD& right)
{
  // This is `true` even if the types mismatch because we want
  // `WindowsFD(-1)` to compare as equivalent to an invalid `HANDLE`
  // or `SOCKET`, even though it is technically of type `HANDLE`.
  if (!left.is_valid() && !right.is_valid()) {
    return true;
  }

  // Otherwise mismatched types are not equivalent.
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

  UNREACHABLE();
}


inline bool operator==(int left, const WindowsFD& right)
{
  return left == right.get_valid();
}


inline bool operator==(const WindowsFD& left, int right)
{
  return left.get_valid() == right;
}


inline bool operator!=(const WindowsFD& left, const WindowsFD& right)
{
  return !(left == right);
}


inline bool operator!=(int left, const WindowsFD& right)
{
  return left != right.get_valid();
}


inline bool operator!=(const WindowsFD& left, int right)
{
  return left.get_valid() != right;
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

    UNREACHABLE();
  }
};

} // namespace std {

#endif // __STOUT_OS_WINDOWS_FD_HPP__
