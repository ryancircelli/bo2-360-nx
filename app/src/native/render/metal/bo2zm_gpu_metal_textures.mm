// Native renderer, Metal backend: images and the placement heaps the texture pool carves them from (see
// bo2zm_gpu_metal.h).

#include "bo2zm_gpu_metal.h"

#include <rex/logging.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>

#if !__has_feature(objc_arc)
#error "bo2zm_gpu_metal_textures.mm is built with -fobjc-arc"
#endif

namespace bo2zm::native::gpu::metal {

namespace {

// The texture descriptor of an image, or nil if its format has no Metal equivalent. A Vulkan image is 2D or 3D with
// layers; Metal fixes the kind at creation: 2D, 2D array, cube, cube array or 3D.
MTLTextureDescriptor* Descriptor(const ImageDesc& desc) {
  const MTLPixelFormat format = MTLPixelFormat(PixelFormat(desc.format));
  if (format == MTLPixelFormatInvalid) {
    return nil;
  }
  MTLTextureDescriptor* d = [[MTLTextureDescriptor alloc] init];
  d.pixelFormat = format;
  d.width = desc.width;
  d.height = desc.height;
  d.depth = desc.volume ? desc.depth : 1;
  d.mipmapLevelCount = desc.levels;
  if (desc.volume) {
    d.textureType = MTLTextureType3D;
  } else if (desc.cube) {
    d.textureType = desc.layers > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
    d.arrayLength = std::max<uint32_t>(desc.layers / 6, 1);
  } else if (desc.layers > 1) {
    d.textureType = MTLTextureType2DArray;
    d.arrayLength = desc.layers;
  } else {
    d.textureType = MTLTextureType2D;
  }
  MTLTextureUsage usage = 0;
  if (desc.usage & kImageSampled) {
    usage |= MTLTextureUsageShaderRead;
  }
  if (desc.usage & (kImageColorTarget | kImageDepthTarget)) {
    usage |= MTLTextureUsageRenderTarget;
  }
  // The renderer makes views in other formats of the same size (resolves, sRGB). Allowed everywhere for now; the
  // command recording (phase 5) narrows it to the images that need it, since it can cost lossless compression.
  usage |= MTLTextureUsagePixelFormatView;
  d.usage = usage;
  d.storageMode = MTLStorageModePrivate;
  return d;
}

}  // namespace

Requirements Device::TextureRequirements(const ImageDesc& desc) {
  @autoreleasepool {
    MTLTextureDescriptor* d = Descriptor(desc);
    if (!d) {
      return Requirements{};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)native();
    const MTLSizeAndAlign sa = [device heapTextureSizeAndAlignWithDescriptor:d];
    return Requirements{Size(sa.size), Size(sa.align), 1u};
  }
}

Memory Device::CreateHeap(Size bytes) {
  @autoreleasepool {
    id<MTLDevice> device = (__bridge id<MTLDevice>)native();
    MTLHeapDescriptor* d = [[MTLHeapDescriptor alloc] init];
    d.type = MTLHeapTypePlacement;
    d.storageMode = MTLStorageModePrivate;
    d.hazardTrackingMode = MTLHazardTrackingModeTracked;
    d.size = NSUInteger(bytes);
    id<MTLHeap> heap = [device newHeapWithDescriptor:d];
    if (!heap) {
      REXLOG_ERROR("[native] Metal: no heap of {} MB", bytes >> 20);
      return nullptr;
    }
    heap.label = @"bo2zm textures";
    return reinterpret_cast<Memory>((__bridge_retained void*)heap);
  }
}

void Device::ReleaseHeap(Memory heap) {
  if (heap) {
    id<MTLHeap> released = (__bridge_transfer id<MTLHeap>)reinterpret_cast<void*>(heap);
    released = nil;
  }
}

Image Device::CreatePlacedImage(Memory heap, Size offset, const ImageDesc& desc) {
  @autoreleasepool {
    MTLTextureDescriptor* d = Descriptor(desc);
    if (!d || !heap) {
      return nullptr;
    }
    id<MTLHeap> h = (__bridge id<MTLHeap>)reinterpret_cast<void*>(heap);
    id<MTLTexture> texture = [h newTextureWithDescriptor:d offset:NSUInteger(offset)];
    if (!texture) {
      return nullptr;
    }
    return reinterpret_cast<Image>((__bridge_retained void*)texture);
  }
}

Image Device::CreateImage(const ImageDesc& desc) {
  @autoreleasepool {
    MTLTextureDescriptor* d = Descriptor(desc);
    if (!d) {
      return nullptr;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)native();
    id<MTLTexture> texture = [device newTextureWithDescriptor:d];
    if (!texture) {
      return nullptr;
    }
    return reinterpret_cast<Image>((__bridge_retained void*)texture);
  }
}

void Device::Release(Image image) {
  if (image) {
    id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)reinterpret_cast<void*>(image);
    released = nil;
  }
}

}  // namespace bo2zm::native::gpu::metal
