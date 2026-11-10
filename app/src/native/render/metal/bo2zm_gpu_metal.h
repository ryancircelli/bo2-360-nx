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

class Device {
 public:
  // nullptr if there is no Metal device that can run the renderer (the reason is logged): it needs tier 2
  // argument buffers (the bindless heaps) and the Apple7 family or Metal 3.
  static std::unique_ptr<Device> Create(const std::filesystem::path& library_folder);
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const std::string& name() const { return name_; }
  // The id<MTLDevice>, for the Objective-C++ parts of the backend.
  void* native() const;
  // The id<MTLCommandQueue> every command buffer goes to (rendering and presentation).
  void* queue() const;

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
};

}  // namespace bo2zm::native::gpu::metal
