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

#ifndef __STOUT_OS_WINDOWS_STAT_HPP__
#define __STOUT_OS_WINDOWS_STAT_HPP__

#include <string>

#include <stout/bytes.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>

#include <stout/internal/windows/attributes.hpp>
#include <stout/internal/windows/longpath.hpp>
#include <stout/internal/windows/reparsepoint.hpp>
#include <stout/internal/windows/symlink.hpp>

#ifdef _USE_32BIT_TIME_T
#error "Implementation of `os::stat::mtime` assumes 64-bit `time_t`."
#endif // _USE_32BIT_TIME_T


namespace os {
namespace stat {

// Forward declaration.
inline bool islink(const std::string& path);

inline bool isdir(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  // A symlink itself is not a directory.
  // If it's not a link, we ignore `follow`.
  if (follow == FollowSymlink::DO_NOT_FOLLOW_SYMLINK && islink(path)) {
    return false;
  }

  const Try<DWORD> attributes = ::internal::windows::get_file_attributes(
      ::internal::windows::longpath(path));

  if (attributes.isError()) {
    return false;
  }

  return attributes.get() & FILE_ATTRIBUTE_DIRECTORY;
}


// TODO(andschwa): Refactor `GetFileInformationByHandle` into its own function.
inline bool isdir(const int_fd& fd)
{
  BY_HANDLE_FILE_INFORMATION info;
  const BOOL result = ::GetFileInformationByHandle(fd, &info);
  if (result == FALSE) {
    return false;
  }

  return info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
}


inline bool isfile(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  // A symlink itself is a file, but not a regular file.
  // On POSIX, this check is done with `S_IFREG`, which
  // returns false for symbolic links.
  // If it's not a link, we ignore `follow`.
  if (follow == FollowSymlink::DO_NOT_FOLLOW_SYMLINK && islink(path)) {
    return false;
  }

  const Try<DWORD> attributes = ::internal::windows::get_file_attributes(
      ::internal::windows::longpath(path));

  if (attributes.isError()) {
    return false;
  }

  // NOTE: Windows files attributes do not define a flag for "regular"
  // files. Instead, this call will only return successfully iff the
  // given file or directory exists. Checking against the directory
  // flag determines if the path is a file or directory.
  return !(attributes.get() & FILE_ATTRIBUTE_DIRECTORY);
}


inline bool islink(const std::string& path)
{
  Try<::internal::windows::SymbolicLink> symlink =
    ::internal::windows::query_symbolic_link_data(path);

  return symlink.isSome();
}


// Returns the size in Bytes of a given file system entry. When applied to a
// symbolic link with `follow` set to `DO_NOT_FOLLOW_SYMLINK`, this will return
// zero because that's what Windows says.
inline Try<Bytes> size(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  const Try<SharedHandle> handle = (follow == FollowSymlink::FOLLOW_SYMLINK)
    ? ::internal::windows::get_handle_follow(path)
    : ::internal::windows::get_handle_no_follow(path);
  if (handle.isError()) {
    return Error("Error obtaining handle to file: " + handle.error());
  }

  LARGE_INTEGER file_size;

  if (::GetFileSizeEx(handle->get_handle(), &file_size) == 0) {
    return WindowsError();
  }

  return Bytes(file_size.QuadPart);
}


inline Try<Bytes> size(const int_fd& fd)
{
  LARGE_INTEGER file_size;

  if (::GetFileSizeEx(fd, &file_size) == 0) {
    return WindowsError();
  }

  return Bytes(file_size.QuadPart);
}


// TODO(andschwa): Replace `::_stat`. See MESOS-8275.
inline Try<long> mtime(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  if (follow == FollowSymlink::DO_NOT_FOLLOW_SYMLINK) {
    Try<::internal::windows::SymbolicLink> symlink =
      ::internal::windows::query_symbolic_link_data(path);

    if (symlink.isSome()) {
      return Error(
          "Requested mtime for '" + path +
          "', but symbolic links don't have an mtime on Windows");
    }
  }

  struct _stat s;

  if (::_stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  // To be safe, we assert that `st_mtime` is represented as `__int64`. To
  // conform to the POSIX, we also cast `st_mtime` to `long`; we choose to make
  // this conversion explicit because we expect the truncation to not cause
  // information loss.
  static_assert(
      std::is_same<__int64, __time64_t>::value,
      "Mesos assumes `__time64_t` is represented as `__int64`");
  return static_cast<long>(s.st_mtime);
}


// TODO(andschwa): Replace `::_stat`. See MESOS-8275.
inline Try<mode_t> mode(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  struct _stat s;

  if (follow == FollowSymlink::DO_NOT_FOLLOW_SYMLINK && islink(path)) {
    return Error("lstat not supported for symlink '" + path + "'");
  }

  if (::_stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  return s.st_mode;
}


// TODO(andschwa): Replace `::_stat`. See MESOS-8275.
inline Try<dev_t> dev(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  struct _stat s;

  if (follow == FollowSymlink::DO_NOT_FOLLOW_SYMLINK && islink(path)) {
    return Error("lstat not supported for symlink '" + path + "'");
  }

  if (::_stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  return s.st_dev;
}


// TODO(andschwa): Replace `::_stat`. See MESOS-8275.
inline Try<ino_t> inode(
    const std::string& path,
    const FollowSymlink follow = FollowSymlink::FOLLOW_SYMLINK)
{
  struct _stat s;

  if (follow == FollowSymlink::DO_NOT_FOLLOW_SYMLINK) {
      return Error("Non-following stat not supported for '" + path + "'");
  }

  if (::_stat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  return s.st_ino;
}

} // namespace stat {
} // namespace os {

#endif // __STOUT_OS_WINDOWS_STAT_HPP__
