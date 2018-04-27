// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License


#ifdef __WINDOWS__
#include <stout/windows.hpp>
#else
#include <netinet/tcp.h>
#endif // __WINDOWS__

#include <process/io.hpp>
#include <process/loop.hpp>
#include <process/network.hpp>
#include <process/socket.hpp>

#include <stout/os/sendfile.hpp>
#include <stout/os/strerror.hpp>
#include <stout/os.hpp>

#include "config.hpp"
#include "libwinio_internal.hpp"
#include "poll_socket.hpp"

using std::string;

namespace process {
namespace network {
namespace internal {

Try<std::shared_ptr<SocketImpl>> PollSocketImpl::create(int_fd s)
{
  Try<Nothing> error = io::prepare_async(s);
  if (error.isError()) {
    return Error(error.error());
  }
  return std::make_shared<PollSocketImpl>(s);
}


Try<Nothing> PollSocketImpl::listen(int backlog)
{
  if (::listen(get(), backlog) < 0) {
    return ErrnoError();
  }
  return Nothing();
}


Future<std::shared_ptr<SocketImpl>> PollSocketImpl::accept()
{
  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return from the call to
  // `io::poll` and end up accepting a socket incorrectly.
  auto self = shared(this);

  Try<Address> address = network::address(get());
  if (address.isError()) {
    return Failure("Failed to get address: " + address.error());
  }

  int family = 0;
  if (address->family() == Address::Family::INET4) {
    family = AF_INET;
  } else if (address->family() == Address::Family::INET6) {
    family = AF_INET6;
  } else {
    return Failure("Unsupported address family. Windows only supports IP.");
  }

  Try<int_fd> accept_socket_ = net::socket(family, SOCK_STREAM, 0);
  if (accept_socket_.isError()) {
    return Failure(accept_socket_.error());
  }

  int_fd accept_socket = accept_socket_.get();

  return windows::accept(self->get(), accept_socket)
    .onAny([accept_socket](const Future<Nothing> future) {
      if (!future.isReady()) {
        os::close(accept_socket);
      }
    })
    .then([self, accept_socket]() -> Future<std::shared_ptr<SocketImpl>> {
      SOCKET listen = self->get();

      // Inherit from the listening socket.
      int res = ::setsockopt(
          accept_socket,
          SOL_SOCKET,
          SO_UPDATE_ACCEPT_CONTEXT,
          reinterpret_cast<char*>(&listen),
          sizeof(listen));

      if (res != 0) {
        const int error = ::WSAGetLastError();
        os::close(accept_socket);
        return Failure(
              "Failed to set accepted socket: " + stringify(error));
      }

      // Disable Nagle algorithm.
      int on = 1;
      res = ::setsockopt(
          accept_socket,
          SOL_TCP,
          TCP_NODELAY,
          reinterpret_cast<const char*>(&on),
          sizeof(on));

      if (res != 0) {
        const int error = ::WSAGetLastError();
        os::close(accept_socket);
        return Failure(
            "Failed to turn off the Nagle algorithm: " + stringify(error));
      }

      Try<std::shared_ptr<SocketImpl>> impl = create(accept_socket);
      if (impl.isError()) {
        os::close(accept_socket);
        return Failure("Failed to create socket: " + impl.error());
      }

      return impl.get();
    });
}


Future<Nothing> PollSocketImpl::connect(const Address& address)
{
  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return.
  auto self = shared(this);

  return windows::connect(self->get(), address)
    .then([self]() -> Future<Nothing> {
      // Set connect socket to be a "real" socket.
      int res = ::setsockopt(
          self->get(),
          SOL_SOCKET,
          SO_UPDATE_CONNECT_CONTEXT,
          nullptr,
          0);

      if (res != 0) {
        const int error = ::WSAGetLastError();
        return Failure(
              "Failed to set connected socket: " + stringify(error));
      }

      return Nothing();
    });
}


Future<size_t> PollSocketImpl::recv(char* data, size_t size)
{
  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return from the call to
  // `io::read` and end up reading data incorrectly.
  auto self = shared(this);

  return io::read(get(), data, size)
    .then([self](size_t length) {
      return length;
    });
}


Future<size_t> PollSocketImpl::send(const char* data, size_t size)
{
  CHECK(size > 0); // TODO(benh): Just return 0 if `size` is 0?

  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return.
  auto self = shared(this);

  // TODO(benh): Reuse `io::write`? Or is `net::send` and
  // `MSG_NOSIGNAL` critical here?
  return io::write(get(), data, size)
    .then([self](size_t length) {
      return length;
    });
}


Future<size_t> PollSocketImpl::sendfile(int_fd fd, off_t offset, size_t size)
{
  CHECK(size > 0); // TODO(benh): Just return 0 if `size` is 0?

  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return.
  auto self = shared(this);

  return windows::sendfile(self->get(), fd, offset, size)
    .then([self](size_t length) {
      return length;
    });
}

} // namespace internal {
} // namespace network {
} // namespace process {
