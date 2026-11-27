#pragma once
// The plugin's compat header assumes Apple arm64; the sweep also runs on x86-64 Linux (the laptop).
#include "../../../src/graphics/metal/compat.h"
#if !defined(__aarch64__)
#undef XE_ARCH_ARM64
#undef XE_TARGET_ARM64
#undef XE_ARCH_AMD64
#define XE_ARCH_ARM64 0
#define XE_TARGET_ARM64 0
#define XE_ARCH_AMD64 1
#endif
