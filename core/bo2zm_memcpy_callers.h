#pragma once

namespace bo2zm::memcpy_callers {

// Test build only (BO2ZM_MEMCPY_CALLERS, bo2zm_memcpy_callers.cpp): the calling thread's memcpy, memmove, memset and
// memcmp calls are counted by call site from now on (one marked thread: the native renderer's ring thread).
void MarkThread();
// From the marked thread, every 10 s: the call sites with the most calls and bytes to the log. Does nothing in other
// builds.
void Report();

}  // namespace bo2zm::memcpy_callers
