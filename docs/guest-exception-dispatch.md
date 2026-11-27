# Guest exception dispatch and unwinding

Status (2026-11-27): runtime half in `sdk/src/kernel/xboxkrnl/guest_exception.cpp`,
behind the `guest_exception_dispatch` cvar (default off). Codegen half planned
below, not written. With the cvar off every export behaves as before.

## What the guest expects

Xbox 360 titles built with MSVC use table-based structured exception handling. A
`throw` (code `0xE06D7363`) or any `RtlRaiseException` call asks the kernel to
capture the caller's registers, walk the guest stack over `.pdata` (virtual
unwind), call each frame's language handler (`__C_specific_handler`,
`__CxxFrameHandler`), and, when one accepts, `RtlUnwind` calls the handlers of
the frames in between and resumes the target in the middle of a function.

## Why not Canary's guest-stack walk

Xenia Canary f21fc2dc4 (BSD-3) walks the guest stack and restores a guest
context; a JIT can resume at any address. We cannot, for two reasons:

- Codegen keeps r14–r31/f14–f31 in host locals (`non_volatile_as_local`) and
  elides the `__savegprlr`/`__restgprlr` helper calls, so the guest stack holds
  neither the saved non-volatiles nor, for helper-saving functions, the return
  address. A virtual unwind cannot recover the callers.
- Static code cannot jump into the middle of a function.

We take Canary's structure layouts, flag values and the `__C_specific_handler`
scope walk, and nothing else.

## What we do instead

Every function with a `.pdata` language handler registers a `HandlerFrame` on a
thread-local chain (its `.pdata` entry, establisher `r1`, current call site, and
pointers to its localized non-volatiles). The REX_EH_* macros in
`guest_exception.h` are the codegen protocol.

- `RtlRaiseException`: offers the record to each registered frame's handler,
  innermost first, with the owner's non-volatiles loaded into `ctx` (handlers and
  funclets are compiled as register-sharing and read them from `ctx`). The
  guest structures (context, dispatcher context, exception pointers) are carved
  below the caller's `r1`.
- `RtlUnwind`: runs the unwind handlers of every frame down to the target, then
  either throws `GuestUnwind` to the target frame (target ip = a label in that
  function, the `__C_specific_handler` form) or returns, when the target ip is
  RtlUnwind's own return address (the C++ runtime form, see below).
- The target function catches only its own `GuestUnwind`, sets `r1` to its
  establisher frame and `r3` to the return value, and `goto`s the continuation
  label through a switch keyed by guest address. C++ catch keeps the target's
  host locals current (a `longjmp` would leave modified locals indeterminate).
- `ResumeContext`: the C++ runtime's restore-context helper. FH1's CRT
  (`sub_830FC980`, `_UnwindNestedFrames`) captures its context, calls RtlUnwind
  with target ip = its own return address, runs the catch funclet, then calls
  `sub_830FC89C` with an `X_CONTEXT` whose `r1` is the target's establisher and
  whose `Iar` is the continuation. Codegen replaces that helper with
  `ResumeContext`, which throws to the frame with that establisher.

Functions without a handler cost nothing; functions with one pay a frame push,
a store per call site, and a try block.

## Not handled (logged, not silent)

- Guest hardware faults or traps inside `__try` are not turned into exceptions.
- `RtlVirtualUnwind` and `RtlLookupFunctionEntry` stay stubs: no title imports
  them (checked all thirteen images), and a guest-stack walk is unreliable here.
- Exit unwinds (target frame 0), collided unwinds, and frames from code generated
  without exception frames: `RtlUnwind` logs and returns.
- Host frames between the raise and the target that restore state by hand (not
  RAII) are skipped. The Windows (MSVC) build needs `/EHs` for C++ exceptions to
  pass the `extern "C"` hooks.
- Vector non-volatiles are not handed to funclets; a funclet's writes to the
  owner's register non-volatiles are dropped (MSVC funclets write locals through
  the frame pointer, not registers). `RtlCaptureContext` reports stale values for
  localized non-volatiles and does not fill `fpscr`.
- Between the C++ runtime's unwind and its `ResumeContext`, the unwound frames are
  still on the chain; a second raise from inside the catch funclet would see them.

## Codegen plan (not done)

Only functions whose `.pdata` entry has a handler (kind 3) are touched.

1. Frame: `REX_EH_FRAME(pdata_entry)` at entry, `__ehf.gpr[i] = &rN` for each
   localized non-volatile, `REX_EH_ESTABLISH()` after the prolog's `stwu r1`,
   `REX_EH_CALLSITE(address)` before each `bl`/`bctrl`.
2. Resume labels: SEH scope `jump_target`s (scope table at `begin - 4`) and C++
   catch continuations (the constant addresses the title's catch funclets
   return, from the `FuncInfo` try-block maps already parsed in
   `phase_register.cpp`). Functions with resume labels get
   `REX_EH_BODY_BEGIN case 0x...: goto loc_...; } ... REX_EH_BODY_END`; an
   unexpected address hits `BadResume` (log + abort).
3. Restore-context helper: detect the CRT routine that loads every register from
   `r4` as an `X_CONTEXT` and branches to its `Iar` (FH1 `sub_830FC89C`; find the
   equivalent per title by that pattern or the `_UnwindNestedFrames` caller) and
   emit a call to `guest_eh::ResumeContext` instead.
4. Gate: per-game counts below are the expected emission sizes; regenerate one
   C++ title with catches (FH1 or Fallout 3) and replay.

## Per-game counts (functions with a .pdata handler)

| image | .pdata | with handler | C++ | C++ with try | SEH | SEH with __except | functions with resume labels |
|---|---|---|---|---|---|---|---|
| bo2 | 22873 | 421 | 357 | 0 | 64 | 20 | 20 |
| bo1 | 14122 | 210 | 166 | 0 | 44 | 18 | 18 |
| nfsmw | 22679 | 66 | 0 | 0 | 66 | 0 | 0 |
| fh1 | 57127 | 356 | 298 | 40 | 58 | 21 | 61 |
| fh1-speech | 4775 | 121 | 69 | 0 | 52 | 15 | 15 |
| fh1-xmedia | 2104 | 33 | 0 | 0 | 33 | 18 | 18 |
| gears | 25239 | 833 | 768 | 0 | 65 | 30 | 30 |
| ac1 | 35276 | 531 | 478 | 0 | 53 | 17 | 17 |
| fallout3 | 57885 | 156 | 93 | 87 | 63 | 21 | 108 |
| me2 | 43693 | 124 | 59 | 0 | 65 | 26 | 26 |
| halo3 | 15831 | 131 | 75 | 48 | 56 | 19 | 67 |
| nfshp | 30913 | 107 | 0 | 0 | 107 | 0 | 0 |
| nfsmw2012 | 31335 | 151 | 0 | 0 | 151 | 0 | 0 |

C++ functions without a try block only destroy locals during unwind
(`cxx_unwind_only`); SEH functions without `__except` only have `__finally`.
