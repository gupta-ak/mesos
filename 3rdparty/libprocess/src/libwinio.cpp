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

#include <stout/lambda.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/sendfile.hpp>

#include <stout/windows/error.hpp>

#include <process/address.hpp>
#include <process/network.hpp>

#include "libwinio.hpp"

namespace process {
namespace windows {

Try<Threadpool*> Threadpool::create(DWORD threads) {
  PTP_POOL pool = ::CreateThreadpool(NULL);
  if (pool == nullptr) {
    return WindowsError();
  }
  ::SetThreadpoolThreadMaximum(pool, threads);
  ::SetThreadpoolThreadMinimum(pool, threads);

  // Cleanup groups can group up different events like IO and timers so they
  // can all be cleaned up at once.
  PTP_CLEANUP_GROUP cleanup_group = ::CreateThreadpoolCleanupGroup();
  if (cleanup_group == nullptr) {
    return WindowsError();
  }

  // The callback environment struct holds all the thread pool information.
  TP_CALLBACK_ENVIRON environment;
  ::InitializeThreadpoolEnvironment(&environment);
  ::SetThreadpoolCallbackPool(&environment, pool);
  ::SetThreadpoolCallbackCleanupGroup(&environment, cleanup_group, nullptr);

  return new Threadpool(pool, cleanup_group, environment);
}

Threadpool::Threadpool(
    const PTP_POOL& pool,
    const PTP_CLEANUP_GROUP& group,
    const TP_CALLBACK_ENVIRON& environment)
  : pool_(pool), group_(group), environment_(environment) {}

static void CALLBACK timer_callback(
    PTP_CALLBACK_INSTANCE instance,
    PVOID context,
    PTP_TIMER timer)
{
  auto f = reinterpret_cast<lambda::function<void()>*>(context);
  (*f)();
  delete f;
  ::CloseThreadpoolTimer(timer);
}

Threadpool::~Threadpool()
{
  CloseThreadpoolCleanupGroupMembers(group_, FALSE, NULL);
  CloseThreadpoolCleanupGroup(group_);
  DestroyThreadpoolEnvironment(&environment_);
  CloseThreadpool(pool_);
}

Try<Nothing> Threadpool::launchTimer(
    const lambda::function<void()>& callback,
    const Duration& duration)
{
  lambda::function<void()>* fptr
    = new lambda::function<void()>(callback);

  PTP_TIMER  timer =
    ::CreateThreadpoolTimer(&timer_callback, (PVOID) fptr, &environment_);

  if (timer == nullptr) {
    return WindowsError();
  }

  // If you give a positive value, then the timer call interprets it as
  // absolute time. A negative value is interpretted as relative time.
  // 0 is run immediately. The resolution is in 100ns.
  ULARGE_INTEGER time_elapsed;
  time_elapsed.QuadPart = -duration.ns() / 100;

  FILETIME filetime;
  filetime.dwHighDateTime = time_elapsed.HighPart;
  filetime.dwLowDateTime = time_elapsed.LowPart;

  ::SetThreadpoolTimer(timer, &filetime, 0, 0);
  return Nothing();
}

enum class IoCallbackType {
  ReadWrite,
  AcceptConnect
};

struct Overlapped {
  OVERLAPPED overlapped;
  IoCallbackType type;
};

struct OverlappedReadWrite final : Overlapped {
  lambda::function<void(DWORD, size_t)> callback;
};

struct OverlappedAcceptConnect final : Overlapped {
  lambda::function<void(DWORD)> callback;
};

static void CALLBACK io_callback(
    PTP_CALLBACK_INSTANCE instance,
    PVOID context,
    PVOID overlapped_,
    ULONG io_result,
    ULONG_PTR number_of_bytes_transferred,
    PTP_IO io)
{
  if (overlapped_ == nullptr) {
    // Nothing we can really do here aside from log the event if the overlapped
    // object is null. It would only happen if something else in the thread
    // pool broke.
    LOG(WARNING)
      << "process::windows::io_callback returned with a null overlapped object"
      << " with error: " << io_result;

    return;
  }

  Overlapped* overlapped = reinterpret_cast<Overlapped*>(overlapped_);
  switch (overlapped->type) {
    case IoCallbackType::ReadWrite: {
      OverlappedReadWrite* rw =
        reinterpret_cast<OverlappedReadWrite*>(overlapped);
      
      rw->callback(io_result, number_of_bytes_transferred);
      delete rw;
    }
    case IoCallbackType::AcceptConnect: {
      OverlappedAcceptConnect* ac =
        reinterpret_cast<OverlappedAcceptConnect*>(overlapped);
      
      ac->callback(io_result);
      delete ac;
    }
  }
}

Try<Handle> Threadpool::registerHandle(const int_fd& fd)
{
  HANDLE handle = fd.get_handle();
  PTP_IO io =
    ::CreateThreadpoolIo(handle, &io_callback, nullptr, &environment_);
  
  if (io == nullptr) {
    return WindowsError();
  }

  return Handle{fd, io};
}

Try<Nothing> Threadpool::unregisterHandle(const Handle& handle)
{
  Try<Nothing> error = Nothing();
  HANDLE raw_handle = handle.fd.get_handle();

  // Cancel all IO and wait for callbacks to complete.
  if (::CancelIoEx(raw_handle, nullptr) == FALSE) {
    // Ignore ERROR_NOT_FOUND since that means that there’s no pending IO.
    if (::GetLastError() !=  ERROR_NOT_FOUND) {
      error = WindowsError();
    }
  }
  ::WaitForThreadpoolIoCallbacks(handle.io, FALSE);
  ::CloseThreadpoolIo(handle.io);
  return error;
}

static Try<Nothing> read_write(
  const Handle& handle,
  void* buf,
  size_t size,
  const ReadWriteCallback& callback,
  os::internal::IOType type)
{
  std::unique_ptr<OverlappedReadWrite> overlapped(new OverlappedReadWrite);
  overlapped->overlapped = {};
  overlapped->type = IoCallbackType::ReadWrite;
  overlapped->callback = callback;

  // Start the asynchronous operation.
  ::StartThreadpoolIo(handle.io);
  Result<size_t> result = os::internal::read_write_async(
      handle.fd,
      buf,
      size,
      overlapped.get(),
      type);

  if (result.isNone()) {
    // IO was successfully scheduled asynchronously. We release the overlapped
    // pointer, since the callback will delete it.
    overlapped.release();
    return Nothing();
  }

  // The operation completed synchronously. We need to call `CancelThreadpoolIo`
  // to notify the thread pool that there won't be an IOCP notification.
  ::CancelThreadpoolIo(handle.io);

  if (result.isError()) {
    return Error(result.error());
  }

  // IO completed synchronously.
  overlapped->callback(result.get(), NO_ERROR);
  return Nothing();
}

Try<Nothing> read(
    const Handle& handle,
    void* buf,
    size_t size,
    const ReadWriteCallback& callback)
{
  return read_write(
      handle,
      buf,
      size,
      callback,
      os::internal::IOType::READ);
}

Try<Nothing> write(
    const Handle& handle,
    const void* buf,
    size_t size,
    const ReadWriteCallback& callback)
{
  return read_write(
      handle,
      const_cast<void*>(buf),
      size,
      callback,
      os::internal::IOType::WRITE);
}

Try<Nothing> recv(
    const Handle& handle,
    void* buf,
    size_t size,
    const ReadWriteCallback& callback)
{
  return read(handle, buf, size, callback);
}

Try<Nothing> send(
    const Handle& handle,
    const void* buf,
    size_t size,
    const ReadWriteCallback& callback)
{
  return write(handle, buf, size, callback);
}

Try<Nothing> accept(
    const Handle& handle,
    const int_fd& accepted_socket,
    const AcceptConnectCallback& callback)
{
  std::unique_ptr<OverlappedAcceptConnect> overlapped(
      new OverlappedAcceptConnect);
  overlapped->overlapped = {};
  overlapped->type = IoCallbackType::AcceptConnect;
  overlapped->callback = callback;

  // AcceptEx needs a buffer equal to the 2 * (sockaddr size + 16).
  constexpr DWORD address_size = sizeof(SOCKADDR_STORAGE) + 16;
  char output_buf[2 * address_size];
  DWORD bytes;

  ::StartThreadpoolIo(handle.io);
  const BOOL success = ::AcceptEx(
      handle.fd,
      accepted_socket,
      output_buf,
      0,
      address_size,
      address_size,
      &bytes,
      reinterpret_cast<OVERLAPPED*>(overlapped.get()));

  DWORD error = ::WSAGetLastError();
  if (!success && error == ERROR_IO_PENDING) {
    overlapped.release();
    return Nothing();
  }

  ::CancelThreadpoolIo(handle.io);
  if (success) {
    overlapped->callback(NO_ERROR);
    return Nothing();
  }

  return WindowsSocketError(error);
}

static LPFN_CONNECTEX init_connect_ex(const int_fd& fd)
{
  LPFN_CONNECTEX connect_ex;
  GUID connect_ex_guid = WSAID_CONNECTEX;
  DWORD bytes;

  int res = ::WSAIoctl(
    fd,
    SIO_GET_EXTENSION_FUNCTION_POINTER,
    &connect_ex_guid,
    sizeof(connect_ex_guid),
    &connect_ex,
    sizeof(connect_ex),
    &bytes,
    NULL,
    NULL);

  // We can't use connect if this fails,  so we just force an abort.
  CHECK_EQ(res, 0);
  return connect_ex;
}

static LPFN_CONNECTEX& get_connect_ex_ptr(const int_fd& fd)
{
  // Magic static initialization.
  static LPFN_CONNECTEX ptr = init_connect_ex(fd);
  return ptr;
}

Try<Nothing> connect(
    const Handle& handle,
    const network::Address& address,
    const AcceptConnectCallback& callback)
{
  std::unique_ptr<OverlappedAcceptConnect> overlapped(
      new OverlappedAcceptConnect);
  overlapped->overlapped = {};
  overlapped->type = IoCallbackType::AcceptConnect;
  overlapped->callback = std::move(callback);

  // ConnectEx needs to bind first.
  Try<Nothing> bind_result = Nothing();
  if (address.family() == network::Address::Family::INET4) {
    const network::inet4::Address addr = network::inet4::Address::ANY_ANY();
    bind_result = network::bind(handle.fd, addr);
  } else if (address.family() == network::Address::Family::INET6) {
    const network::inet6::Address addr = network::inet6::Address::ANY_ANY();
    bind_result = network::bind(handle.fd, addr);
  } else {
    return Error("Async connect only supports IPv6 and IPv4");
  }

  if (bind_result.isError()) {
    // `WSAEINVAL` means socket is already bound, so we can continue.
    if (::WSAGetLastError() != WSAEINVAL) {
      return bind_result;
    }
  }

  sockaddr_storage storage = address;
  const socklen_t address_size = static_cast<socklen_t>(address.size());
  LPFN_CONNECTEX connect_ex = get_connect_ex_ptr(handle.fd);

  ::StartThreadpoolIo(handle.io);
  BOOL success = connect_ex(
      handle.fd,
      reinterpret_cast<sockaddr*>(&storage),
      address_size,
      nullptr,
      0,
      nullptr,
      reinterpret_cast<OVERLAPPED*>(overlapped.get()));

  DWORD error = ::WSAGetLastError();
  if (!success && error == WSA_IO_PENDING) {
    overlapped.release();
    return Nothing();
  }

  ::CancelThreadpoolIo(handle.io);
  if (success) {
    overlapped->callback(NO_ERROR);
    return Nothing();
  }

  return WindowsSocketError(error);
}

Try<Nothing> sendfile(
  const Handle& handle,
  const int_fd& file,
  off_t offset,
  size_t size,
  const ReadWriteCallback& callback)
{
  if (offset < 0) {
    return Error("process::windows::sendfile got negative offset");
  }

  uint64_t offset64 = static_cast<uint64_t>(offset);
  std::unique_ptr<OverlappedReadWrite> overlapped(new OverlappedReadWrite);
  overlapped->overlapped = {};
  overlapped->overlapped.Offset = static_cast<DWORD>(offset64);
  overlapped->overlapped.OffsetHigh = static_cast<DWORD>(offset64 >> 32);
  overlapped->type = IoCallbackType::ReadWrite;
  overlapped->callback = callback;

  ::StartThreadpoolIo(handle.io);
  Result<size_t> result =
    os::sendfile_async(handle.fd, file, size, overlapped.get());

  if (result.isNone()) {
    overlapped.release();
    return Nothing();
  }

  ::CancelThreadpoolIo(handle.io);
  if (result.isError()) {
    return Error(result.error());
  }

  overlapped->callback(result.get(), NO_ERROR);
  return Nothing();
}

} // namespace windows {
} // namespace process {