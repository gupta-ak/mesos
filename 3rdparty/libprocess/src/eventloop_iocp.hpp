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

#ifndef __EVENTLOOP_IOCP_HPP__
#define __EVENTLOOP_IOCP_HPP__

#include "stout/windows.hpp"

namespace process {

// IOCP handle.
extern HANDLE IocpHandle;

// Generic function callback for IOCP callers.
typedef void (*IocpCallback)(OVERLAPPED* overlapped, DWORD bytesRead);

struct Overlapped {
    OVERLAPPED overlapped;
    IocpCallback callback;
    // Caller will add more info if needed.
};

class IocpLoop {
public:
    static void initialize();
    static void run();
    static void stop();
};

} // namespace process {

#endif // __EVENTLOOP_IOCP_HPP__