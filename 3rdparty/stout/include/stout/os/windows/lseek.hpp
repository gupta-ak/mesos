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

#ifndef __STOUT_OS_WINDOWS_LSEEK_HPP__
#define __STOUT_OS_WINDOWS_LSEEK_HPP__

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>

namespace os {

inline Try<off_t> lseek(int_fd fd, off_t offset, int whence)
{
  DWORD move;
  if (whence == SEEK_SET) { move = FILE_BEGIN; }
  else if (whence == SEEK_CUR) { move = FILE_CURRENT; }
  else if (whence == SEEK_END) { move = FILE_END; }
  // TODO(andschwa): Maybe we want to support the Windows flags here
  // in addition to the POSIX flags instead of erroring.
  else { return Error("`whence` did not make sense."); }

  // TODO(andschwa): This may need to be synchronized if users aren't
  // careful about sharing their file handles among threads.
  //
  // TODO(andschwa): This may need to check if `offset` would overflow
  // 32 signed bits, in which case we need to split its value across
  // two arguments, which combined represent the 64-bit offset.
  const DWORD result =
    ::SetFilePointer(fd, static_cast<LONG>(offset), nullptr, move);

  if (result == INVALID_SET_FILE_POINTER) {
    return WindowsError();
  }

  return static_cast<off_t>(result);
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_LSEEK_HPP__
