#pragma once
#include <Windows.h>

namespace mic {
// All callers use overlapped handles. Cancellation is drained before the stack
// OVERLAPPED/event/buffer can go out of scope, including app and service shutdown.
inline bool transfer(HANDLE pipe, bool write, void* data, DWORD bytes,
                     HANDLE stop, DWORD timeoutMs) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD done = 0;
    BOOL ok = write ? WriteFile(pipe, data, bytes, &done, &ov)
                    : ReadFile(pipe, data, bytes, &done, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        HANDLE waits[] = {stop, ov.hEvent};
        if (WaitForMultipleObjects(2, waits, FALSE, timeoutMs) != WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe, &ov);
            GetOverlappedResult(pipe, &ov, &done, TRUE);
            ok = FALSE;
        } else {
            ok = GetOverlappedResult(pipe, &ov, &done, FALSE);
        }
    }
    CloseHandle(ov.hEvent);
    return ok && done == bytes;
}
}
