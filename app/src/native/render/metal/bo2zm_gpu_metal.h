// Native renderer, Metal backend (BO2ZM_NATIVE_METAL): the device and the shader libraries.
//
// The shaders are never compiled at runtime from source: tools/metal turns the shader pack into sharded .metallib
// files ahead of time (bo2zm_<first hex digit of the hash>.metallib, one function per shader and variant, named
// vs_<hash16>, ps_<hash16> and ps_<hash16>_ez). This header has no Objective-C in it.

#pragma once

#include "../bo2zm_gpu.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace bo2zm::native::gpu::metal {

// The MTLPixelFormat of an image format (gpu::Format, Vulkan's numbering), as its integer value; 0
// (MTLPixelFormatInvalid) if Metal has none. Apple GPUs have no 24-bit depth: D24S8 and X8_D24 become 32-bit float
// depth, which is what the renderer's D32S8 fallback already draws with on other drivers.
uint32_t PixelFormat(Format format);
// The MTLVertexFormat of a vertex attribute format, as its integer value; 0 (MTLVertexFormatInvalid) if none.
uint32_t VertexFormat(Format format);

// A buffer in shared storage: the CPU writes it through `data` and the shaders read it at `address`. Shared storage
// is coherent on Apple GPUs, so the Vulkan backend's Map, Flush and Invalidate have nothing to do here.
struct HostBuffer {
  Buffer buffer = nullptr;  // an id<MTLBuffer>, retained; Device::Release gives it back
  uint8_t* data = nullptr;
  Address address = 0;
  Size size = 0;
};

class Device {
 public:
  // nullptr if there is no Metal device that can run the renderer (the reason is logged): it needs tier 2
  // argument buffers (the bindless heaps) and the Apple7 family or Metal 3.
  static std::unique_ptr<Device> Create(const std::filesystem::path& library_folder);
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const std::string& name() const { return name_; }
  // What the device offers the renderer, in the same terms as the Vulkan backend's CapsOf.
  const Caps& caps() const { return caps_; }
  // The id<MTLDevice>, for the Objective-C++ parts of the backend.
  void* native() const;
  // The id<MTLCommandQueue> every command buffer goes to (rendering and presentation).
  void* queue() const;

  // A new shared-storage buffer of `size` bytes (any usage: Metal buffers are not typed). False if Metal refuses.
  bool CreateHostBuffer(Size size, HostBuffer& out);
  // The guest's memory itself as a buffer, without a copy: `base` and `size` must be multiples of the host page
  // size, and the range must stay mapped while the buffer lives. The guest's physical memory (a shared memory
  // mapping in the SDK's arena on macOS) qualifies, so the GPU can read guest vertices, indices and textures, and
  // write resolves, in place. False (logged) otherwise.
  bool WrapHostMemory(uint8_t* base, Size size, HostBuffer& out);
  // Gives back a buffer from CreateHostBuffer or WrapHostMemory; the memory of a wrapped range stays the caller's.
  static void Release(HostBuffer& buffer);

  // Images. Textures live in placement heaps (the texture pool's slabs), anything else in its own allocation; all in
  // private storage. TextureRequirements is what a heap needs for the image (types = 1: Metal has one memory type).
  Requirements TextureRequirements(const ImageDesc& desc);
  // A placement heap of `bytes` bytes for images; nullptr if Metal refuses. ReleaseHeap gives it back once its images
  // are gone.
  Memory CreateHeap(Size bytes);
  static void ReleaseHeap(Memory heap);
  // An image at `offset` in a heap, or one with its own memory; nullptr if the format has no Metal equivalent or
  // Metal refuses. Release gives either back.
  Image CreatePlacedImage(Memory heap, Size offset, const ImageDesc& desc);
  Image CreateImage(const ImageDesc& desc);
  static void Release(Image image);

  // Samplers (bo2zm_gpu_metal_samplers.mm), usable in argument buffers; SamplerId is the 64-bit
  // MTLResourceID the bindless sampler heap stores.
  Sampler CreateSampler(const SamplerDesc& desc);
  static void Release(Sampler sampler);
  static uint64_t SamplerId(Sampler sampler);

  // The function of a shader, from its library (loaded the first time one of its functions is asked for).
  // nullptr if there is no such function; the returned id<MTLFunction> is owned by the device.
  void* Function(std::string_view name);
  // The name of a shader's function: vs_/ps_ then the 16 hex digits of its hash, then _ez for the
  // early-fragment-tests variant of a pixel shader.
  static std::string FunctionName(bool vertex, uint64_t hash, bool early_z = false);

 private:
  struct State;
  explicit Device(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
  std::string name_;
  Caps caps_;
};

}  // namespace bo2zm::native::gpu::metal
