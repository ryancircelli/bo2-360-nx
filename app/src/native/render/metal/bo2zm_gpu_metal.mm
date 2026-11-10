// Native renderer, Metal backend: the device and the shader libraries (see bo2zm_gpu_metal.h).

#include "bo2zm_gpu_metal.h"

#include <rex/logging.h>

#import <Foundation/Foundation.h>
#include <vulkan/vulkan_core.h>

#import <Metal/Metal.h>

#include <array>
#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#if !__has_feature(objc_arc)
#error "bo2zm_gpu_metal.mm is built with -fobjc-arc"
#endif

namespace bo2zm::native::gpu::metal {

struct Device::State {
  id<MTLDevice> device = nil;
  // The one queue. Command buffers are made and encoded on any thread (one per viewport and worker on the
  // recording side, each committed in submission order); Metal queues are thread safe.
  id<MTLCommandQueue> queue = nil;
  std::filesystem::path folder;
  std::mutex lock;
  // One library per first hex digit of the hash; nil until loaded, and kept nil (with `missing` set) if its
  // file does not load, so a missing shard is reported once.
  std::array<id<MTLLibrary>, 16> libraries{};
  std::array<bool, 16> missing{};
  std::unordered_map<std::string, id<MTLFunction>> functions;
};

Device::Device(std::unique_ptr<State> state) : state_(std::move(state)) {
  name_ = [[state_->device name] UTF8String];
}

Device::~Device() = default;

void* Device::native() const { return (__bridge void*)state_->device; }
void* Device::queue() const { return (__bridge void*)state_->queue; }

std::unique_ptr<Device> Device::Create(const std::filesystem::path& library_folder) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      REXLOG_ERROR("[native] Metal: no device");
      return nullptr;
    }
    const bool family = [device supportsFamily:MTLGPUFamilyApple7] || [device supportsFamily:MTLGPUFamilyMetal3];
    if (!family || device.argumentBuffersSupport < MTLArgumentBuffersTier2) {
      REXLOG_ERROR("[native] Metal: {} lacks the Apple7 family or Metal 3, or tier 2 argument buffers",
                   [[device name] UTF8String]);
      return nullptr;
    }
    auto state = std::make_unique<State>();
    state->device = device;
    state->queue = [device newCommandQueueWithMaxCommandBufferCount:64];
    if (!state->queue) {
      REXLOG_ERROR("[native] Metal: could not make the command queue");
      return nullptr;
    }
    state->queue.label = @"bo2zm";
    state->folder = library_folder;
    std::unique_ptr<Device> result(new Device(std::move(state)));
    Caps& caps = result->caps_;
    std::snprintf(caps.device_name, sizeof(caps.device_name), "%s", [[device name] UTF8String]);
    // Tier 2 argument buffers (checked above) give 64-bit integers, GPU addresses and the bindless heaps:
    // unbounded, partially filled arrays indexed per draw and updated while earlier frames still use them.
    caps.int64 = true;
    caps.buffer_address = true;
    caps.runtime_arrays = true;
    caps.dynamic_indexing = true;
    caps.partially_bound = true;
    caps.update_after_bind = true;
    caps.update_unused_pending = true;
    caps.mirror_clamp_to_edge = true;
    caps.anisotropy = true;
    caps.max_anisotropy = 16.0f;
    // setVertexBuffer:offset: wants 4-byte offsets on Apple GPUs; Mac-family (Intel, AMD) GPUs want 256.
    caps.uniform_alignment = [device supportsFamily:MTLGPUFamilyApple7] ? 4 : 256;
    // MTLVisibilityResultModeCounting counts samples, like a precise occlusion query.
    caps.occlusion_precise = true;
    caps.pipeline_statistics = false;
    // Timestamps come from counter sample buffers, decided with the command recording (phase 5); none until then.
    caps.timestamp_period_ns = 0.0f;
    // The pipeline cache is an MTLBinaryArchive serialized to disk, and pipelines can be asked to come only from it
    // (MTLPipelineOptionFailOnBinaryArchiveMiss).
    caps.pipeline_cache = true;
    caps.pipeline_cache_control = true;
    caps.api_version = 0;
    // Viewport, scissor, cull, winding, depth bias and depth/stencil state are encoder state, set per draw;
    // topology class and blending live in the pipeline, so there is no third set.
    caps.dynamic_state12 = true;
    caps.dynamic_state3 = false;
    // No 24-bit depth on Apple GPUs; the renderer's D32S8 fallback. Blits cannot scale, so depth is never scaled.
    caps.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    caps.depth_sampled = true;
    caps.depth_copy_source = true;
    caps.depth_copy_target = true;
    caps.depth_scalable = false;
    REXLOG_INFO("[native] Metal: {}, shader libraries in {}", result->name_, library_folder.string());
    return result;
  }
}

std::string Device::FunctionName(bool vertex, uint64_t hash, bool early_z) {
  char name[32];
  std::snprintf(name, sizeof(name), "%s_%016" PRIx64 "%s", vertex ? "vs" : "ps", hash, early_z ? "_ez" : "");
  return name;
}

void* Device::Function(std::string_view name) { return Specialized(name, false, 0); }

void* Device::SpecializedFunction(std::string_view name, uint32_t constant) { return Specialized(name, true, constant); }

void* Device::Specialized(std::string_view name, bool specialized, uint32_t constant) {
  // vs_ or ps_, then the hash: the shard is the hash's first hex digit.
  if (name.size() < 4) {
    return nullptr;
  }
  const char digit = name[3];
  const int shard = digit >= '0' && digit <= '9' ? digit - '0' : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : -1;
  if (shard < 0) {
    return nullptr;
  }
  State& s = *state_;
  std::lock_guard<std::mutex> guard(s.lock);
  std::string key(name);
  if (specialized) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "#%08x", constant);
    key += suffix;
  }
  if (auto found = s.functions.find(key); found != s.functions.end()) {
    return (__bridge void*)found->second;
  }
  @autoreleasepool {
    if (!s.libraries[shard] && !s.missing[shard]) {
      char file[32];
      std::snprintf(file, sizeof(file), "bo2zm_%x.metallib", shard);
      const std::filesystem::path path = s.folder / file;
      NSError* error = nil;
      NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
      s.libraries[shard] = [s.device newLibraryWithURL:url error:&error];
      if (!s.libraries[shard]) {
        s.missing[shard] = true;
        REXLOG_ERROR("[native] Metal: could not load {}: {}", path.string(),
                     error ? [[error localizedDescription] UTF8String] : "unknown error");
      }
    }
    if (!s.libraries[shard]) {
      return nullptr;
    }
    NSString* function_name = [[NSString alloc] initWithBytes:name.data()
                                                       length:name.size()
                                                     encoding:NSUTF8StringEncoding];
    id<MTLFunction> function = nil;
    if (specialized) {
      // The pack shaders' one specialization constant (id 0, g_SpecConstants) as function constant 0.
      MTLFunctionConstantValues* values = [[MTLFunctionConstantValues alloc] init];
      [values setConstantValue:&constant type:MTLDataTypeUInt atIndex:0];
      NSError* error = nil;
      function = [s.libraries[shard] newFunctionWithName:function_name constantValues:values error:&error];
      if (!function) {
        REXLOG_WARN("[native] Metal: {} not specialized with {:#x}: {}", std::string(name), constant,
                    error ? [[error localizedDescription] UTF8String] : "not in the library");
      }
    } else {
      function = [s.libraries[shard] newFunctionWithName:function_name];
    }
    if (!function) {
      return nullptr;
    }
    s.functions.emplace(key, function);
    return (__bridge void*)function;
  }
}

}  // namespace bo2zm::native::gpu::metal
