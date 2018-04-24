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

#include <memory>
#include <string>

#include <boost/shared_array.hpp>

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/loop.hpp>
#include <process/process.hpp> // For process::initialize.

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/read.hpp>
#include <stout/os/strerror.hpp>
#include <stout/os/write.hpp>

#include "io_internal.hpp"
#include "libwinio.hpp"

namespace process {
namespace io {
namespace internal {

Future<size_t> read(int_fd fd, void* data, size_t size)
{ 
  // TODO(benh): Let the system calls do what ever they're supposed to
  // rather than return 0 here?
  if (size == 0) {
    return 0;
  }

  windows::Threadpool* tp = windows::Threadpool::create(1).get();
  windows::Handle h = tp->registerHandle(fd).get();

  process::initialize();

  Promise<size_t>* promise = new Promise<size_t>();

  Try<Nothing> result =
    windows::read(h, data, size, [promise](size_t result, DWORD code) {
      if (promise->future().hasDiscard()) {
        promise->discard();
      } else if (code != NO_ERROR) {
        promise->fail("Got failed exit code");
      } else {
        promise->set(result);
      }
      delete promise;
    });

  if (result.isError()) {
    delete promise;
    return Failure(result.error());
  }

  return promise->future();
}

Future<size_t> write(int_fd fd, const void* data, size_t size)
{
  // TODO(benh): Let the system calls do what ever they're supposed to
  // rather than return 0 here?
  if (size == 0) {
    return 0;
  }

  windows::Threadpool* tp = windows::Threadpool::create(1).get();
  windows::Handle h = tp->registerHandle(fd).get();
  
  process::initialize();

  Promise<size_t>* promise = new Promise<size_t>();

  Try<Nothing> result =
    windows::write(h, data, size, [promise](size_t result, DWORD code) {
      if (promise->future().hasDiscard()) {
        promise->discard();
      } else if (code != NO_ERROR) {
        promise->fail("Got failed exit code");
      } else {
        promise->set(result);
      }
      delete promise;
    });
  
  if (result.isError()) {
    delete promise;
    return Failure(result.error());
  }

  return promise->future();
}

} // namespace internal {
} // namespace io {
} // namespace process {
