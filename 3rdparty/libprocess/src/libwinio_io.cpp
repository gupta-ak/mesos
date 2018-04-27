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

#include <process/future.hpp>
#include <process/process.hpp> // For process::initialize.

#include <stout/error.hpp>
#include <stout/try.hpp>

#include <stout/os/read.hpp>
#include <stout/os/write.hpp>

#include "io_internal.hpp"
#include "libwinio.hpp"

namespace process {
namespace io {
namespace internal {

Future<size_t> read(int_fd fd, void* data, size_t size)
{
  process::initialize();

  // TODO(benh): Let the system calls do what ever they're supposed to
  // rather than return 0 here?
  if (size == 0) {
    return 0;
  }

  // Just do a synchronous call.
  if (!fd.is_overlapped()) {
    ssize_t result = os::read(fd, data, size);
    if (result == -1) {
      return Failure(WindowsError().message);
    }
    return static_cast<size_t>(result);
  }

  return windows::read(fd, data, size);
}

Future<size_t> write(int_fd fd, const void* data, size_t size)
{
  process::initialize();

  // TODO(benh): Let the system calls do what ever they're supposed to
  // rather than return 0 here?
  if (size == 0) {
    return 0;
  }

  // Just do a synchronous call.
  if (!fd.is_overlapped()) {
    ssize_t result = os::write(fd, data, size);
    if (result == -1) {
      return Failure(WindowsError().message);
    }
    return static_cast<size_t>(result);
  }

  return windows::write(fd, data, size);
}


Try<Nothing> prepare_async(int_fd fd)
{
  // TOOD(akagup): Makde this idempotent like os::nonblock.
  if (!fd.is_overlapped()) {
    return Nothing();
  }

  return libwinio_loop->registerHandle(fd);
}


Try<bool> is_async(int_fd fd)
{
  // TODO(akagup): Add windows iocp.
  return true;
}

} // namespace internal {
} // namespace io {
} // namespace process {
