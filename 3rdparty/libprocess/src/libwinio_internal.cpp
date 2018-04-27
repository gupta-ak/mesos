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
#include <process/process.hpp> // For process::initialize.

#include "libwinio_internal.hpp"
#include "libwinio.hpp"

namespace process {
namespace windows {

// Constants for IOCP GetQueuedCompletionStatusEx
constexpr int MAX_GQCS_ENTRIES = 1;
constexpr DWORD KEY_QUIT = 0;
constexpr DWORD KEY_IO = 1;
constexpr DWORD KEY_TIMER = 2;


// Definitions for handling for event loop timers.
struct TimerOverlapped {
  HANDLE timer;
  LARGE_INTEGER time;
  lambda::function<void()> callback;
};

static void CALLBACK timer_apc(
    LPVOID arg,
    DWORD timer_low,
    DWORD timer_high)
{
  TimerOverlapped* timer = reinterpret_cast<TimerOverlapped*>(arg);
  timer->callback();
  ::CloseHandle(timer->timer);
  delete timer;
}

static Try<Nothing> queue_timer_apc_from_iocp(LPOVERLAPPED overlapped)
{
  TimerOverlapped* timer = reinterpret_cast<TimerOverlapped*>(overlapped);

  const BOOL success = ::SetWaitableTimer(
      timer->timer,
      &timer->time,
      0,
      &timer_apc,
      timer,
      TRUE);

  if (success) {
    return Nothing();
  }

  return WindowsError();
}


// Definitions for handling event loop IO.
enum class IOType {
  READ,
  WRITE,
  RECV,
  SEND,
  ACCEPT,
  CONNECT,
  SENDFILE,
  CANCEL
};

struct IOOverlappedBase {
  OVERLAPPED overlapped;
  HANDLE handle;
  IOType type;
};

template <typename T>
struct IOOverlapped : IOOverlappedBase {
  Promise<T> promise;
};

static void handle_io(
    LPOVERLAPPED overlapped_,
    DWORD number_of_bytes_transferred)
{
  IOOverlappedBase* overlapped =
    reinterpret_cast<IOOverlappedBase*>(overlapped_);

  // Get the Win32 error code of the overlapped operation. The status code
  // is actually in overlapped->overlapped->Internal, but it's the NT
  // status code instead of the Win32 error.
  DWORD error = ERROR_SUCCESS;
  DWORD bytes;
  const BOOL success = ::GetOverlappedResult(
      overlapped->handle,
      &overlapped->overlapped,
      &bytes,
      FALSE);

  if (!success) {
    error = ::GetLastError();
  } else {
    // If the IO succeeded, these should be the same for sure.
    CHECK_EQ(bytes, number_of_bytes_transferred);
  }

  switch (overlapped->type) {
    // For reads, we need to make sure we ignore the EOF errors.
    case IOType::READ:
    case IOType::RECV: {
      IOOverlapped<size_t>* io_read =
        reinterpret_cast<IOOverlapped<size_t>*>(overlapped);
      
      if (io_read->promise.future().hasDiscard()) {
        io_read->promise.discard();
      } else if (error == ERROR_SUCCESS) {
        io_read->promise.set(number_of_bytes_transferred);
      } else if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) {
        io_read->promise.set(0);
      } else {
        io_read->promise.fail("IO failed with error code: " + stringify(error));
      }

      break;
    }

    // For writes, we will fail on any error.
    case IOType::WRITE:
    case IOType::SEND:
    case IOType::SENDFILE: {
      IOOverlapped<size_t>* io_write =
        reinterpret_cast<IOOverlapped<size_t>*>(overlapped);
      
      if (io_write->promise.future().hasDiscard()) {
        io_write->promise.discard();
      } else if (error == ERROR_SUCCESS) {
        io_write->promise.set(number_of_bytes_transferred);
      } else {
        io_write->promise.fail("IO failed with error code: " + stringify(error));
      }

      break;
    }

    // connect and accept are the only ones that have a different Promise type.
    case IOType::CONNECT:
    case IOType::ACCEPT: {
      IOOverlapped<Nothing>* io_connect =
        reinterpret_cast<IOOverlapped<Nothing>*>(overlapped);
      
      if (io_connect->promise.future().hasDiscard()) {
        io_connect->promise.discard();
      } else if (error == ERROR_SUCCESS) {
        io_connect->promise.set(Nothing());
      } else {
        io_connect->promise.fail("IO failed with error code: " + stringify(error));
      }

      break;
    }
  }
}


// Function to handle all IOCP notifications.
static bool handle_completion(const OVERLAPPED_ENTRY* entry)
{
  bool continue_loop = true;
  switch (entry->lpCompletionKey) {
    case KEY_QUIT: {
      continue_loop = false;
      break;
    }
    case KEY_TIMER: {
      // In the IOCP, we just set the timer. When the timer completes, it will
      // queue up an APC that will interrupt the IOCP loop in order to execute
      // the APC callback.
      Try<Nothing> result = queue_timer_apc_from_iocp(entry->lpOverlapped);
      if (result.isError()) {
        // Nothing we can really do here aside from log the event.
        LOG(WARNING) << "process::windows::handle_completion failed to set timer: "
                     << result.error();
      }
      break;
    }
    case KEY_IO: {
      if (entry->lpOverlapped == nullptr) {
        // Don't know if this is possible, but just log it in case.
        LOG(WARNING)
          << "process::windows::handle_completion returned with a null"
          << "overlapped object";
      } else {
        handle_io(entry->lpOverlapped, entry->dwNumberOfBytesTransferred);
      }
      break;
    }
  }
  return continue_loop;
}


// Windows Event loop implementation.
Try<EventLoop*> EventLoop::create()
{
  HANDLE iocp_handle = ::CreateIoCompletionPort(
      INVALID_HANDLE_VALUE,
      nullptr,
      0,
      1);

  if (iocp_handle == nullptr) {
    return WindowsError();
  }

  return new EventLoop(iocp_handle);
}

EventLoop::EventLoop(HANDLE iocp_handle)
  : iocp_handle_(std::unique_ptr<HANDLE, HandleDeleter>(iocp_handle)) {}

Try<Nothing> EventLoop::run()
{
  bool continue_loop = true;
  while (continue_loop) {
    OVERLAPPED_ENTRY entries[MAX_GQCS_ENTRIES];
    ULONG dequeued_entries;

    // This function can return in three ways:
    //   1) We get some IO completion events and the function will return true.
    //   2) A timer APC interrupts the function in it's alertable wait status,
    //      so the thread will execute the APC. The function will return false
    //      and set the error to WAIT_IO_COMPLETION.
    //   3) We get a legitimate error, so we exit early.
    BOOL success = ::GetQueuedCompletionStatusEx(
        iocp_handle_.get(),
        entries,
        MAX_GQCS_ENTRIES,
        &dequeued_entries,
        INFINITE,
        TRUE);

    if (!success) {
      if (::GetLastError() != WAIT_IO_COMPLETION) {
        return WindowsError();
      }

      // Got APC interrupt. We do a quick poll with GQCS to check if we have
      // some work to do after the APC. If there isn't any work, then we get
      // a timeout error. Since we don't do an alertable wait, we won't get
      // interrupted by APCs.
      success = ::GetQueuedCompletionStatusEx(
        iocp_handle_.get(),
        entries,
        MAX_GQCS_ENTRIES,
        &dequeued_entries,
        0,
        FALSE);

      if (!success && ::GetLastError() != WAIT_TIMEOUT) {
        return WindowsError();  
      }

      // We didn't have anything in the IOCP queue, so we go back to the loop.
      if (!success) {
        continue;
      }
    }

    // Dequeue completion packets and process them. If we get a quit
    // notification, then we will finish the current queue and then exit.
    for (ULONG i = 0; i < dequeued_entries; i++) {
      continue_loop = continue_loop && handle_completion(entries + i);
    }
  }
  return Nothing();
}


Try<Nothing> EventLoop::stop()
{
  const BOOL success =
    ::PostQueuedCompletionStatus(iocp_handle_.get(), 0, KEY_QUIT, nullptr);

  if (success) {
    return Nothing();
  }

  return WindowsError();
}


Try<Nothing> EventLoop::launchTimer(
    const Duration& duration,
    const lambda::function<void()>& callback)
{
  HANDLE timer = ::CreateWaitableTimerW(NULL, true, NULL);
  if (timer == NULL) {
    return WindowsError();
  }

  // If you give a positive value, then the timer call interprets it as
  // absolute time. A negative value is interpretted as relative time.
  // 0 is run immediately. The resolution is in 100ns.
  LARGE_INTEGER time_elapsed;
  time_elapsed.QuadPart = -duration.ns() / 100;

  TimerOverlapped* overlapped =
    new TimerOverlapped{timer, time_elapsed, callback};

  // We don't actually set the timer here since APCs only execute in the same
  // thread that called the async function. So, we queue the function call to
  // the IOCP so the event loop thread can queue the APC.
  const BOOL success = ::PostQueuedCompletionStatus(
      iocp_handle_.get(),
      0,
      KEY_TIMER,
      reinterpret_cast<LPOVERLAPPED>(overlapped));

  if (success) {
    return Nothing();
  }

  return WindowsError();
}


Try<Nothing> EventLoop::registerHandle(const int_fd& fd)
{
  // Confusing name, but CreateIoCompletionPort can also assigns handles to an
  // IO completion port 
  HANDLE iocp_handle = ::CreateIoCompletionPort(fd, iocp_handle_.get(), KEY_IO, 0);
  if (iocp_handle == nullptr) {
    return WindowsError();
  }

  BOOL success = ::SetFileCompletionNotificationModes(
      fd,
      FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);

  if (!success) {
    return WindowsError();
  }
  return Nothing();
}


Try<Nothing> EventLoop::unregisterHandle(const int_fd& fd)
{
  // We can't do this through Win32. If we want to do this, we can load ntdll
  // and do NtSetInformationFile.
  // See https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/content/ntifs/nf-ntifs-ntsetinformationfile
  return Nothing();
}


static Future<size_t> read_write(
  const int_fd& fd,
  void* buf,
  size_t size,
  IOType type)
{
  process::initialize();

  // We use a `std::shared_ptr`, so we can safely support canceling. By having
  // the callback store the overlapped pointer. 
  auto overlapped = std::make_shared<IOOverlapped<size_t>>();
  overlapped->handle = fd;
  overlapped->type = type;
  Future<size_t> future = overlapped->promise.future();

  os::internal::IOType os_type;
  if (type == IOType::READ || type == IOType::RECV) {
    os_type = os::internal::IOType::READ;
  } else {
    os_type = os::internal::IOType::WRITE;
  }
  // Start the asynchronous operation.
  Result<size_t> result = os::internal::read_write_async(
      fd,
      buf,
      size,
      overlapped.get(),
      os_type);

  // In a synchronous failure or success, we can immediately set the promise.
  if (result.isError()) {
    overlapped->promise.fail("IO operation failed: " + result.error());
  } else if (result.isSome()) {
    overlapped->promise.set(result.get());
  }

  // We use capture a `weak_ptr` in the `.onDiscard` callback, so that we can
  // cancel the IO operation with the overlapped object if it still exists.
  // The captured `shared_ptr` in the `.onAny` callback ensures that we 
  // keep the overlapped object alive until the IOCP callbacks are done.
  auto overlapped_weak = std::weak_ptr<IOOverlapped<size_t>>(overlapped);
  return future
    .onDiscard([fd, overlapped_weak]() { 
      std::shared_ptr<IOOverlapped<size_t>> cancel = overlapped_weak.lock();
      if (static_cast<bool>(cancel)) {
        // Note that there is technically a race here. We could be between the
        // IOCP event and setting the promise in the overlapped object. In that
        // case, it's still safe to call this because it will just no-op.
        ::CancelIoEx(fd, reinterpret_cast<LPOVERLAPPED>(cancel.get()));
      }
    })
    .then([overlapped](size_t length) {
      return length;
    });
}


Future<size_t> read(const int_fd& fd, void* buf, size_t size)
{
  return read_write(fd, buf, size, IOType::READ);
}


Future<size_t> write(const int_fd& fd, const void* buf, size_t size)
{
  return read_write(fd, const_cast<void*>(buf), size, IOType::WRITE);
}


Future<size_t> recv(const int_fd& fd, void* buf, size_t size)
{
  return read_write(fd, buf, size, IOType::RECV);
}


Future<size_t> send(const int_fd& fd, const void* buf, size_t size)
{
  return read_write(fd, const_cast<void*>(buf), size, IOType::SEND);
}


Future<Nothing> accept(
    const int_fd& fd,
    const int_fd& accepted_socket)
{
  process::initialize();

  auto overlapped = std::make_shared<IOOverlapped<Nothing>>();
  overlapped->handle = fd;
  overlapped->type = IOType::ACCEPT;

  // AcceptEx needs a buffer equal to the 2 * (sockaddr size + 16) for the
  // remote and local address.
  constexpr DWORD address_size = sizeof(SOCKADDR_STORAGE) + 16;
  char* output_buf = new char[2 * address_size];
  DWORD bytes;

  Future<Nothing> future = overlapped->promise.future();
  const BOOL success = ::AcceptEx(
      fd,
      accepted_socket,
      output_buf,
      0,
      address_size,
      address_size,
      &bytes,
      reinterpret_cast<LPOVERLAPPED>(overlapped.get()));

  // On a synchronous operation, we can already set the future.
  DWORD error = ::WSAGetLastError();
  if (success) {
    overlapped->promise.set(Nothing());
  } else if (error != WSA_IO_PENDING) {
    overlapped->promise.fail("IO operation failed: " + stringify(error));
  }

  auto overlapped_weak = std::weak_ptr<IOOverlapped<Nothing>>(overlapped);
  return future
    .onDiscard([fd, overlapped_weak]() { 
      std::shared_ptr<IOOverlapped<Nothing>> cancel = overlapped_weak.lock();
      if (static_cast<bool>(cancel)) {
        // Note that there is technically a race here. We could be between the
        // IOCP event and setting the promise in the overlapped object. In that
        // case, it's still safe to call this because it will just no-op.
        ::CancelIoEx(fd, reinterpret_cast<LPOVERLAPPED>(cancel.get()));
      }
    })
    .onAny([overlapped, output_buf]() {
      delete output_buf;
    });
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


Future<Nothing> connect(
    const int_fd& fd,
    const network::Address& address)
{
  // ConnectEx needs to bind first.
  Try<Nothing> bind_result = Nothing();
  if (address.family() == network::Address::Family::INET4) {
    const network::inet4::Address addr = network::inet4::Address::ANY_ANY();
    bind_result = network::bind(fd, addr);
  } else if (address.family() == network::Address::Family::INET6) {
    const network::inet6::Address addr = network::inet6::Address::ANY_ANY();
    bind_result = network::bind(fd, addr);
  } else {
    return Failure("Async connect only supports IPv6 and IPv4");
  }

  if (bind_result.isError()) {
    // `WSAEINVAL` means socket is already bound, so we can continue.
    if (::WSAGetLastError() != WSAEINVAL) {
      return Failure("Failed to bind connect socket: " + bind_result.error());
    }
  }

  // Load ConnectEx function pointer, since it's not normally available.
  sockaddr_storage storage = address;
  const int address_size = static_cast<int>(address.size());
  LPFN_CONNECTEX connect_ex = get_connect_ex_ptr(fd);

  auto overlapped = std::make_shared<IOOverlapped<Nothing>>();
  overlapped->handle = fd;
  overlapped->type = IOType::CONNECT;
  Future<Nothing> future = overlapped->promise.future();

  BOOL success = connect_ex(
      fd,
      reinterpret_cast<const sockaddr*>(&storage),
      address_size,
      nullptr,
      0,
      nullptr,
      reinterpret_cast<LPOVERLAPPED>(overlapped.get()));

  // On a synchronous operation, we can already set the future.
  DWORD error = ::WSAGetLastError();
  if (success) {
    overlapped->promise.set(Nothing());
  } else if (error != WSA_IO_PENDING) {
    overlapped->promise.fail("IO operation failed: " + stringify(error));
  }

  auto overlapped_weak = std::weak_ptr<IOOverlapped<Nothing>>(overlapped);
  return future
    .onDiscard([fd, overlapped_weak]() { 
      std::shared_ptr<IOOverlapped<Nothing>> cancel = overlapped_weak.lock();
      if (static_cast<bool>(cancel)) {
        // Note that there is technically a race here. We could be between the
        // IOCP event and setting the promise in the overlapped object. In that
        // case, it's still safe to call this because it will just no-op.
        ::CancelIoEx(fd, reinterpret_cast<LPOVERLAPPED>(cancel.get()));
      }
    })
    .then([overlapped](Nothing nothing) {
      return nothing;
    });
}


Future<size_t> sendfile(
  const int_fd& socket,
  const int_fd& file,
  off_t offset,
  size_t size)
{
  process::initialize();

  if (offset < 0) {
    return Failure("process::windows::sendfile got negative offset");
  }

  uint64_t offset64 = static_cast<uint64_t>(offset);
  auto overlapped = std::make_shared<IOOverlapped<size_t>>();
  overlapped->overlapped.Offset = static_cast<DWORD>(offset64);
  overlapped->overlapped.OffsetHigh = static_cast<DWORD>(offset64 >> 32);
  overlapped->handle = socket;
  overlapped->type = IOType::SENDFILE;
  Future<size_t> future = overlapped->promise.future();

  Result<size_t> result =
    os::sendfile_async(socket, file, size, overlapped.get());

  // In a synchronous failure or success, we can immediately set the promise.
  if (result.isError()) {
    overlapped->promise.fail("IO operation failed: " + result.error());
  } else if (result.isSome()) {
    overlapped->promise.set(result.get());
  }

  // We use capture a `weak_ptr` in the `.onDiscard` callback, so that we can
  // cancel the IO operation with the overlapped object if it still exists.
  // The captured `shared_ptr` in the `.onAny` callback ensures that we 
  // keep the overlapped object alive until the IOCP callbacks are done.
  auto overlapped_weak = std::weak_ptr<IOOverlapped<size_t>>(overlapped);
  return future
    .onDiscard([socket, overlapped_weak]() { 
      std::shared_ptr<IOOverlapped<size_t>> cancel = overlapped_weak.lock();
      if (static_cast<bool>(cancel)) {
        // Note that there is technically a race here. We could be between the
        // IOCP event and setting the promise in the overlapped object. In that
        // case, it's still safe to call this because it will just no-op.
        ::CancelIoEx(socket, reinterpret_cast<LPOVERLAPPED>(cancel.get()));
      }
    })
    .then([overlapped](size_t length) {
      return length;
    });
}

} // namespace windows {
} // namespace process {