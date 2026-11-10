// Exercises the native renderer's Metal backend outside the game: the device and its capabilities, every image
// format in the format table as a texture placed in a heap, the image kinds the renderer makes, packing textures
// into a heap the way the texture pool does, and buffers (new ones and wrapped host memory, read and written by the
// GPU).
//
// Build (from the repository root; the Vulkan headers only provide the format numbering):
//   xcrun clang++ -std=c++20 -fobjc-arc -O1 -Itools/metal/stub -Iapp/src/native/render \
//     -Isdk/thirdparty/vulkan-headers/include tools/metal/check_backend.mm \
//     app/src/native/render/metal/bo2zm_gpu_metal.mm app/src/native/render/metal/bo2zm_gpu_metal_formats.mm \
//     app/src/native/render/metal/bo2zm_gpu_metal_memory.mm app/src/native/render/metal/bo2zm_gpu_metal_textures.mm \
//     -framework Metal -framework Foundation -o check_backend
// Exit status 1 if any check fails.

#include "metal/bo2zm_gpu_metal.h"

#include <vulkan/vulkan_core.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <sys/mman.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace bo2zm::native;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  failures += !ok;
}

}  // namespace

int main() {
  @autoreleasepool {
    auto device = gpu::metal::Device::Create(std::filesystem::temp_directory_path());
    if (!device) {
      std::printf("FAIL no Metal device the renderer can use\n");
      return 1;
    }
    const gpu::Caps& c = device->caps();
    std::printf("%s: uniform alignment %llu, depth format %u, max anisotropy %.0f, dynamic state %d/%d\n",
                device->name().c_str(), (unsigned long long)c.uniform_alignment, c.depth_format, c.max_anisotropy,
                c.dynamic_state12, c.dynamic_state3);

    // Every format of the table, as a 64x64 sampled texture placed at offset 0 of a heap.
    gpu::Memory heap = device->CreateHeap(gpu::Size(64) << 20);
    Check(heap != nullptr, "64 MB placement heap");
    int mapped = 0, placed = 0;
    for (uint32_t f = 1; f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; ++f) {
      if (gpu::metal::PixelFormat(f) == 0) {
        continue;
      }
      ++mapped;
      gpu::ImageDesc desc;
      desc.format = f;
      desc.width = desc.height = 64;
      const bool depth = f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT;
      desc.usage = gpu::kImageSampled | gpu::kImageCopyTarget | (depth ? gpu::kImageDepthTarget : 0);
      const gpu::Requirements r = device->TextureRequirements(desc);
      gpu::Image image = r.size ? device->CreatePlacedImage(heap, 0, desc) : nullptr;
      if (image) {
        ++placed;
      } else {
        std::printf("FAIL format %u (MTLPixelFormat %u) could not be placed\n", f, gpu::metal::PixelFormat(f));
        ++failures;
      }
      gpu::metal::Device::Release(image);
    }
    std::printf("%d of %d mapped formats placed in a heap\n", placed, mapped);

    // The image kinds.
    struct Kind {
      const char* what;
      gpu::ImageDesc desc;
    };
    gpu::ImageDesc d2;
    d2.format = VK_FORMAT_R8G8B8A8_UNORM;
    d2.width = d2.height = 256;
    d2.levels = 9;
    gpu::ImageDesc array = d2;
    array.layers = 4;
    gpu::ImageDesc cube = d2;
    cube.cube = true;
    cube.layers = 6;
    gpu::ImageDesc cubes = cube;
    cubes.layers = 12;
    gpu::ImageDesc volume = d2;
    volume.volume = true;
    volume.depth = 32;
    volume.levels = 1;
    gpu::ImageDesc target = d2;
    target.width = 1280;
    target.height = 720;
    target.levels = 1;
    target.format = c.depth_format;
    target.usage = gpu::kImageSampled | gpu::kImageDepthTarget | gpu::kImageCopySource;
    gpu::ImageDesc color = target;
    color.format = VK_FORMAT_B8G8R8A8_UNORM;
    color.usage = gpu::kImageSampled | gpu::kImageColorTarget | gpu::kImageCopySource;
    for (const Kind& k : {Kind{"2D with 9 mips", d2}, Kind{"2D array of 4", array}, Kind{"cube", cube},
                          Kind{"cube array of 2", cubes}, Kind{"3D 256x256x32", volume},
                          Kind{"720p depth target (dedicated)", target},
                          Kind{"720p color target (dedicated)", color}}) {
      const bool dedicated = k.desc.usage & (gpu::kImageColorTarget | gpu::kImageDepthTarget);
      gpu::Image image = dedicated ? device->CreateImage(k.desc) : device->CreatePlacedImage(heap, 0, k.desc);
      Check(image != nullptr, k.what);
      gpu::metal::Device::Release(image);
    }

    // Packing like the texture pool: 64 KB units, each texture at the next multiple of its alignment.
    {
      gpu::ImageDesc t = d2;
      t.width = t.height = 128;
      t.levels = 8;
      t.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
      const gpu::Requirements r = device->TextureRequirements(t);
      gpu::Size offset = 0;
      int count = 0;
      std::vector<gpu::Image> images;
      while (offset + r.size <= (gpu::Size(64) << 20) && count < 256) {
        gpu::Image image = device->CreatePlacedImage(heap, offset, t);
        if (!image) {
          break;
        }
        images.push_back(image);
        ++count;
        offset += (r.size + 0xFFFF) & ~gpu::Size(0xFFFF);
      }
      std::printf("pool packing: %d BC1 128x128 textures of %llu bytes (alignment %llu) in 64 KB units\n", count,
                  (unsigned long long)r.size, (unsigned long long)r.alignment);
      Check(count == 256, "256 textures placed side by side");
      for (gpu::Image image : images) {
        gpu::metal::Device::Release(image);
      }
    }
    gpu::metal::Device::ReleaseHeap(heap);

    // Buffers: a new one, and host memory wrapped in place, copied between by the GPU.
    gpu::metal::HostBuffer fresh;
    Check(device->CreateHostBuffer(1 << 20, fresh) && fresh.data && fresh.address, "1 MB shared buffer");
    const size_t host_bytes = size_t(64) << 20;
    void* host = mmap(nullptr, host_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    gpu::metal::HostBuffer wrapped;
    Check(host != MAP_FAILED && device->WrapHostMemory(static_cast<uint8_t*>(host), host_bytes, wrapped) &&
              wrapped.data == host,
          "64 MB of host memory wrapped without a copy");
    if (wrapped.buffer && fresh.buffer) {
      for (size_t i = 0; i < (1 << 20); ++i) {
        wrapped.data[(32 << 20) + i] = uint8_t(i * 13);
      }
      id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)device->queue();
      id<MTLCommandBuffer> commands = [queue commandBuffer];
      id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
      id<MTLBuffer> from = (__bridge id<MTLBuffer>)reinterpret_cast<void*>(wrapped.buffer);
      id<MTLBuffer> to = (__bridge id<MTLBuffer>)reinterpret_cast<void*>(fresh.buffer);
      [blit copyFromBuffer:from sourceOffset:32 << 20 toBuffer:to destinationOffset:0 size:1 << 20];
      [blit fillBuffer:from range:NSMakeRange(48 << 20, 4096) value:0x5A];
      [blit endEncoding];
      [commands commit];
      [commands waitUntilCompleted];
      Check(std::memcmp(fresh.data, wrapped.data + (32 << 20), 1 << 20) == 0, "GPU reads the CPU's writes in place");
      Check(wrapped.data[(48 << 20) + 17] == 0x5A, "the CPU reads the GPU's writes in place");
    }
    gpu::metal::Device::Release(wrapped);
    gpu::metal::Device::Release(fresh);
    munmap(host, host_bytes);

    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
  }
}
