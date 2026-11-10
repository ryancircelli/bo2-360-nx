// Native renderer, Metal backend: buffers in shared storage and the guest's memory as a buffer (see
// bo2zm_gpu_metal.h).

#include "bo2zm_gpu_metal.h"

#include <rex/logging.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <unistd.h>

#if !__has_feature(objc_arc)
#error "bo2zm_gpu_metal_memory.mm is built with -fobjc-arc"
#endif

namespace bo2zm::native::gpu::metal {

namespace {

// Hazard tracking stays on: the recorder does not yet place its own fences between passes (phase 5 decides).
constexpr MTLResourceOptions kShared = MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;

void Fill(id<MTLBuffer> buffer, HostBuffer& out) {
  out.data = static_cast<uint8_t*>(buffer.contents);
  out.address = buffer.gpuAddress;
  out.size = buffer.length;
  out.buffer = reinterpret_cast<Buffer>((__bridge_retained void*)buffer);
}

}  // namespace

bool Device::CreateHostBuffer(Size size, HostBuffer& out) {
  @autoreleasepool {
    id<MTLDevice> device = (__bridge id<MTLDevice>)native();
    id<MTLBuffer> buffer = [device newBufferWithLength:NSUInteger(size) options:kShared];
    if (!buffer) {
      REXLOG_ERROR("[native] Metal: no buffer of {} bytes", size);
      return false;
    }
    Fill(buffer, out);
    return true;
  }
}

bool Device::WrapHostMemory(uint8_t* base, Size size, HostBuffer& out) {
  @autoreleasepool {
    const Size page = Size(getpagesize());
    if (!base || size == 0 || (reinterpret_cast<uintptr_t>(base) % page) != 0 || (size % page) != 0) {
      REXLOG_ERROR("[native] Metal: guest memory at {} ({} bytes) is not page aligned", static_cast<void*>(base),
                   size);
      return false;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)native();
    if (size > device.maxBufferLength) {
      REXLOG_ERROR("[native] Metal: {} MB of guest memory is more than one buffer can hold ({} MB)", size >> 20,
                   Size(device.maxBufferLength) >> 20);
      return false;
    }
    id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:base length:NSUInteger(size) options:kShared
                                                deallocator:nil];
    if (!buffer) {
      REXLOG_ERROR("[native] Metal: guest memory at {} ({} MB) could not be wrapped", static_cast<void*>(base),
                   size >> 20);
      return false;
    }
    buffer.label = @"bo2zm guest memory";
    Fill(buffer, out);
    return true;
  }
}

void Device::Release(HostBuffer& buffer) {
  if (buffer.buffer) {
    id<MTLBuffer> released = (__bridge_transfer id<MTLBuffer>)reinterpret_cast<void*>(buffer.buffer);
    released = nil;
  }
  buffer = HostBuffer{};
}

}  // namespace bo2zm::native::gpu::metal
