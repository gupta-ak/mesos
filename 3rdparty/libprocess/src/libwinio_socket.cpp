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
#include "poll_socket.hpp"

using std::string;

namespace process {
namespace network {
namespace internal {

Try<std::shared_ptr<SocketImpl>> PollSocketImpl::create(int_fd s)
{
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

  return Failure("XXX");
}


Future<Nothing> PollSocketImpl::connect(const Address& address)
{
  return Nothing();
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
  return Failure("asdsad");
}

} // namespace internal {
} // namespace network {
} // namespace process {
