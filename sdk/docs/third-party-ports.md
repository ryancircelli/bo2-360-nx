# Code ported from other ReXGlue projects

Some changes in this SDK were ported from patches that other ReXGlue
game ports carry against the SDK. Each commit names its source patch in
its message. The notices below are kept as those projects' licenses
require.

## Skullburglar

Source: https://github.com/nlwstein/skullburglar (commit 67aacb2),
`patches/rexglue-sdk/`.

Ported changes:

- Unlinking the guest shared memory object right after creating it (0022)
- Not recreating the presenter's final effect pipeline every frame (0006)
- Per-stage FMA contraction control for the SPIR-V translator (0015)
- Moving shared memory uploads out of open render passes (0003)
- Rewinding append-mode files on POSIX (0008)
- Background creation of stored Vulkan pipelines (0008)
- POSIX timed waits without the last-millisecond spin (0014)
- The word count for the 16-in-32 copy swap (0026)
- Sleeping in the timer queue while nothing is due (0029)
- Non-Space fullscreen on macOS (0006)

```
BSD 3-Clause License

Copyright (c) 2026, the Skullburglar contributors

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The patches in patches/rexglue-sdk/ modify the ReXGlue SDK and are also covered
by its license (BSD 3-Clause, Copyright (c) 2026, Tom Clay; portions derived from
the Xenia project).
```

## Fable II Recomp for macOS

Source: https://github.com/andrew17182/Fable-2-Recomp-macOS (commit
6f5459e), `patches/sdk/`.

Ported changes:

- Per-architecture FPSCR rounding mode table (fpscr-rounding-per-arch)
- Unlinking the guest shared memory object right after creating it
  (shm-unlink-on-create, same fix as Skullburglar 0022)

```
MIT License

Copyright 2026 Himdo
Copyright 2026 Fable II Recomp for macOS contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

This license covers the code in this repository only. Fable II is © Microsoft / Lionhead Studios; no game
content is included. Third-party components keep their own licenses (see backends/metal/licenses/ and
thirdparty/).
```

## GTA V recomp

The MoltenVK defaults (`MVK_CONFIG_FAST_MATH_ENABLED=0`,
`MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION=1`) follow findings
documented by https://github.com/andrewnakas/gtav-recomp. No code was
copied from it, so no notice is needed.

## In bo2-360-nx's vendored copy

Taken into this sdk/ (by cherry-pick from the mac-perf branch): the shared memory unlink, the arm64 FPSCR rounding
table, the 16-in-32 copy swap word count, the append-mode rewind, non-Space fullscreen and the MoltenVK precise math
default. Not taken because this copy already had its own fix from the Horizon port: the POSIX timed wait spin and the
timer queue sleep. The Xenos (SDK renderer) changes are not taken: bo2-360-nx draws with its own native renderer.
