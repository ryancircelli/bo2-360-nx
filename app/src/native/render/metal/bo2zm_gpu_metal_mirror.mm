// Native renderer, Metal backend: the mirror (see bo2zm_gpu_metal_mirror.h).
//
// The enum values the layer passes are in Vulkan's numbering (gpu::Format, CompareOp, StencilOp, ...). Where Metal
// numbers the same thing the same way (compare functions, stencil operations, blend operations, cull modes,
// primitive types 0 to 4) the value is passed through; the rest goes through the small tables below. The
// translation follows what MoltenVK does for the same calls, since that is what draws the Vulkan image on a Mac.

#include "bo2zm_gpu_metal_mirror.h"

#include "bo2zm_gpu_metal.h"

#include <rex/logging.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#if !__has_feature(objc_arc)
#error "bo2zm_gpu_metal_mirror.mm is built with -fobjc-arc"
#endif

namespace bo2zm::native::gpu::metal {

Mirror* Mirror::instance_ = nullptr;

namespace {

// Bytes of one texel, or of one 4x4 block for the compressed formats (block 4).
bool FormatBlock(Format format, uint32_t& bytes, uint32_t& block) {
  block = 1;
  switch (VkFormat(format)) {
    case VK_FORMAT_R8_UNORM:
      bytes = 1;
      return true;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_D16_UNORM:
      bytes = 2;
      return true;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
      bytes = 4;
      return true;
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT:
      bytes = 8;
      return true;
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      bytes = 16;
      return true;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
      bytes = 8;
      block = 4;
      return true;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
      bytes = 16;
      block = 4;
      return true;
    default:
      return false;
  }
}

bool HasStencil(MTLPixelFormat format) {
  return format == MTLPixelFormatDepth32Float_Stencil8 || format == MTLPixelFormatDepth24Unorm_Stencil8 ||
         format == MTLPixelFormatStencil8;
}
bool IsDepth(MTLPixelFormat format) {
  return format == MTLPixelFormatDepth16Unorm || format == MTLPixelFormatDepth32Float || HasStencil(format);
}

MTLBlendFactor BlendFactor(uint32_t v) {
  static constexpr MTLBlendFactor kTable[] = {
      MTLBlendFactorZero,
      MTLBlendFactorOne,
      MTLBlendFactorSourceColor,
      MTLBlendFactorOneMinusSourceColor,
      MTLBlendFactorDestinationColor,
      MTLBlendFactorOneMinusDestinationColor,
      MTLBlendFactorSourceAlpha,
      MTLBlendFactorOneMinusSourceAlpha,
      MTLBlendFactorDestinationAlpha,
      MTLBlendFactorOneMinusDestinationAlpha,
      MTLBlendFactorBlendColor,
      MTLBlendFactorOneMinusBlendColor,
      MTLBlendFactorBlendAlpha,
      MTLBlendFactorOneMinusBlendAlpha,
      MTLBlendFactorSourceAlphaSaturated,
  };
  return v < std::size(kTable) ? kTable[v] : MTLBlendFactorOne;
}

// Vulkan's R G B A bits (1 2 4 8) are Metal's A B G R (1 2 4 8).
MTLColorWriteMask WriteMask(uint32_t v) {
  MTLColorWriteMask m = MTLColorWriteMaskNone;
  if (v & 1) m |= MTLColorWriteMaskRed;
  if (v & 2) m |= MTLColorWriteMaskGreen;
  if (v & 4) m |= MTLColorWriteMaskBlue;
  if (v & 8) m |= MTLColorWriteMaskAlpha;
  return m;
}

// Vulkan's swizzle (0 identity, 1 zero, 2 one, 3 to 6 red to alpha) for channel `i`.
MTLTextureSwizzle Swizzle(uint32_t v, uint32_t i) {
  if (v == 0) return MTLTextureSwizzle(MTLTextureSwizzleRed + i);
  if (v == 1) return MTLTextureSwizzleZero;
  if (v == 2) return MTLTextureSwizzleOne;
  return MTLTextureSwizzle(MTLTextureSwizzleRed + (v - 3));
}

MTLLoadAction Load(LoadOp op) {
  return op == kLoadLoad ? MTLLoadActionLoad : op == kLoadClear ? MTLLoadActionClear : MTLLoadActionDontCare;
}

struct BufferM {
  id<MTLBuffer> buffer = nil;
  Size offset = 0;  // where the Vulkan buffer's byte 0 is in `buffer` (the wrap starts at a page)
  Size size = 0;
};

struct ImageM {
  id<MTLTexture> texture = nil;
  ImageDesc desc;
};

struct ViewM {
  id<MTLTexture> texture = nil;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct SetLayoutM {
  std::vector<SetBinding> bindings;
};

struct SetM {
  // A bindless heap (sets 0 to 3): one argument buffer, an 8-byte resource id per element.
  id<MTLBuffer> heap = nil;
  uint32_t capacity = 0;
  // Uniform buffers (set 4): per binding, and whether its offset is given when the set is bound.
  struct Uniform {
    Buffer buffer = nullptr;
    Size offset = 0;
    bool dynamic = false;
  };
  std::array<Uniform, 4> uniforms{};
  uint32_t n_uniforms = 0;
};

struct ShaderTag {
  bool vertex = false;
  uint64_t hash = 0;
  bool early_z = false;
};

struct PipelineM {
  bool vs_tagged = false;
  bool ps_tagged = false;
  bool has_ps = false;
  ShaderTag vs;
  ShaderTag ps;
  uint32_t constant = 0;
  std::vector<uint32_t> strides;
  std::vector<VertexAttribute> attributes;
  PipelineState state{};
  // Made at the first draw, for the formats of the pass it draws in (the key); `failed` keeps a failure from
  // being retried every draw.
  std::string key;
  id<MTLRenderPipelineState> made = nil;
  bool failed = false;
};

struct Bound {
  Pipeline pipeline = nullptr;
  std::array<Set, 8> sets{};
  std::array<uint32_t, 8> dynamic_offsets{};  // set 4: the offsets given with the set, in binding order
  uint32_t n_vertex = 0;
  std::array<Buffer, 16> vertex{};
  std::array<Size, 16> vertex_offsets{};
  Buffer index = nullptr;
  bool indices_16 = true;
  Viewport viewport{};
  Rect scissor{};
  float blend[4] = {};
  float bias_constant = 0.0f;
  float bias_slope = 0.0f;
  uint32_t stencil_front = 0;
  uint32_t stencil_back = 0;
};

struct ListM {
  id<MTLCommandBuffer> commands = nil;
  id<MTLRenderCommandEncoder> render = nil;
  id<MTLBlitCommandEncoder> blit = nil;
  // The pass being recorded: its description and attachments (for restarting the encoder on a clear).
  Pass pass = nullptr;
  Framebuffer framebuffer = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  Bound bound;
};

struct PassM {
  PassDesc desc;
};

struct FramebufferM {
  Pass pass = nullptr;
  std::vector<ImageView> views;
  uint32_t width = 0;
  uint32_t height = 0;
};

}  // namespace

struct Mirror::State {
  Device* device = nullptr;
  id<MTLDevice> mtl = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLResidencySet> residency = nil;
  bool residency_dirty = false;
  std::recursive_mutex lock;
  std::unordered_map<Buffer, BufferM> buffers;
  std::unordered_map<Image, ImageM> images;
  std::unordered_map<ImageView, ViewM> views;
  std::unordered_map<Sampler, id<MTLSamplerState>> samplers;
  std::unordered_map<Shader, ShaderTag> shaders;
  std::unordered_map<SetLayout, SetLayoutM> set_layouts;
  std::unordered_map<Set, SetM> sets;
  std::unordered_map<Pass, PassM> passes;
  std::unordered_map<Framebuffer, FramebufferM> framebuffers;
  std::unordered_map<Pipeline, PipelineM> pipelines;
  std::unordered_map<CommandList, ListM> lists;
  std::unordered_map<std::string, id<MTLDepthStencilState>> depth_states;
  std::set<std::string> logged;
  id<MTLTexture> shown = nil;
  uint32_t shown_width = 0;
  uint32_t shown_height = 0;
  uint64_t draws = 0;
  uint64_t draws_skipped = 0;
  uint64_t fans = 0;  // triangle fans, drawn as lists
  uint64_t clears_part = 0;  // clears of part of an attachment, drawn as rectangles
  uint64_t frames = 0;

  void Once(const std::string& what) {
    if (logged.insert(what).second) {
      REXLOG_WARN("[native] Metal mirror: {}", what);
    }
  }

  void Resident(id<MTLAllocation> allocation, bool add) {
    if (!residency || !allocation) return;
    if (add) {
      [residency addAllocation:allocation];
    } else {
      [residency removeAllocation:allocation];
    }
    residency_dirty = true;
  }

  ListM& List(CommandList list) { return lists[list]; }

  id<MTLCommandBuffer> Commands(ListM& l) {
    if (!l.commands) {
      l.commands = [queue commandBuffer];
      l.commands.label = @"bo2zm mirror";
      if (residency) {
        [l.commands useResidencySet:residency];
      }
    }
    return l.commands;
  }

  void EndEncoders(ListM& l) {
    if (l.render) {
      [l.render endEncoding];
      l.render = nil;
    }
    if (l.blit) {
      [l.blit endEncoding];
      l.blit = nil;
    }
  }

  id<MTLBlitCommandEncoder> Blit(ListM& l) {
    if (l.render) {
      Once("copy recorded inside a pass (the pass is ended for it)");
      [l.render endEncoding];
      l.render = nil;
    }
    if (!l.blit) {
      l.blit = [Commands(l) blitCommandEncoder];
    }
    return l.blit;
  }

  // Starts (or restarts) the render encoder of the current pass. `restart`: every attachment loads what the
  // previous encoder stored, except those cleared now.
  bool StartPass(ListM& l, bool restart, uint32_t n_clears, const ClearValue* clears, int clear_color,
                 bool clear_depth, bool clear_stencil, const ClearValue* clear_value) {
    auto p = passes.find(l.pass);
    auto f = framebuffers.find(l.framebuffer);
    if (p == passes.end() || f == framebuffers.end()) {
      Once("pass or framebuffer the mirror did not see created");
      return false;
    }
    const PassDesc& desc = p->second.desc;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    uint32_t attachment = 0;
    for (uint32_t i = 0; i < desc.color_count && attachment < f->second.views.size(); ++i, ++attachment) {
      auto v = views.find(f->second.views[attachment]);
      if (v == views.end() || !v->second.texture) {
        Once("pass attachment without a Metal view");
        return false;
      }
      MTLRenderPassColorAttachmentDescriptor* c = rp.colorAttachments[i];
      c.texture = v->second.texture;
      c.storeAction = MTLStoreActionStore;
      if (restart) {
        c.loadAction = int(i) == clear_color ? MTLLoadActionClear : MTLLoadActionLoad;
        if (int(i) == clear_color) {
          const ClearColor& cc = clear_value->color;
          c.clearColor = MTLClearColorMake(cc.f[0], cc.f[1], cc.f[2], cc.f[3]);
        }
      } else {
        c.loadAction = Load(desc.colors[i].load);
        if (attachment < n_clears && clears) {
          const ClearColor& cc = clears[attachment].color;
          c.clearColor = MTLClearColorMake(cc.f[0], cc.f[1], cc.f[2], cc.f[3]);
        }
      }
    }
    if (desc.has_depth && attachment < f->second.views.size()) {
      auto v = views.find(f->second.views[attachment]);
      if (v == views.end() || !v->second.texture) {
        Once("depth attachment without a Metal view");
        return false;
      }
      id<MTLTexture> t = v->second.texture;
      MTLRenderPassDepthAttachmentDescriptor* d = rp.depthAttachment;
      d.texture = t;
      d.storeAction = MTLStoreActionStore;
      const ClearValue* value = restart ? clear_value : (attachment < n_clears && clears ? &clears[attachment] : nullptr);
      if (restart) {
        d.loadAction = clear_depth ? MTLLoadActionClear : MTLLoadActionLoad;
      } else {
        d.loadAction = Load(desc.depth.load);
      }
      if (value) {
        d.clearDepth = value->depth_stencil.depth;
      }
      if (HasStencil(t.pixelFormat)) {
        MTLRenderPassStencilAttachmentDescriptor* s = rp.stencilAttachment;
        s.texture = t;
        s.storeAction = MTLStoreActionStore;
        if (restart) {
          s.loadAction = clear_stencil ? MTLLoadActionClear : MTLLoadActionLoad;
        } else {
          s.loadAction = Load(desc.depth.stencil_load);
        }
        if (value) {
          s.clearStencil = value->depth_stencil.stencil;
        }
      }
    }
    rp.renderTargetWidth = f->second.width;
    rp.renderTargetHeight = f->second.height;
    if (l.blit) {
      [l.blit endEncoding];
      l.blit = nil;
    }
    if (l.render) {
      [l.render endEncoding];
      l.render = nil;
    }
    l.render = [Commands(l) renderCommandEncoderWithDescriptor:rp];
    l.width = f->second.width;
    l.height = f->second.height;
    return l.render != nil;
  }

  // The pipeline for the pass being recorded; nil (logged once) if it cannot be made.
  id<MTLRenderPipelineState> Pipeline(ListM& l, PipelineM& p) {
    auto pass = passes.find(l.pass);
    if (pass == passes.end()) return nil;
    const PassDesc& desc = pass->second.desc;
    std::string key;
    for (uint32_t i = 0; i < desc.color_count; ++i) key += std::to_string(desc.colors[i].format) + ",";
    key += desc.has_depth ? std::to_string(desc.depth.format) : "-";
    if (p.made && p.key == key) return p.made;
    if (p.failed && p.key == key) return nil;
    p.key = key;
    p.made = nil;
    p.failed = true;
    if (!p.vs_tagged || (p.has_ps && !p.ps_tagged)) {
      Once("pipeline with one of the renderer's own shaders (not in the pack): its draws are skipped");
      return nil;
    }
    id<MTLFunction> vs = (__bridge id<MTLFunction>)device->SpecializedFunction(
        Device::FunctionName(true, p.vs.hash), p.constant);
    id<MTLFunction> ps = nil;
    if (p.has_ps) {
      ps = (__bridge id<MTLFunction>)device->SpecializedFunction(
          Device::FunctionName(false, p.ps.hash, p.ps.early_z), p.constant);
    }
    if (!vs || (p.has_ps && !ps)) {
      Once("a pack shader is missing from the Metal libraries (see the specialization warning)");
      return nil;
    }
    MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = vs;
    d.fragmentFunction = ps;
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (const VertexAttribute& a : p.attributes) {
      const MTLVertexFormat format = MTLVertexFormat(VertexFormat(a.format));
      if (format == MTLVertexFormatInvalid) {
        Once("vertex format without a Metal one: " + std::to_string(a.format));
        return nil;
      }
      vd.attributes[a.location].format = format;
      vd.attributes[a.location].offset = a.offset;
      vd.attributes[a.location].bufferIndex = 8 + a.binding;
    }
    for (size_t b = 0; b < p.strides.size(); ++b) {
      MTLVertexBufferLayoutDescriptor* layout = vd.layouts[8 + b];
      if (p.strides[b] == 0) {
        layout.stepFunction = MTLVertexStepFunctionConstant;
        layout.stepRate = 0;
        layout.stride = 4;
      } else {
        layout.stepFunction = MTLVertexStepFunctionPerVertex;
        layout.stride = p.strides[b];
      }
    }
    d.vertexDescriptor = vd;
    for (uint32_t i = 0; i < desc.color_count; ++i) {
      MTLRenderPipelineColorAttachmentDescriptor* c = d.colorAttachments[i];
      c.pixelFormat = MTLPixelFormat(PixelFormat(desc.colors[i].format));
      if (i < p.state.n_colors) {
        const BlendTarget& b = p.state.blends[i];
        c.blendingEnabled = b.enable != 0;
        c.sourceRGBBlendFactor = BlendFactor(b.equation.src_color);
        c.destinationRGBBlendFactor = BlendFactor(b.equation.dst_color);
        c.rgbBlendOperation = MTLBlendOperation(b.equation.color_op);
        c.sourceAlphaBlendFactor = BlendFactor(b.equation.src_alpha);
        c.destinationAlphaBlendFactor = BlendFactor(b.equation.dst_alpha);
        c.alphaBlendOperation = MTLBlendOperation(b.equation.alpha_op);
        c.writeMask = WriteMask(b.mask);
      } else {
        c.writeMask = MTLColorWriteMaskNone;
      }
    }
    if (desc.has_depth) {
      const MTLPixelFormat depth = MTLPixelFormat(PixelFormat(desc.depth.format));
      d.depthAttachmentPixelFormat = depth;
      if (HasStencil(depth)) d.stencilAttachmentPixelFormat = depth;
    }
    NSError* error = nil;
    p.made = [mtl newRenderPipelineStateWithDescriptor:d error:&error];
    if (!p.made) {
      Once(std::string("pipeline not made: ") + (error ? [[error localizedDescription] UTF8String] : "?"));
      return nil;
    }
    p.failed = false;
    return p.made;
  }

  id<MTLDepthStencilState> DepthState(const PipelineState& s, const Bound& b, bool has_depth, bool has_stencil) {
    char key[160];
    const uint32_t masks = has_stencil && s.stencil_test ? ((b.stencil_front >> 8) & 0xFFFF) |
                                                               (((b.stencil_back >> 8) & 0xFFFF) << 16)
                                                         : 0;
    std::snprintf(key, sizeof(key), "%u%u%u%u|%u%u%u%u|%u%u%u%u|%08x", has_depth && s.depth_test,
                  has_depth && s.depth_write, s.depth_compare, has_stencil && s.stencil_test, s.front.fail,
                  s.front.pass, s.front.depth_fail, s.front.compare, s.back.fail, s.back.pass, s.back.depth_fail,
                  s.back.compare, masks);
    auto found = depth_states.find(key);
    if (found != depth_states.end()) return found->second;
    MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
    if (has_depth && s.depth_test) {
      d.depthCompareFunction = MTLCompareFunction(s.depth_compare);
    } else {
      d.depthCompareFunction = MTLCompareFunctionAlways;
    }
    d.depthWriteEnabled = has_depth && s.depth_test && s.depth_write;
    if (has_stencil && s.stencil_test) {
      auto face = [](const StencilOps& o, uint32_t packed) {
        MTLStencilDescriptor* f = [[MTLStencilDescriptor alloc] init];
        f.stencilFailureOperation = MTLStencilOperation(o.fail);
        f.depthStencilPassOperation = MTLStencilOperation(o.pass);
        f.depthFailureOperation = MTLStencilOperation(o.depth_fail);
        f.stencilCompareFunction = MTLCompareFunction(o.compare);
        f.readMask = (packed >> 8) & 0xFF;
        f.writeMask = (packed >> 16) & 0xFF;
        return f;
      };
      d.frontFaceStencil = face(s.front, b.stencil_front);
      d.backFaceStencil = face(s.back, b.stencil_back);
    }
    id<MTLDepthStencilState> state = [mtl newDepthStencilStateWithDescriptor:d];
    depth_states.emplace(key, state);
    return state;
  }

  // Everything a draw needs, set on the encoder before it. False (counted) if the draw cannot be made.
  bool Prepare(ListM& l, MTLPrimitiveType& primitive) {
    if (!l.render) {
      Once("draw outside a pass");
      return false;
    }
    auto pi = pipelines.find(l.bound.pipeline);
    if (pi == pipelines.end()) {
      Once("draw with a pipeline the mirror did not see created");
      return false;
    }
    PipelineM& p = pi->second;
    if (p.state.topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) {
      Once("primitive topology " + std::to_string(p.state.topology) + " is not drawn");
      return false;
    }
    id<MTLRenderPipelineState> made = Pipeline(l, p);
    if (!made) return false;
    // Metal has no fans: Draw and DrawIndexed turn them into triangle lists (FanIndices).
    primitive = p.state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ? MTLPrimitiveTypeTriangle
                                                                       : MTLPrimitiveType(p.state.topology);
    id<MTLRenderCommandEncoder> e = l.render;
    const Bound& b = l.bound;
    [e setRenderPipelineState:made];
    const PassDesc& desc = passes[l.pass].desc;
    bool has_stencil = false;
    if (desc.has_depth) has_stencil = HasStencil(MTLPixelFormat(PixelFormat(desc.depth.format)));
    [e setDepthStencilState:DepthState(p.state, b, desc.has_depth, has_stencil)];
    if (has_stencil) {
      [e setStencilFrontReferenceValue:b.stencil_front & 0xFF backReferenceValue:b.stencil_back & 0xFF];
    }
    [e setCullMode:MTLCullMode(p.state.cull)];
    [e setFrontFacingWinding:p.state.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE ? MTLWindingCounterClockwise
                                                                                   : MTLWindingClockwise];
    if (p.state.depth_bias) {
      [e setDepthBias:b.bias_constant slopeScale:b.bias_slope clamp:0.0f];
    } else {
      [e setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
    }
    [e setDepthClipMode:p.state.depth_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
    const Viewport& v = b.viewport;
    [e setViewport:MTLViewport{v.x, v.y, v.width, v.height, v.min_depth, v.max_depth}];
    // Metal requires the scissor inside the attachments.
    const int64_t x0 = std::clamp<int64_t>(b.scissor.x, 0, l.width);
    const int64_t y0 = std::clamp<int64_t>(b.scissor.y, 0, l.height);
    const int64_t x1 = std::clamp<int64_t>(int64_t(b.scissor.x) + b.scissor.width, 0, l.width);
    const int64_t y1 = std::clamp<int64_t>(int64_t(b.scissor.y) + b.scissor.height, 0, l.height);
    if (x1 <= x0 || y1 <= y0) return false;
    [e setScissorRect:MTLScissorRect{NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0)}];
    [e setBlendColorRed:b.blend[0] green:b.blend[1] blue:b.blend[2] alpha:b.blend[3]];
    // Sets 0 to 3: the heaps' argument buffers at buffers 0 to 3; set 4: the uniform buffers at 4 to 6.
    for (uint32_t i = 0; i < 8; ++i) {
      auto s = sets.find(b.sets[i]);
      if (s == sets.end()) continue;
      if (s->second.heap && i < 4) {
        [e setVertexBuffer:s->second.heap offset:0 atIndex:i];
        [e setFragmentBuffer:s->second.heap offset:0 atIndex:i];
      } else if (i == 4) {
        uint32_t dynamic = 0;
        for (uint32_t u = 0; u < s->second.n_uniforms && u < 3; ++u) {
          const SetM::Uniform& uniform = s->second.uniforms[u];
          auto buffer = buffers.find(uniform.buffer);
          if (buffer == buffers.end()) continue;
          Size offset = buffer->second.offset + uniform.offset;
          if (uniform.dynamic) offset += b.dynamic_offsets[dynamic++];
          [e setVertexBuffer:buffer->second.buffer offset:offset atIndex:4 + u];
          [e setFragmentBuffer:buffer->second.buffer offset:offset atIndex:4 + u];
        }
      }
    }
    for (uint32_t i = 0; i < b.n_vertex; ++i) {
      auto buffer = buffers.find(b.vertex[i]);
      if (buffer == buffers.end()) {
        Once("vertex buffer the mirror did not see created");
        return false;
      }
      [e setVertexBuffer:buffer->second.buffer offset:buffer->second.offset + b.vertex_offsets[i] atIndex:8 + i];
    }
    return true;
  }

  bool Fan(const ListM& l) {
    auto pi = pipelines.find(l.bound.pipeline);
    return pi != pipelines.end() && pi->second.state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  }

  // A triangle fan as a list of 32-bit indices, in a buffer of its own: triangle i is (i + 1, i + 2, 0) of its fan,
  // Vulkan's order (the same winding as (0, i + 1, i + 2), and the provoking vertex Vulkan has). `source` null: a
  // draw without indices, vertices 0 to count - 1 (the draw's first vertex goes in as the base vertex). With
  // `restart`, that index ends the fan and the next index starts a new one. Nil if there is no triangle.
  id<MTLBuffer> FanIndices(const void* source, bool indices_16, uint32_t count, uint32_t restart,
                           uint32_t& triangles_out) {
    std::vector<uint32_t> list;
    list.reserve(count > 2 ? size_t(count - 2) * 3 : 0);
    auto at = [&](uint32_t i) -> uint32_t {
      if (!source) return i;
      return indices_16 ? static_cast<const uint16_t*>(source)[i] : static_cast<const uint32_t*>(source)[i];
    };
    uint32_t start = 0;
    while (start < count) {
      uint32_t end = start;
      while (end < count && !(source && at(end) == restart)) ++end;
      const uint32_t hub = at(start);
      for (uint32_t i = start + 1; i + 1 < end; ++i) {
        list.push_back(at(i));
        list.push_back(at(i + 1));
        list.push_back(hub);
      }
      start = end + 1;
    }
    triangles_out = uint32_t(list.size() / 3);
    if (list.empty()) return nil;
    return [mtl newBufferWithBytes:list.data() length:list.size() * sizeof(uint32_t)
                           options:MTLResourceStorageModeShared];
  }

  id<MTLTexture> Texture(Image image) {
    auto found = images.find(image);
    return found == images.end() ? nil : found->second.texture;
  }

  // Clears of part of an attachment (vkCmdClearAttachments with a rectangle smaller than the pass, e.g. one
  // player's view in split screen): a rectangle drawn into the open pass with a pipeline that writes the value.
  // One small library per color attachment index (the output's [[color(n)]] is fixed in the source).
  std::unordered_map<std::string, id<MTLRenderPipelineState>> clear_pipelines;
  std::array<id<MTLLibrary>, 8> clear_libraries{};
  id<MTLDepthStencilState> clear_depth_states[4] = {};  // bit 0 depth written, bit 1 stencil written

  id<MTLLibrary> ClearLibrary(uint32_t color_index) {
    if (color_index >= clear_libraries.size()) return nil;
    if (clear_libraries[color_index]) return clear_libraries[color_index];
    char source[1024];
    std::snprintf(source, sizeof(source),
                  "#include <metal_stdlib>\n"
                  "using namespace metal;\n"
                  "struct ClearIn { float4 color; float depth; };\n"
                  "struct ClearV { float4 position [[position]]; };\n"
                  "vertex ClearV clear_vs(uint id [[vertex_id]], constant ClearIn& in [[buffer(0)]]) {\n"
                  "  float2 p = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? 1.0 : -1.0);\n"
                  "  ClearV v; v.position = float4(p, in.depth, 1.0); return v;\n"
                  "}\n"
                  "struct ClearOut { float4 color [[color(%u)]]; };\n"
                  "fragment ClearOut clear_fs(constant ClearIn& in [[buffer(0)]]) {\n"
                  "  ClearOut o; o.color = in.color; return o;\n"
                  "}\n"
                  "fragment void clear_fs_none() {}\n",
                  color_index);
    NSError* error = nil;
    id<MTLLibrary> library = [mtl newLibraryWithSource:[NSString stringWithUTF8String:source] options:nil
                                                 error:&error];
    if (!library) {
      Once(std::string("clear library not made: ") + (error ? [[error localizedDescription] UTF8String] : "?"));
      return nil;
    }
    clear_libraries[color_index] = library;
    return library;
  }

  // Draws the clear of `area` into the pass being recorded. The encoder's pipeline, depth state, viewport and
  // scissor are changed; Prepare sets all of them again before the next draw.
  void ClearArea(ListM& l, int color_index, bool depth, bool stencil, const ClearValue& value, const Rect& area) {
    auto p = passes.find(l.pass);
    auto f = framebuffers.find(l.framebuffer);
    if (p == passes.end() || f == framebuffers.end()) return;
    const PassDesc& desc = p->second.desc;
    if (color_index >= int(desc.color_count)) return;
    if (!desc.has_depth) depth = stencil = false;
    MTLPixelFormat depth_format = MTLPixelFormatInvalid;
    if (desc.has_depth) depth_format = MTLPixelFormat(PixelFormat(desc.depth.format));
    if (!HasStencil(depth_format)) stencil = false;
    if (color_index < 0 && !depth && !stencil) return;
    // The attachments' sample count (all attachments of a pass share it).
    NSUInteger samples = 1;
    if (!f->second.views.empty()) {
      auto v = views.find(f->second.views[0]);
      if (v != views.end() && v->second.texture) samples = v->second.texture.sampleCount;
    }
    std::string key = std::to_string(color_index) + (depth ? "d" : "") + (stencil ? "s" : "") + "/" +
                      std::to_string(samples) + "/";
    for (uint32_t i = 0; i < desc.color_count; ++i) key += std::to_string(desc.colors[i].format) + ",";
    key += std::to_string(int(depth_format));
    id<MTLRenderPipelineState> pipeline = nil;
    auto found = clear_pipelines.find(key);
    if (found != clear_pipelines.end()) {
      pipeline = found->second;
    } else {
      id<MTLLibrary> library = ClearLibrary(color_index < 0 ? 0 : uint32_t(color_index));
      if (!library) return;
      MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
      d.vertexFunction = [library newFunctionWithName:@"clear_vs"];
      d.fragmentFunction = [library newFunctionWithName:color_index < 0 ? @"clear_fs_none" : @"clear_fs"];
      d.rasterSampleCount = samples;
      for (uint32_t i = 0; i < desc.color_count; ++i) {
        d.colorAttachments[i].pixelFormat = MTLPixelFormat(PixelFormat(desc.colors[i].format));
        d.colorAttachments[i].writeMask = int(i) == color_index ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
      }
      d.depthAttachmentPixelFormat = depth_format;
      if (HasStencil(depth_format)) d.stencilAttachmentPixelFormat = depth_format;
      NSError* error = nil;
      pipeline = [mtl newRenderPipelineStateWithDescriptor:d error:&error];
      if (!pipeline) {
        Once(std::string("clear pipeline not made: ") +
             (error ? [[error localizedDescription] UTF8String] : "?"));
      }
      clear_pipelines.emplace(key, pipeline);  // nil too: not tried again
    }
    if (!pipeline) return;
    const int state = (depth ? 1 : 0) | (stencil ? 2 : 0);
    if (!clear_depth_states[state]) {
      MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
      d.depthCompareFunction = MTLCompareFunctionAlways;
      d.depthWriteEnabled = depth;
      if (stencil) {
        MTLStencilDescriptor* s = [[MTLStencilDescriptor alloc] init];
        s.stencilCompareFunction = MTLCompareFunctionAlways;
        s.depthStencilPassOperation = MTLStencilOperationReplace;
        s.readMask = 0xFF;
        s.writeMask = 0xFF;
        d.frontFaceStencil = s;
        d.backFaceStencil = s;
      }
      clear_depth_states[state] = [mtl newDepthStencilStateWithDescriptor:d];
    }
    const int64_t x0 = std::clamp<int64_t>(area.x, 0, l.width);
    const int64_t y0 = std::clamp<int64_t>(area.y, 0, l.height);
    const int64_t x1 = std::clamp<int64_t>(int64_t(area.x) + area.width, 0, l.width);
    const int64_t y1 = std::clamp<int64_t>(int64_t(area.y) + area.height, 0, l.height);
    if (x1 <= x0 || y1 <= y0) return;
    struct {
      float color[4];
      float depth;
      float pad[3];
    } in = {{value.color.f[0], value.color.f[1], value.color.f[2], value.color.f[3]}, 0.0f, {}};
    if (color_index < 0) {
      in.depth = value.depth_stencil.depth;
    }
    id<MTLRenderCommandEncoder> e = l.render;
    [e setRenderPipelineState:pipeline];
    [e setDepthStencilState:clear_depth_states[state]];
    if (stencil) [e setStencilReferenceValue:value.depth_stencil.stencil & 0xFF];
    [e setCullMode:MTLCullModeNone];
    [e setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
    [e setDepthClipMode:MTLDepthClipModeClip];
    [e setViewport:MTLViewport{double(x0), double(y0), double(x1 - x0), double(y1 - y0), 0.0, 1.0}];
    [e setScissorRect:MTLScissorRect{NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0)}];
    [e setVertexBytes:&in length:sizeof(in) atIndex:0];
    [e setFragmentBytes:&in length:sizeof(in) atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  }

  // A render pass that clears one level and layer of `texture`.
  void ClearSlice(ListM& l, id<MTLTexture> texture, uint32_t level, uint32_t layer, const ClearColor* color,
                  const ClearDepthStencil* depth) {
    EndEncoders(l);
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (color) {
      rp.colorAttachments[0].texture = texture;
      rp.colorAttachments[0].level = level;
      rp.colorAttachments[0].slice = layer;
      rp.colorAttachments[0].loadAction = MTLLoadActionClear;
      rp.colorAttachments[0].storeAction = MTLStoreActionStore;
      rp.colorAttachments[0].clearColor = MTLClearColorMake(color->f[0], color->f[1], color->f[2], color->f[3]);
    } else {
      rp.depthAttachment.texture = texture;
      rp.depthAttachment.level = level;
      rp.depthAttachment.slice = layer;
      rp.depthAttachment.loadAction = MTLLoadActionClear;
      rp.depthAttachment.storeAction = MTLStoreActionStore;
      rp.depthAttachment.clearDepth = depth->depth;
      if (HasStencil(texture.pixelFormat)) {
        rp.stencilAttachment.texture = texture;
        rp.stencilAttachment.level = level;
        rp.stencilAttachment.slice = layer;
        rp.stencilAttachment.loadAction = MTLLoadActionClear;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.clearStencil = depth->stencil;
      }
    }
    [[Commands(l) renderCommandEncoderWithDescriptor:rp] endEncoding];
  }
};

void Mirror::Enable(Device& device) {
  if (!instance_) {
    instance_ = new Mirror(device);
  }
}

void Mirror::Disable() {
  delete instance_;
  instance_ = nullptr;
}

Mirror::Mirror(Device& device) : state_(new State) {
  State& s = *state_;
  s.device = &device;
  s.mtl = (__bridge id<MTLDevice>)device.native();
  s.queue = [s.mtl newCommandQueue];
  s.queue.label = @"bo2zm mirror";
  MTLResidencySetDescriptor* r = [[MTLResidencySetDescriptor alloc] init];
  r.label = @"bo2zm mirror";
  NSError* error = nil;
  s.residency = [s.mtl newResidencySetWithDescriptor:r error:&error];
  if (s.residency) {
    [s.queue addResidencySet:s.residency];
  } else {
    REXLOG_WARN("[native] Metal mirror: no residency set; textures in the heaps may not be resident");
  }
  REXLOG_INFO("[native] Metal mirror on: the Vulkan renderer's work is repeated on Metal ({})", device.name());
}

Mirror::~Mirror() { delete state_; }

void Mirror::BufferCreated(Buffer buffer, void* data, Size size) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  if (!data || !size) return;
  // Wrapped where it is, without a copy: from the page it starts in to the page after its end.
  const uintptr_t page = uintptr_t(getpagesize());
  const uintptr_t begin = reinterpret_cast<uintptr_t>(data) & ~(page - 1);
  const uintptr_t end = (reinterpret_cast<uintptr_t>(data) + size + page - 1) & ~(page - 1);
  id<MTLBuffer> wrapped = [s.mtl newBufferWithBytesNoCopy:reinterpret_cast<void*>(begin)
                                                   length:end - begin
                                                  options:MTLResourceStorageModeShared
                                              deallocator:nil];
  if (!wrapped) {
    s.Once("a host buffer could not be wrapped");
    return;
  }
  wrapped.label = @"bo2zm mirror host buffer";
  s.buffers[buffer] = BufferM{wrapped, Size(reinterpret_cast<uintptr_t>(data) - begin), size};
  s.Resident(wrapped, true);
}

void Mirror::BufferDestroyed(Buffer buffer) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  auto found = s.buffers.find(buffer);
  if (found == s.buffers.end()) return;
  s.Resident(found->second.buffer, false);
  s.buffers.erase(found);
}

void Mirror::ImageCreated(Image image, const ImageDesc& desc) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  @autoreleasepool {
    const MTLPixelFormat format = MTLPixelFormat(PixelFormat(desc.format));
    if (format == MTLPixelFormatInvalid) {
      s.Once("image format without a Metal one: " + std::to_string(desc.format));
      return;
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
    // Every image can be drawn in (clears are passes) except the compressed ones.
    uint32_t bytes = 0;
    uint32_t block = 1;
    FormatBlock(desc.format, bytes, block);
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView |
              (block == 1 ? MTLTextureUsageRenderTarget : MTLTextureUsage(0));
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texture = [s.mtl newTextureWithDescriptor:d];
    if (!texture) {
      s.Once("an image could not be made");
      return;
    }
    s.images[image] = ImageM{texture, desc};
    s.Resident(texture, true);
  }
}

void Mirror::ImageDestroyed(Image image) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  auto found = s.images.find(image);
  if (found == s.images.end()) return;
  s.Resident(found->second.texture, false);
  s.images.erase(found);
}

void Mirror::ViewCreated(ImageView view, Image image, const ViewDesc& desc) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  auto found = s.images.find(image);
  if (found == s.images.end()) {
    s.Once("view of an image the mirror does not have");
    return;
  }
  @autoreleasepool {
    id<MTLTexture> texture = found->second.texture;
    MTLPixelFormat format = MTLPixelFormat(PixelFormat(desc.format));
    if (IsDepth(texture.pixelFormat)) {
      format = texture.pixelFormat;  // the depth aspect of a depth/stencil image is read through its own format
    }
    if (format == MTLPixelFormatInvalid) {
      s.Once("view format without a Metal one: " + std::to_string(desc.format));
      return;
    }
    const uint32_t levels = desc.levels == kAllLevels ? uint32_t(texture.mipmapLevelCount) : desc.levels;
    const uint32_t texture_layers = texture.textureType == MTLTextureType3D ? 1u
                                    : (texture.textureType == MTLTextureTypeCube ||
                                       texture.textureType == MTLTextureTypeCubeArray)
                                        ? uint32_t(texture.arrayLength * 6)
                                        : uint32_t(texture.arrayLength);
    const uint32_t layers = desc.layers == kAllLevels ? texture_layers : desc.layers;
    MTLTextureType type = MTLTextureType2D;
    if (desc.type == kView3D) {
      type = MTLTextureType3D;
    } else if (desc.type == kViewCube) {
      type = layers > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
    } else if (layers > 1) {
      type = MTLTextureType2DArray;
    }
    const MTLTextureSwizzleChannels swizzle = MTLTextureSwizzleChannelsMake(
        Swizzle(desc.swizzle[0], 0), Swizzle(desc.swizzle[1], 1), Swizzle(desc.swizzle[2], 2),
        Swizzle(desc.swizzle[3], 3));
    id<MTLTexture> made = nil;
    @try {
      made = [texture newTextureViewWithPixelFormat:format
                                        textureType:type
                                             levels:NSMakeRange(0, std::max<uint32_t>(levels, 1))
                                             slices:NSMakeRange(0, type == MTLTextureType3D ? 1 : layers)
                                            swizzle:swizzle];
    } @catch (NSException* exception) {
      s.Once(std::string("view not made: ") + [[exception reason] UTF8String]);
    }
    if (!made) return;
    s.views[view] = ViewM{made, uint32_t(texture.width), uint32_t(texture.height)};
  }
}

void Mirror::ViewDestroyed(ImageView view) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.views.erase(view);
}

void Mirror::SamplerCreated(Sampler sampler, const SamplerDesc& desc) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  Sampler made = s.device->CreateSampler(desc);
  if (!made) {
    s.Once("a sampler could not be made");
    return;
  }
  s.samplers[sampler] = (__bridge_transfer id<MTLSamplerState>)reinterpret_cast<void*>(made);
}

void Mirror::SamplerDestroyed(Sampler sampler) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.samplers.erase(sampler);
}

void Mirror::ShaderTagged(Shader shader, bool vertex, uint64_t hash, bool early_z) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.shaders[shader] = ShaderTag{vertex, hash, early_z};
}

void Mirror::ShaderDestroyed(Shader shader) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.shaders.erase(shader);
}

void Mirror::SetLayoutCreated(SetLayout layout, const SetBinding* bindings, uint32_t count) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.set_layouts[layout].bindings.assign(bindings, bindings + count);
}

void Mirror::SetsAllocated(uint32_t count, const SetLayout* layouts, const Set* sets) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  for (uint32_t i = 0; i < count; ++i) {
    auto layout = s.set_layouts.find(layouts[i]);
    if (layout == s.set_layouts.end()) continue;
    SetM set;
    for (const SetBinding& b : layout->second.bindings) {
      if ((b.kind == kDescriptorSampledImage || b.kind == kDescriptorSampler) && b.binding == 0 && b.count > 1) {
        set.capacity = b.count;
        set.heap = [s.mtl newBufferWithLength:NSUInteger(b.count) * 8 options:MTLResourceStorageModeShared];
        set.heap.label = @"bo2zm mirror heap";
        s.Resident(set.heap, true);
      } else if (b.kind == kDescriptorUniformBuffer || b.kind == kDescriptorUniformBufferDynamic) {
        if (b.binding < set.uniforms.size()) {
          set.uniforms[b.binding].dynamic = b.kind == kDescriptorUniformBufferDynamic;
          set.n_uniforms = std::max(set.n_uniforms, b.binding + 1);
        }
      }
    }
    s.sets[sets[i]] = set;
  }
}

void Mirror::WriteSampledImage(Set set, uint32_t binding, uint32_t element, ImageView view) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  auto found = s.sets.find(set);
  if (found == s.sets.end() || !found->second.heap || binding != 0 || element >= found->second.capacity) return;
  auto v = s.views.find(view);
  uint64_t id = 0;
  if (v != s.views.end() && v->second.texture) {
    id = v->second.texture.gpuResourceID._impl;
  }
  static_cast<uint64_t*>(found->second.heap.contents)[element] = id;
}

void Mirror::WriteSampler(Set set, uint32_t binding, uint32_t element, Sampler sampler) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  auto found = s.sets.find(set);
  if (found == s.sets.end() || !found->second.heap || binding != 0 || element >= found->second.capacity) return;
  auto m = s.samplers.find(sampler);
  const uint64_t id = m != s.samplers.end() ? m->second.gpuResourceID._impl : 0;
  static_cast<uint64_t*>(found->second.heap.contents)[element] = id;
}

void Mirror::WriteUniform(Set set, uint32_t binding, Buffer buffer, Size offset) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  auto found = s.sets.find(set);
  if (found == s.sets.end() || binding >= found->second.uniforms.size()) return;
  found->second.uniforms[binding].buffer = buffer;
  found->second.uniforms[binding].offset = offset;
  found->second.n_uniforms = std::max(found->second.n_uniforms, binding + 1);
}

void Mirror::PassCreated(Pass pass, const PassDesc& desc) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.passes[pass].desc = desc;
}

void Mirror::FramebufferCreated(Framebuffer framebuffer, Pass pass, uint32_t count, const ImageView* views,
                                uint32_t width, uint32_t height) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  FramebufferM& f = s.framebuffers[framebuffer];
  f.pass = pass;
  f.views.assign(views, views + count);
  f.width = width;
  f.height = height;
}

void Mirror::FramebufferDestroyed(Framebuffer framebuffer) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.framebuffers.erase(framebuffer);
}

void Mirror::PipelineCreated(Pipeline pipeline, const GraphicsPipelineDesc& desc) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  PipelineM p;
  if (auto vs = s.shaders.find(desc.vs); vs != s.shaders.end()) {
    p.vs_tagged = true;
    p.vs = vs->second;
  }
  p.has_ps = desc.ps != nullptr;
  if (p.has_ps) {
    if (auto ps = s.shaders.find(desc.ps); ps != s.shaders.end()) {
      p.ps_tagged = true;
      p.ps = ps->second;
    }
  }
  p.constant = desc.n_constants > 0 && desc.constants ? desc.constants[0] : 0;
  if (desc.strides) p.strides.assign(desc.strides, desc.strides + desc.n_bindings);
  if (desc.attributes) p.attributes.assign(desc.attributes, desc.attributes + desc.n_attributes);
  if (desc.state) p.state = *desc.state;
  s.pipelines[pipeline] = std::move(p);
}

void Mirror::PipelineDestroyed(Pipeline pipeline) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.pipelines.erase(pipeline);
}

void Mirror::Begin(CommandList list) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  ListM& l = s.List(list);
  s.EndEncoders(l);
  l = ListM{};
}

void Mirror::Submit(uint32_t count, const CommandList* lists) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  if (s.residency && s.residency_dirty) {
    [s.residency commit];
    s.residency_dirty = false;
  }
  id<MTLCommandBuffer> last = nil;
  for (uint32_t i = 0; i < count; ++i) {
    auto found = s.lists.find(lists[i]);
    if (found == s.lists.end()) continue;
    ListM& l = found->second;
    s.EndEncoders(l);
    if (l.commands) {
      [l.commands commit];
      last = l.commands;
      l.commands = nil;
    }
  }
  if (last) {
    [last waitUntilCompleted];
    if (last.error) {
      s.Once(std::string("command buffer failed: ") + [[last.error localizedDescription] UTF8String]);
    }
  }
}

void Mirror::BeginPass(CommandList list, Pass pass, Framebuffer framebuffer, const Rect& area, uint32_t n_clears,
                       const ClearValue* clears) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  (void)area;  // Metal's load actions clear whole attachments; the renderer's passes cover them
  ListM& l = s.List(list);
  l.pass = pass;
  l.framebuffer = framebuffer;
  s.StartPass(l, false, n_clears, clears, -1, false, false, nullptr);
}

void Mirror::EndPass(CommandList list) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  ListM& l = s.List(list);
  if (l.render) {
    [l.render endEncoding];
    l.render = nil;
  }
  l.pass = nullptr;
  l.framebuffer = nullptr;
}

void Mirror::SetViewport(CommandList list, const Viewport& viewport) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.List(list).bound.viewport = viewport;
}

void Mirror::SetScissor(CommandList list, const Rect& scissor) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.List(list).bound.scissor = scissor;
}

void Mirror::SetBlendConstants(CommandList list, const float constants[4]) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  std::memcpy(s.List(list).bound.blend, constants, sizeof(float) * 4);
}

void Mirror::SetDepthBias(CommandList list, float constant, float slope) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  Bound& b = s.List(list).bound;
  b.bias_constant = constant;
  b.bias_slope = slope;
}

void Mirror::SetStencilValues(CommandList list, uint32_t front, uint32_t back) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  Bound& b = s.List(list).bound;
  b.stencil_front = front;
  b.stencil_back = back;
}

void Mirror::BindPipeline(CommandList list, Pipeline pipeline) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.List(list).bound.pipeline = pipeline;
}

void Mirror::BindSets(CommandList list, uint32_t first, uint32_t count, const Set* sets, uint32_t n_offsets,
                      const uint32_t* offsets) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  Bound& b = s.List(list).bound;
  for (uint32_t i = 0; i < count && first + i < b.sets.size(); ++i) {
    b.sets[first + i] = sets[i];
  }
  // The dynamic offsets belong to the uniform set (set 4 of the pack shaders' layout).
  if (n_offsets && offsets) {
    for (uint32_t i = 0; i < n_offsets && i < b.dynamic_offsets.size(); ++i) {
      b.dynamic_offsets[i] = offsets[i];
    }
  }
}

void Mirror::BindVertexBuffers(CommandList list, uint32_t count, const Buffer* buffers, const Size* offsets) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  Bound& b = s.List(list).bound;
  b.n_vertex = std::min<uint32_t>(count, uint32_t(b.vertex.size()));
  for (uint32_t i = 0; i < b.n_vertex; ++i) {
    b.vertex[i] = buffers[i];
    b.vertex_offsets[i] = offsets[i];
  }
}

void Mirror::BindIndexBuffer(CommandList list, Buffer buffer, bool indices_16) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  Bound& b = s.List(list).bound;
  b.index = buffer;
  b.indices_16 = indices_16;
}

void Mirror::Draw(CommandList list, uint32_t count, uint32_t first_vertex) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  ListM& l = s.List(list);
  MTLPrimitiveType primitive;
  if (!s.Prepare(l, primitive)) {
    ++s.draws_skipped;
    return;
  }
  if (s.Fan(l)) {
    uint32_t triangles = 0;
    id<MTLBuffer> list_indices = s.FanIndices(nullptr, false, count, 0, triangles);
    if (list_indices) {
      [l.render drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                           indexCount:triangles * 3
                            indexType:MTLIndexTypeUInt32
                          indexBuffer:list_indices
                    indexBufferOffset:0
                        instanceCount:1
                           baseVertex:first_vertex
                         baseInstance:0];
    }
    ++s.draws;
    ++s.fans;
    return;
  }
  [l.render drawPrimitives:primitive vertexStart:first_vertex vertexCount:count];
  ++s.draws;
}

void Mirror::DrawIndexed(CommandList list, uint32_t count, uint32_t first_index, int32_t vertex_offset) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  ListM& l = s.List(list);
  MTLPrimitiveType primitive;
  auto index = s.buffers.find(l.bound.index);
  if (index == s.buffers.end()) {
    s.Once("index buffer the mirror did not see created");
    ++s.draws_skipped;
    return;
  }
  if (!s.Prepare(l, primitive)) {
    ++s.draws_skipped;
    return;
  }
  const Size size = l.bound.indices_16 ? 2 : 4;
  if (s.Fan(l)) {
    // The indices are read on the CPU: the buffer wraps host memory the renderer wrote before recording.
    const uint8_t* source = static_cast<const uint8_t*>(index->second.buffer.contents) + index->second.offset +
                            first_index * size;
    uint32_t triangles = 0;
    id<MTLBuffer> list_indices =
        s.FanIndices(source, l.bound.indices_16, count, l.bound.indices_16 ? 0xFFFFu : 0xFFFFFFFFu, triangles);
    if (list_indices) {
      [l.render drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                           indexCount:triangles * 3
                            indexType:MTLIndexTypeUInt32
                          indexBuffer:list_indices
                    indexBufferOffset:0
                        instanceCount:1
                           baseVertex:vertex_offset
                         baseInstance:0];
    }
    ++s.draws;
    ++s.fans;
    return;
  }
  [l.render drawIndexedPrimitives:primitive
                       indexCount:count
                        indexType:l.bound.indices_16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                      indexBuffer:index->second.buffer
                indexBufferOffset:index->second.offset + first_index * size
                    instanceCount:1
                       baseVertex:vertex_offset
                     baseInstance:0];
  ++s.draws;
}

void Mirror::ClearAttachment(CommandList list, Aspects aspects, uint32_t color_index, const ClearValue& value,
                             const Rect& area) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  ListM& l = s.List(list);
  if (!l.render) return;
  const bool color = (aspects & kAspectColor) != 0;
  if (area.x != 0 || area.y != 0 || area.width < l.width || area.height < l.height) {
    // Part of it (one split-screen view, for example): a rectangle drawn with the value.
    s.ClearArea(l, color ? int(color_index) : -1, (aspects & kAspectDepth) != 0, (aspects & kAspectStencil) != 0,
                value, area);
    ++s.clears_part;
    return;
  }
  // The whole attachment: the encoder restarts with a clear load action for it.
  s.StartPass(l, true, 0, nullptr, color ? int(color_index) : -1, (aspects & kAspectDepth) != 0,
              (aspects & kAspectStencil) != 0, &value);
}

void Mirror::ClearColorImage(CommandList list, Image image, const ClearColor& color, uint32_t count,
                             const Range* ranges) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  id<MTLTexture> texture = s.Texture(image);
  if (!texture || !(texture.usage & MTLTextureUsageRenderTarget)) return;
  ListM& l = s.List(list);
  for (uint32_t r = 0; r < count; ++r) {
    const uint32_t levels = ranges[r].levels == kAllLevels ? uint32_t(texture.mipmapLevelCount) : ranges[r].levels;
    const uint32_t layers = ranges[r].layers == kAllLevels ? 1u : ranges[r].layers;
    for (uint32_t m = 0; m < levels; ++m) {
      for (uint32_t a = 0; a < layers; ++a) {
        s.ClearSlice(l, texture, ranges[r].base_level + m, ranges[r].base_layer + a, &color, nullptr);
      }
    }
  }
}

void Mirror::ClearDepthStencilImage(CommandList list, Image image, const ClearDepthStencil& value, uint32_t count,
                                    const Range* ranges) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  id<MTLTexture> texture = s.Texture(image);
  if (!texture) return;
  ListM& l = s.List(list);
  for (uint32_t r = 0; r < count; ++r) {
    const uint32_t layers = ranges[r].layers == kAllLevels ? 1u : ranges[r].layers;
    for (uint32_t a = 0; a < layers; ++a) {
      s.ClearSlice(l, texture, ranges[r].base_level, ranges[r].base_layer + a, nullptr, &value);
    }
  }
}

void Mirror::CopyBuffer(CommandList list, Buffer source, Buffer target, uint32_t count, const BufferCopy* regions) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  auto from = s.buffers.find(source);
  auto to = s.buffers.find(target);
  if (from == s.buffers.end() || to == s.buffers.end()) return;
  id<MTLBlitCommandEncoder> blit = s.Blit(s.List(list));
  for (uint32_t i = 0; i < count; ++i) {
    [blit copyFromBuffer:from->second.buffer
             sourceOffset:from->second.offset + regions[i].source_offset
                 toBuffer:to->second.buffer
        destinationOffset:to->second.offset + regions[i].target_offset
                     size:regions[i].size];
  }
}

void Mirror::CopyBufferToImage(CommandList list, Buffer source, Image target, uint32_t count,
                               const BufferImageCopy* regions) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  auto from = s.buffers.find(source);
  auto to = s.images.find(target);
  if (from == s.buffers.end() || to == s.images.end()) return;
  uint32_t bytes = 0;
  uint32_t block = 1;
  if (!FormatBlock(to->second.desc.format, bytes, block)) {
    s.Once("upload to an image format the mirror cannot size: " + std::to_string(to->second.desc.format));
    return;
  }
  if (IsDepth(to->second.texture.pixelFormat)) {
    s.Once("upload to a depth image is not done yet");
    return;
  }
  id<MTLTexture> texture = to->second.texture;
  id<MTLBlitCommandEncoder> blit = s.Blit(s.List(list));
  for (uint32_t i = 0; i < count; ++i) {
    const BufferImageCopy& r = regions[i];
    const uint32_t row = r.row_length ? r.row_length : r.extent.width;
    const uint32_t rows = r.image_height ? r.image_height : r.extent.height;
    const Size bytes_row = Size((row + block - 1) / block) * bytes;
    const Size bytes_image = bytes_row * ((rows + block - 1) / block);
    const uint32_t level = r.layers.level;
    const uint32_t mip_w = std::max<uint32_t>(uint32_t(texture.width) >> level, 1);
    const uint32_t mip_h = std::max<uint32_t>(uint32_t(texture.height) >> level, 1);
    const uint32_t w = std::min<uint32_t>(r.extent.width, mip_w - uint32_t(r.offset.x));
    const uint32_t h = std::min<uint32_t>(r.extent.height, mip_h - uint32_t(r.offset.y));
    const uint32_t d = std::max<uint32_t>(r.extent.depth, 1);
    for (uint32_t layer = 0; layer < r.layers.layers; ++layer) {
      [blit copyFromBuffer:from->second.buffer
                 sourceOffset:from->second.offset + r.buffer_offset + layer * bytes_image * d
            sourceBytesPerRow:bytes_row
          sourceBytesPerImage:texture.textureType == MTLTextureType3D ? bytes_image : 0
                   sourceSize:MTLSizeMake(w, h, d)
                    toTexture:texture
             destinationSlice:r.layers.base_layer + layer
             destinationLevel:level
            destinationOrigin:MTLOriginMake(NSUInteger(r.offset.x), NSUInteger(r.offset.y), NSUInteger(r.offset.z))];
    }
  }
}

void Mirror::CopyImage(CommandList list, Image source, Image target, uint32_t count, const ImageCopy* regions) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  id<MTLTexture> from = s.Texture(source);
  id<MTLTexture> to = s.Texture(target);
  if (!from || !to) return;
  if (from.pixelFormat != to.pixelFormat) {
    s.Once("copy between images of different formats is not done yet");
    return;
  }
  id<MTLBlitCommandEncoder> blit = s.Blit(s.List(list));
  for (uint32_t i = 0; i < count; ++i) {
    const ImageCopy& r = regions[i];
    for (uint32_t layer = 0; layer < r.source.layers; ++layer) {
      [blit copyFromTexture:from
                sourceSlice:r.source.base_layer + layer
                sourceLevel:r.source.level
               sourceOrigin:MTLOriginMake(NSUInteger(r.source_offset.x), NSUInteger(r.source_offset.y),
                                          NSUInteger(r.source_offset.z))
                 sourceSize:MTLSizeMake(r.extent.width, r.extent.height, std::max<uint32_t>(r.extent.depth, 1))
                  toTexture:to
           destinationSlice:r.target.base_layer + layer
           destinationLevel:r.target.level
          destinationOrigin:MTLOriginMake(NSUInteger(r.target_offset.x), NSUInteger(r.target_offset.y),
                                          NSUInteger(r.target_offset.z))];
    }
  }
}

void Mirror::Blit(CommandList list, Image source, Image target, uint32_t count, const ImageBlit* regions) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  id<MTLTexture> from = s.Texture(source);
  id<MTLTexture> to = s.Texture(target);
  if (!from || !to) return;
  // Unscaled blits of one format are copies; the scaled ones need a draw (not done yet).
  id<MTLBlitCommandEncoder> blit = nil;
  for (uint32_t i = 0; i < count; ++i) {
    const ImageBlit& r = regions[i];
    const int32_t sw = r.source_offsets[1].x - r.source_offsets[0].x;
    const int32_t sh = r.source_offsets[1].y - r.source_offsets[0].y;
    const int32_t tw = r.target_offsets[1].x - r.target_offsets[0].x;
    const int32_t th = r.target_offsets[1].y - r.target_offsets[0].y;
    if (sw != tw || sh != th || sw <= 0 || sh <= 0 || from.pixelFormat != to.pixelFormat) {
      s.Once("scaled or converting blit is not done yet");
      continue;
    }
    if (!blit) blit = s.Blit(s.List(list));
    [blit copyFromTexture:from
              sourceSlice:r.source.base_layer
              sourceLevel:r.source.level
             sourceOrigin:MTLOriginMake(NSUInteger(r.source_offsets[0].x), NSUInteger(r.source_offsets[0].y), 0)
               sourceSize:MTLSizeMake(NSUInteger(sw), NSUInteger(sh), 1)
                toTexture:to
         destinationSlice:r.target.base_layer
         destinationLevel:r.target.level
        destinationOrigin:MTLOriginMake(NSUInteger(r.target_offsets[0].x), NSUInteger(r.target_offsets[0].y), 0)];
  }
}

void Mirror::Skipped(const char* what) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  s.Once(std::string(what) + " (stub)");
}

void Mirror::Showed(ImageView view, uint32_t width, uint32_t height) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  auto found = s.views.find(view);
  s.shown = found == s.views.end() ? nil : found->second.texture;
  s.shown_width = s.shown ? std::min<uint32_t>(width ? width : uint32_t(s.shown.width), uint32_t(s.shown.width)) : 0;
  s.shown_height =
      s.shown ? std::min<uint32_t>(height ? height : uint32_t(s.shown.height), uint32_t(s.shown.height)) : 0;
  if (++s.frames % 600 == 1) {
    REXLOG_INFO("[native] Metal mirror: frame {}, {} draws made, {} skipped, {} fans, {} part clears, shown {}x{} "
                "of a {}x{} image", s.frames, s.draws, s.draws_skipped, s.fans, s.clears_part, s.shown_width,
                s.shown_height, s.shown ? s.shown.width : 0, s.shown ? s.shown.height : 0);
  }
}

void* Mirror::Shown(uint32_t& width, uint32_t& height) {
  State& s = *state_;
  std::lock_guard guard(s.lock);
  if (!s.shown || !s.shown_width || !s.shown_height) return nullptr;
  width = s.shown_width;
  height = s.shown_height;
  return (__bridge void*)s.shown;
}

}  // namespace bo2zm::native::gpu::metal
