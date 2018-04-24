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

#ifndef __LIBWINIO_HPP__
#define __LIBWINIO_HPP__

#include <stout/lambda.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>

#include <process/address.hpp>

namespace process {
namespace windows {

// Holds a handle registered by the thread pool. The PTP_IO object is used by
// the thread pool for io operations.
struct Handle {
  int_fd fd;
  PTP_IO io;
};

class Threadpool {
public:
  static Try<Threadpool*> create(DWORD threads);

  ~Threadpool();

  Try<Nothing> launchTimer(
      const lambda::function<void()>& callback,
      const Duration& duration);

  Try<Handle> registerHandle(const int_fd& fd);
  Try<Nothing> unregisterHandle(const Handle& fd);

private:
  Threadpool(
      const PTP_POOL& pool,
      const PTP_CLEANUP_GROUP& group,
      const TP_CALLBACK_ENVIRON& environment);

  PTP_POOL pool_;
  PTP_CLEANUP_GROUP group_;
  TP_CALLBACK_ENVIRON environment_;
};

// Read/Write callback. First argument is bytes read/written. Second argument
// is the Win32 error code.
typedef lambda::function<void(size_t, DWORD)> ReadWriteCallback;

// Callback for accept and connect. Only argument is the Win32 error code.
typedef lambda::function<void(DWORD)> AcceptConnectCallback;

// All of these functions do an asynchronous IO operation and execute the
// callback when the IO is complete. If the IO could not be scheduled, then
// an error is returned and the callback does not execute.
Try<Nothing> read(
    const Handle& fd,
    void* buf,
    size_t size,
    const ReadWriteCallback& callback);

Try<Nothing> write(
    const Handle& fd,
    const void* buf,
    size_t size,
    const ReadWriteCallback& callback);

// Socket only functions.
Try<Nothing> recv(
    const Handle& fd,
    void* buf,
    size_t size,
    const ReadWriteCallback& callback);

Try<Nothing> send(
    const Handle& fd,
    const void* buf,
    size_t size,
    const ReadWriteCallback& callback);

Try<Nothing> accept(
    const Handle& fd,
    const int_fd& accepted_socket,
    const AcceptConnectCallback& callback);

Try<Nothing> connect(
    const Handle& fd,
    const network::inet::Address& address,
    const AcceptConnectCallback& callback);

Try<Nothing> sendfile(
  const Handle& fd,
  const int_fd& file_fd,
  off_t offset,
  size_t size,
  const ReadWriteCallback& callback);

} // namespace windows {
} // namespace process {

#endif // __LIBWINIO_HPP__
