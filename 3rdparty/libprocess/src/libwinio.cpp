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

#include <process/logging.hpp>
#include <process/once.hpp>

#include "event_loop.hpp"
#include "libwinio.hpp"
#include "libwinio_internal.hpp"

namespace process {

windows::EventLoop* libwinio_loop;

void EventLoop::initialize()
{
  static Once* initialized = new Once();

  if (initialized->once()) {
    return;
  }

  Try<windows::EventLoop*> try_loop = windows::EventLoop::create();
  if (try_loop.isError()) {
    LOG(FATAL) << "Failed to initialize Windows IOCP event loop";
  }

  libwinio_loop = try_loop.get();

  initialized->done();
}


void EventLoop::delay(
    const Duration& duration,
    const std::function<void()>& function)
{
  libwinio_loop->launchTimer(duration, function);
}


double EventLoop::time()
{
  FILETIME filetime;
  ULARGE_INTEGER time;

  ::GetSystemTimeAsFileTime(&filetime);

  // FILETIME isn't 8 byte aligned so they suggest not do cast to int64*.
  time.HighPart = filetime.dwHighDateTime;
  time.LowPart = filetime.dwLowDateTime;

  // Constant taken from here:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms724228(v=vs.85).aspx.
  // It is the number of 100ns periods between the NT epoch (01/01/1601) and the
  // UNIX one (01/01/1970).
  if (time.QuadPart < 116444736000000000UL) {
    // We got a time before the epoch?
    LOG(FATAL) << "System clock is not set correctly";
 }

  time.QuadPart -= 116444736000000000UL;

  // time has returns 100ns segments, so we divide to get seconds.
  return static_cast<double>(time.QuadPart) / 10000000;
}


void EventLoop::run()
{
  libwinio_loop->run();
}


void EventLoop::stop()
{
  libwinio_loop->stop();
}

} // namespace process {
