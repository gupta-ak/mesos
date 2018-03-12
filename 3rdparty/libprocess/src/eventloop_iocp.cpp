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

#include <stout/stringify.hpp>
#include <stout/windows.hpp>

#include <process/logging.hpp>
#include <process/once.hpp>

#include "eventloop_iocp.hpp"

namespace process {

HANDLE IocpHandle = NULL;

void IocpLoop::initialize()
{
    static Once* initialized = new Once();

    if (initialized->once()) {
        return;
    }

    IocpHandle = ::CreateIoCompletionPort(
        INVALID_HANDLE_VALUE,
        NULL,
        0,
        1);

    if (IocpHandle == NULL) {
        DWORD err = ::GetLastError();
        LOG(FATAL) << "Failed to init run loop (CreateIoCompletionPort): "
                   << stringify(err);
    }

    initialized->done();
}


void IocpLoop::run()
{
    OVERLAPPED_ENTRY entries[128];
    ULONG entriesRemoved = 0;
    do {
        BOOL success 
          = ::GetQueuedCompletionStatusEx(
                IocpHandle,
                entries,
                128,
                &entriesRemoved,
                INFINITE,
                FALSE);

        if (!success) {
            break;
        }

        for (ULONG i = 0; i < entriesRemoved; i++) {
            if (entries[i].lpCompletionKey == 0) {
                // Got signal to end IOCP.
                return;
            }

            Overlapped* overlapped = reinterpret_cast<Overlapped*>(entries[i].lpOverlapped);
            overlapped->callback(entries[i].lpOverlapped, entries[i].dwNumberOfBytesTransferred);
        }

    } while (true);
}


void IocpLoop::stop()
{
    BOOL success = ::PostQueuedCompletionStatus(IocpHandle, 0, 0, NULL);
    if (!success) {
        DWORD error = ::GetLastError();
        LOG(FATAL) << "Failed to stop IOCP event loop: " << stringify(error);
    }
}

} // namespace process {
