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

#ifndef __STOUT_OS_WINDOWS_READ_HPP__
#define __STOUT_OS_WINDOWS_READ_HPP__

#include <stout/result.hpp>

#include <stout/os/windows/io.hpp>

namespace os {

// Asynchronous read on a overlapped int_fd. Returns `Error` on fatal errors,
// `None()` on a successful pending IO operation or number of bytes read on a
// successful IO operation that finished immediately.
//
// NOTE: The type of `overlapped` is `void*` instead of `OVERLAPPED*`, so that
// the caller can use their custom overlapped struct without having to cast
// it. A common practice in Windows Overlapped IO is to create a new overlapped
// struct that extends the `OVERLAPPED` struct with custom data. For more info,
// see https://blogs.msdn.microsoft.com/oldnewthing/20101217-00/?p=11983.
inline Result<size_t> read_async(
    const int_fd& fd,
    void* data,
    size_t size,
    void* overlapped)
{
  return internal::read_write_async(
      fd, data, size, overlapped, internal::IOType::READ);
}

// Synchronous reads on any int_fd. Returns -1 on error and
// number of bytes read on success.
inline ssize_t read(const int_fd& fd, void* data, size_t size)
{
  return internal::read_write_sync(fd, data, size, internal::IOType::READ);
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_READ_HPP__
