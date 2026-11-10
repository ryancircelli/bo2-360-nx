// Native renderer, Metal backend: samplers from gpu::SamplerDesc, whose values are in Vulkan's numbering (see
// bo2zm_gpu_metal.h).

#include "bo2zm_gpu_metal.h"

#include <rex/logging.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>

namespace bo2zm::native::gpu::metal {

namespace {

MTLSamplerMinMagFilter MinMag(Filter f) {
  return f == kFilterLinear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
}

MTLSamplerAddressMode Address(AddressMode m) {
  switch (m) {
    case kAddressMirroredRepeat:
      return MTLSamplerAddressModeMirrorRepeat;
    case kAddressClampToEdge:
      return MTLSamplerAddressModeClampToEdge;
    case kAddressClampToBorder:
      return MTLSamplerAddressModeClampToBorderColor;
    case kAddressMirrorClampToEdge:
      return MTLSamplerAddressModeMirrorClampToEdge;
    case kAddressRepeat:
    default:
      return MTLSamplerAddressModeRepeat;
  }
}

}  // namespace

Sampler Device::CreateSampler(const SamplerDesc& desc) {
  @autoreleasepool {
    MTLSamplerDescriptor* d = [[MTLSamplerDescriptor alloc] init];
    d.magFilter = MinMag(desc.mag);
    d.minFilter = MinMag(desc.min);
    // Vulkan always has a mip filter; mipmapped images sampled with "nearest" pick the nearest level, which is
    // Metal's Nearest (NotMipmapped would read level 0 only).
    d.mipFilter = desc.mip == kFilterLinear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    d.sAddressMode = Address(desc.u);
    d.tAddressMode = Address(desc.v);
    d.rAddressMode = Address(desc.w);
    d.borderColor = desc.border == kBorderOpaqueWhite ? MTLSamplerBorderColorOpaqueWhite
                                                      : MTLSamplerBorderColorTransparentBlack;
    d.maxAnisotropy = desc.anisotropy ? NSUInteger(std::clamp(desc.anisotropy_max, 1.0f, 16.0f)) : 1;
    d.lodMinClamp = desc.lod_min;
    d.lodMaxClamp = desc.lod_max;
    d.normalizedCoordinates = YES;
    d.supportArgumentBuffers = YES;  // the samplers live in the bindless sampler heap
    if (desc.lod_bias != 0.0f) {
      if (@available(macOS 26.0, *)) {
        d.lodBias = desc.lod_bias;
      } else {
        // Before macOS 26 a sampler has no LOD bias; the texture is sampled unbiased (slightly sharper or
        // blurrier mips than the game asked for).
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
          REXLOG_WARN("[native] Metal: sampler LOD bias needs macOS 26; sampling without it");
        }
      }
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)native();
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:d];
    if (!sampler) {
      return nullptr;
    }
    return reinterpret_cast<Sampler>((__bridge_retained void*)sampler);
  }
}

void Device::Release(Sampler sampler) {
  if (sampler) {
    id<MTLSamplerState> released = (__bridge_transfer id<MTLSamplerState>)reinterpret_cast<void*>(sampler);
    released = nil;
  }
}

uint64_t Device::SamplerId(Sampler sampler) {
  id<MTLSamplerState> s = (__bridge id<MTLSamplerState>)reinterpret_cast<void*>(sampler);
  return s.gpuResourceID._impl;
}

}  // namespace bo2zm::native::gpu::metal
