# Build environment for the Linux ARM64 (AArch64) build on an x86_64 machine: Ubuntu 24.04 with clang 20 and lld,
# CMake, Ninja, and the AArch64 sysroot from tools/linux/make_sysroot.sh baked into the image.
#
#   docker build -t bo2zm-linux-arm64 -f tools/linux/Dockerfile tools/linux
#   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" bo2zm-linux-arm64
#
# The second command runs games/bo2/build_linux.sh on the checkout mounted at /src (with games/bo2/generated and sdk/thirdparty
# already in place); the package ends up in games/bo2/out/linux-arm64/package. docs/building-linux-arm64.md has the rest.
FROM ubuntu:24.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates clang-20 lld-20 llvm-20 cmake ninja-build pkg-config python3 libwayland-bin mmdebstrap file \
 && rm -rf /var/lib/apt/lists/*

COPY make_sysroot.sh sysroot-packages.txt /opt/bo2zm/tools/linux/
RUN bash /opt/bo2zm/tools/linux/make_sysroot.sh /opt/bo2zm/sysroot

ENV BO2_LINUX_SYSROOT=/opt/bo2zm/sysroot \
    CLANG_SUFFIX=-20 \
    HOME=/tmp
WORKDIR /src
CMD ["bash", "games/bo2/build_linux.sh"]
