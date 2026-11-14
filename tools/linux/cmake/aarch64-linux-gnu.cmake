# CMake toolchain file: AArch64 Linux (glibc) from an x86_64 Linux host, with LLVM clang and lld and the sysroot that
# tools/linux/make_sysroot.sh builds. games/bo2/build_linux.sh passes it; an ARM64 Linux machine builds natively without it.
#
# The sysroot comes from BO2ZM_LINUX_SYSROOT (environment or cache). Clang 19 or newer (games/bo2/build_linux.sh says why);
# CLANG_SUFFIX picks a versioned install (e.g. -20 for clang-20).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(DEFINED ENV{BO2ZM_LINUX_SYSROOT})
  set(BO2ZM_LINUX_SYSROOT "$ENV{BO2ZM_LINUX_SYSROOT}" CACHE PATH "AArch64 sysroot (tools/linux/make_sysroot.sh)")
endif()
if(NOT BO2ZM_LINUX_SYSROOT OR NOT EXISTS "${BO2ZM_LINUX_SYSROOT}/usr/include/stdio.h")
  message(FATAL_ERROR "BO2ZM_LINUX_SYSROOT must name the sysroot made by tools/linux/make_sysroot.sh")
endif()
# try_compile projects run this file again in a fresh CMake: hand the sysroot on to them.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES BO2ZM_LINUX_SYSROOT)

set(CMAKE_SYSROOT "${BO2ZM_LINUX_SYSROOT}")
set(_bo2zm_triple aarch64-linux-gnu)
set(CMAKE_C_COMPILER "clang$ENV{CLANG_SUFFIX}")
set(CMAKE_CXX_COMPILER "clang++$ENV{CLANG_SUFFIX}")
set(CMAKE_ASM_COMPILER "clang$ENV{CLANG_SUFFIX}")
set(CMAKE_C_COMPILER_TARGET ${_bo2zm_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_bo2zm_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_bo2zm_triple})
find_program(CMAKE_AR NAMES "llvm-ar$ENV{CLANG_SUFFIX}" llvm-ar REQUIRED)
find_program(CMAKE_RANLIB NAMES "llvm-ranlib$ENV{CLANG_SUFFIX}" llvm-ranlib REQUIRED)
find_program(CMAKE_STRIP NAMES "llvm-strip$ENV{CLANG_SUFFIX}" llvm-strip)
find_program(CMAKE_OBJCOPY NAMES "llvm-objcopy$ENV{CLANG_SUFFIX}" llvm-objcopy)

# lld: GNU ld from the host knows nothing about AArch64.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")

# Libraries, headers and CMake packages from the sysroot only; programs (python, wayland-scanner) from the host.
set(CMAKE_FIND_ROOT_PATH "${BO2ZM_LINUX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config (SDL3, the SDK's X11/Wayland checks) reads the sysroot's .pc files and prefixes their paths with it.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${BO2ZM_LINUX_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${BO2ZM_LINUX_SYSROOT}/usr/lib/${_bo2zm_triple}/pkgconfig:${BO2ZM_LINUX_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
