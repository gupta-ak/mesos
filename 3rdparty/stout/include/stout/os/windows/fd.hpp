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

#include <fcntl.h> // For `O_RDWR`.
#include <io.h> // For `_open_osfhandle`.

#include <array>
#include <memory>
#include <mutex>
#include <ostream>
#include <type_traits>

#include <stout/check.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
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

  // The `HANDLE` here is expected to be file handles. Specifically,
  // `HANDLE`s returned by file API such as `CreateFile`. There are
  // APIs that return `HANDLE`s with different error values, and
  // therefore must be handled accordingly. For example, a thread API
  // such as `CreateThread` returns `NULL` as the error value, rather
  // than `INVALID_HANDLE_VALUE`.
  //
  // TODO(mpark): Consider adding a second parameter which tells us
  //              what the error values are.
  static_assert(
      std::is_same<HANDLE, void*>::value,
      "Expected `HANDLE` to be of type `void*`.");
  explicit WindowsFD(HANDLE handle, bool overlapped = false)
    : WindowsFD(handle, overlapped, std::make_shared<IOCPHandle>()) {}

  // The `SOCKET` here is expected to be Windows sockets, such as that
  // used by the Windows Sockets 2 library. The only expected error
  // value is `INVALID_SOCKET`.
  //
  // Note that sockets should almost always be overlapped. We do provide
  // a way in stout to create non-overlapped sockets, so for completeness, we
  // have an overlapped parameter in the constructor.
  static_assert(
      std::is_same<SOCKET, unsigned __int64>::value,
      "Expected `SOCKET` to be of type `unsigned __int64`.");
  explicit WindowsFD(SOCKET socket, bool overlapped = true)
    : WindowsFD(socket, overlapped, std::make_shared<IOCPHandle>()) {}

  // On Windows, libevent's `evutil_socket_t` is set to `intptr_t`.
  explicit WindowsFD(intptr_t socket) : WindowsFD(static_cast<SOCKET>(socket))
  {}

  // This constructor is provided in so that the canonical integer
  // file descriptors representing `stdin` (0), `stdout` (1), and
  // `stderr` (2), and the invalid value of -1 can be used.
  //
  // TODO(andschwa): Consider constraining to the range [-1, 2].
  WindowsFD(int crt) : WindowsFD(INVALID_HANDLE_VALUE)
  {
    if (crt == 0) {
      handle_ = ::GetStdHandle(STD_INPUT_HANDLE);
    } else if (crt == 1) {
      handle_ = ::GetStdHandle(STD_OUTPUT_HANDLE);
    } else if (crt == 2) {
      handle_ = ::GetStdHandle(STD_ERROR_HANDLE);
    }
    // All others default to `INVALID_HANDLE_VALUE`.
  }

  // Default construct with invalid handle semantics.
  WindowsFD() : WindowsFD(INVALID_HANDLE_VALUE) {}

  WindowsFD(const WindowsFD&) = default;
  WindowsFD(WindowsFD&&) = default;

  WindowsFD& operator=(const WindowsFD&) = default;
  WindowsFD& operator=(WindowsFD&&) = default;

  ~WindowsFD() = default;

  bool is_valid() const
  {
    switch (type()) {
      case Type::HANDLE: {
        // Remember that both of these values can represent an invalid
        // handle.
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
      }
      case Type::SOCKET: {
        // Only this value is used for an invalid socket.
        return socket_ != INVALID_SOCKET;
      }
      default: {
        return false;
      }
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
    // TODO(andschwa): Consider if we should overwrite `handle_`.
    return ::_open_osfhandle(reinterpret_cast<intptr_t>(handle_), O_RDWR);
  }

  operator HANDLE() const
  {
    if (type() == Type::SOCKET) {
      return reinterpret_cast<HANDLE>(socket_);
    }
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

  // Assigns this `WindowsFD` to an IOCP. Returns `nullptr` is this is the first
  // time that `this` was assigned to an IOCP. If `this` was already assigned,
  // then this function no-ops and returns the assigned IOCP `HANDLE`. We have
  // this function because `CreateIoCompletionPort` returns an error if a
  // `HANDLE` gets assigned to an IOCP `HANDLE` twice, but provides no way to
  // check for that error.
  Try<HANDLE> assign_iocp(HANDLE iocp_handle, ULONG_PTR key) const
  {
    std::lock_guard<std::mutex> lock(iocp_handle_->mutex);
    HANDLE prev_handle = iocp_handle_->handle;
    if (prev_handle == nullptr) {
      // Confusing name, but `::CreateIoCompletionPort` can also assigns
      // a `HANDLE` to an IO completion port.
      if (::CreateIoCompletionPort(*this, iocp_handle, key, 0) == nullptr) {
        return WindowsError();
      }
      iocp_handle_->handle = iocp_handle;
    }
    return prev_handle;
  }

  HANDLE get_iocp() const
  {
    std::lock_guard<std::mutex> lock(iocp_handle_->mutex);
    return iocp_handle_->handle;
  }

  // Helpers to properly construct a new `WindowsFD` class if the underlying
  // `HANDLE` or `SOCKET` was dupicated.
  WindowsFD dup(HANDLE duplicate) const
  {
    CHECK_EQ(Type::HANDLE, type());
    return WindowsFD(duplicate, overlapped_, iocp_handle_);
  }

  WindowsFD dup(SOCKET duplicate) const
  {
    CHECK_EQ(Type::SOCKET, type());
    return WindowsFD(duplicate, overlapped_, iocp_handle_);
  }

private:
  Type type_;

  union
  {
    HANDLE handle_;
    SOCKET socket_;
  };

  bool overlapped_;

  // There can be many `int_fd`copies of the same `HANDLE` and many `HANDLE`
  // can reference the same kernel `FILE_OBJECT`. Since the IOCP affects the
  // underlying `FILE_OBJECT`, we keep a pointer to the IOCP handle so we can
  // update it for all int_fds that refer to the same `FILE_OBJECT`.
  struct IOCPHandle
  {
    std::mutex mutex;
    HANDLE handle = nullptr;
  };

  std::shared_ptr<IOCPHandle> iocp_handle_;

  // Private constructors to help properly implement `WindowsFD::dup()`.
  explicit WindowsFD(
      HANDLE handle,
      bool overlapped,
      const std::shared_ptr<IOCPHandle>& iocp_handle)
    : type_(Type::HANDLE),
      handle_(handle),
      overlapped_(overlapped),
      iocp_handle_(iocp_handle) {}

  explicit WindowsFD(
      SOCKET socket,
      bool overlapped,
      const std::shared_ptr<IOCPHandle>& iocp_handle)
    : type_(Type::SOCKET),
      socket_(socket),
      overlapped_(overlapped),
      iocp_handle_(iocp_handle) {}

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

  // NOTE: These operators are used solely to support checking a
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
  stream << fd.type() << "=";
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
// possible a `WindowsFD` type and an `int`. The point of this is that
// the `WindowsFD` type must act like an `int` for compatibility
// reasons (e.g. checking validity through `fd < 0`), without actually
// being castable to an `int` to avoid ambiguous types.
inline bool operator<(int left, const WindowsFD& right)
{
  return left < right.get_valid();
}


inline bool operator<(const WindowsFD& left, int right)
{
  return left.get_valid() < right;
}


inline bool operator>(int left, const WindowsFD& right)
{
  return left > right.get_valid();
}


inline bool operator>(const WindowsFD& left, int right)
{
  return left.get_valid() > right;
}


inline bool operator<=(int left, const WindowsFD& right)
{
  return left <= right.get_valid();
}


inline bool operator<=(const WindowsFD& left, int right)
{
  return left.get_valid() <= right;
}


inline bool operator>=(int left, const WindowsFD& right)
{
  return left >= right.get_valid();
}


inline bool operator>=(const WindowsFD& left, int right)
{
  return left.get_valid() >= right;
}


inline bool operator==(int left, const WindowsFD& right)
{
  return left == right.get_valid();
}


inline bool operator==(const WindowsFD& left, int right)
{
  return left.get_valid() == right;
}


inline bool operator!=(int left, const WindowsFD& right)
{
  return left != right.get_valid();
}


inline bool operator!=(const WindowsFD& left, int right)
{
  return left.get_valid() != right;
}


// NOTE: This operator exists so that `WindowsFD` can be used in an
// `unordered_map` (and other STL containers requiring equality).
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

} // namespace os {

namespace std {

// NOTE: This specialization exists so that `WindowsFD` can be used in
// an `unordered_map` (and other STL containers requiring a hash).
template <>
struct hash<os::WindowsFD>
{
  using argument_type = os::WindowsFD;
  using result_type = size_t;

  result_type operator()(const argument_type& fd) const noexcept
  {
    switch (fd.type()) {
      case os::WindowsFD::Type::HANDLE: {
        return std::hash<HANDLE>{}(static_cast<HANDLE>(fd));
      }
      case os::WindowsFD::Type::SOCKET: {
        return std::hash<SOCKET>{}(static_cast<SOCKET>(fd));
      }
    }

    UNREACHABLE();
  }
};

} // namespace std {

#endif // __STOUT_OS_WINDOWS_FD_HPP__
