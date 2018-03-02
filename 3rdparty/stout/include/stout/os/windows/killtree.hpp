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

#ifndef __STOUT_OS_WINDOWS_KILLTREE_HPP__
#define __STOUT_OS_WINDOWS_KILLTREE_HPP__

#include <stdlib.h>

#include <stout/os.hpp>

#include <stout/os.hpp>       // For `kill_job`.
#include <stout/try.hpp>      // For `Try<>`.
#include <stout/windows.hpp>  // For `SharedHandle`.

#include <stout/os/kill.hpp> // For `os::kill`

#include <stout/windows/error.hpp> // For `WindowsError`

namespace os {

namespace internal {

inline Try<Nothing> kill_job_tree(pid_t pid)
{
  Try<std::wstring> name = os::name_job(pid);
  if (name.isError()) {
    return Error("Failed to determine job object name: " + name.error());
  }

  Try<SharedHandle> handle =
    os::open_job(JOB_OBJECT_TERMINATE, false, name.get());
  if (handle.isError()) {
    return Error("Failed to open job object: " + handle.error());
  }

  Try<Nothing> killJobResult = os::kill_job(handle.get());
  if (killJobResult.isError()) {
    return Error("Failed to delete job object: " + killJobResult.error());
  }

  return Nothing();
}

} // namespace internal {


// Terminate the "process tree" rooted at the specified pid.
// Since there is no process tree concept on Windows,
// internally this function handles two cases:
//  - If the process is not in a job object, this function just kills the
//    process like `os::kill`.
//  - If the process is in a job object, then we have strong parent-child
//    a relationship, so we can kill the process tree. This function looks
//    up the job object for the given pid and terminates the job. This is
//    possible because `name_job` provides an idempotent one-to-one mapping
//    from pid to name.
inline Try<std::list<ProcessTree>> killtree(
    pid_t pid,
    int signal,
    bool groups = false,
    bool sessions = false)
{
  const Try<bool> in_job = os::is_process_in_job(None(), pid);
  if (in_job.isError()) {
    return Error(
        "Failed to determine if process is in job: " + in_job.error());
  }

  if (in_job.get()) {
    const Try<Nothing> result = os::internal::kill_job_tree(pid);
    if (result.isError()) {
      return Error("Failed to delete job object tree: " + result.error());
    }
  } else {
    const int result = os::kill(pid, SIGKILL);
    if (result != KILL_PASS) {
      // `os::kill` will call `TerminateProcess` so we can get that error.
      return WindowsError("Failed to kill process");
    }
  }

  // NOTE: This return value is unused. A future refactor
  // may change the return type to `Try<None>`.
  std::list<ProcessTree> process_tree_list;
  return process_tree_list;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_KILLTREE_HPP__
