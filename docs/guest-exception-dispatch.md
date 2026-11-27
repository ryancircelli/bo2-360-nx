# Guest exception dispatch and unwinding: design note

Status: not implemented. No shipped game needs it yet (2026-11-27).

## What the guest expects

Xbox 360 titles built with MSVC use table-based structured exception handling. A
`throw` (code `0xE06D7363`) or any `RtlRaiseException` call asks the kernel to:

1. Capture the caller's registers into a context record (`RtlCaptureContext`).
2. Walk the guest stack using `.pdata` (function start, prolog length, length)
   and the prolog's saved-register layout (virtual unwind).
3. Call each frame's language handler (`__C_specific_handler`, `__CxxFrameHandler`)
   with a dispatcher context.
4. When a handler accepts, `RtlUnwind` calls the handlers of the frames in between
   with the unwind flag, then loads the target context and resumes at its `Iar`:
   the catch funclet's continuation, somewhere in the middle of a function.

## What we do

`RtlRaiseException` handles the thread-name exception (`0x406D1388`) and logs a
C++ throw's type before `rex::debug::Break`. `RtlUnwind`, `RtlCaptureContext` and
`__C_specific_handler` are warning stubs (`sdk/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp`).
Shipped titles (BO2, NFS Most Wanted, BO1) never throw on the paths we run. The
Forza Horizon 1 exit at the first Start press looked like a throw but is not: the
thrown-type log stays silent and the trap is the game's own fatal-error routine
(`sub_82C09F00`).

## What Xenia Canary f21fc2dc4 does (BSD-3, read 2026-11-27)

`exception_dispatch.cc` implements the steps above for the JIT: `X_CONTEXT`
(0xA40 bytes), `.pdata` lookup, virtual unwind, a kernel frame below the guest
stack for calling guest handlers, `DispatchException`, `Unwind`, and
`RestoreContext`, which sets the guest PC so the JIT resumes at the target `Iar`.

## Why it does not port as is

Steps 1 to 3 port directly: the context layout, `.pdata` walk and virtual unwind
are data work, and guest handlers are recompiled functions we can call. Step 4
does not: statically recompiled code cannot resume at an arbitrary guest address
inside a function. That is the same problem as Forza's CRT continuation
`sub_830FC89C` (`ld r1,32(r4)` full context restore, called from `sub_830F85C0`).

## If a game needs it

- Codegen: mark every address a handler can return to (catch continuations from
  the title's `FuncInfo` tables, plus the CRT continuation helper) as a labelled
  entry, and give each function containing one a dispatch switch keyed by guest
  address, entered through a host `setjmp` taken at function entry, the way
  `ppc_setjmp` / `ppc_longjmp` already work.
- Kernel: port Canary's context capture, `.pdata` walk and handler calls; replace
  `RestoreContext` with a host `longjmp` to the frame that owns the target `r1`,
  then jump to the labelled continuation.
- Gate: a guest test title that throws through three frames and catches, plus the
  first game route that reaches a real throw.

Until then the stubs stay; a real throw stops at `rex::debug::Break` with its type
logged, which is the signal to start this work.
