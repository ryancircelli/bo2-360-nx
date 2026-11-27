/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless a POSIX/XSI feature level
// is requested before <ucontext.h> is included. fiber.h includes ucontext.h,
// so the feature macro must be set in this translation unit first.
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE  // keeps MAP_ANON visible under _XOPEN_SOURCE
#endif

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/thread/fiber.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cassert>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::ForThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  // Guard page below the stack: an overflow faults instead of silently corrupting the heap.
  const size_t page = size_t(sysconf(_SC_PAGESIZE));
  stack_size = (stack_size + page - 1) & ~(page - 1);
  void* mem = mmap(nullptr, stack_size + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mem == MAP_FAILED) {
    delete f;
    return nullptr;
  }
  mprotect(mem, page, PROT_NONE);
  f->stack_ = mem;
  f->stack_size_ = stack_size + page;

  if (getcontext(&f->context_) == -1) {
    munmap(f->stack_, f->stack_size_);
    delete f;
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = static_cast<uint8_t*>(mem) + page;
  f->context_.uc_stack.ss_size = stack_size;
  f->context_.uc_link = nullptr;
  // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before swapcontext returned here.
  Fiber* f = tls_current_;
  f->started_ = true;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  swapcontext(&from->context_, &target->context_);
}

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    // Only clear the current fiber if it is this one (rexglue-sdk 6937495).
    if (tls_current_ == this) {
      tls_current_ = nullptr;
    }
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // No POSIX equivalent of ConvertFiberToThread.
  if (stack_) {
    munmap(stack_, stack_size_);
  }
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
