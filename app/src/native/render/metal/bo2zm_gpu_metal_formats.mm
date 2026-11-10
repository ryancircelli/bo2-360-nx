// Native renderer, Metal backend: image and vertex formats.
//
// The table follows plume's mapPixelFormat (MIT, Copyright (c) 2024 renderbag and contributors), keyed by the Vulkan
// formats the native renderer uses instead of plume's RenderFormat. Vulkan's headers are only read for the enum
// values of VkFormat (gpu::Format uses that numbering); nothing here calls Vulkan.
//
// The USCALED and SSCALED vertex formats (integers read as floats) map to the integer formats: Metal has no scaled
// formats, so the vertex shaders that read them will need to convert (to settle when the Metal draws come up).

#include "bo2zm_gpu_metal.h"

#include <vulkan/vulkan_core.h>

#import <Metal/Metal.h>

namespace bo2zm::native::gpu::metal {

uint32_t PixelFormat(Format format) {
  switch (VkFormat(format)) {
    case VK_FORMAT_UNDEFINED:
      return uint32_t(MTLPixelFormatInvalid);
    case VK_FORMAT_R8_UNORM:
      return uint32_t(MTLPixelFormatR8Unorm);
    case VK_FORMAT_R8G8_UNORM:
      return uint32_t(MTLPixelFormatRG8Unorm);
    case VK_FORMAT_R8G8B8A8_UNORM:
      return uint32_t(MTLPixelFormatRGBA8Unorm);
    case VK_FORMAT_R8G8B8A8_SNORM:
      return uint32_t(MTLPixelFormatRGBA8Snorm);
    case VK_FORMAT_R8G8B8A8_UINT:
      return uint32_t(MTLPixelFormatRGBA8Uint);
    case VK_FORMAT_R8G8B8A8_SINT:
      return uint32_t(MTLPixelFormatRGBA8Sint);
    case VK_FORMAT_B8G8R8A8_UNORM:
      return uint32_t(MTLPixelFormatBGRA8Unorm);
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return uint32_t(MTLPixelFormatRGB10A2Unorm);
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
      return uint32_t(MTLPixelFormatRGB10A2Uint);
    case VK_FORMAT_R16_UNORM:
      return uint32_t(MTLPixelFormatR16Unorm);
    case VK_FORMAT_R16_SFLOAT:
      return uint32_t(MTLPixelFormatR16Float);
    case VK_FORMAT_R16G16_UNORM:
      return uint32_t(MTLPixelFormatRG16Unorm);
    case VK_FORMAT_R16G16_SNORM:
      return uint32_t(MTLPixelFormatRG16Snorm);
    case VK_FORMAT_R16G16_UINT:
      return uint32_t(MTLPixelFormatRG16Uint);
    case VK_FORMAT_R16G16_SINT:
      return uint32_t(MTLPixelFormatRG16Sint);
    case VK_FORMAT_R16G16_SFLOAT:
      return uint32_t(MTLPixelFormatRG16Float);
    case VK_FORMAT_R16G16B16A16_UNORM:
      return uint32_t(MTLPixelFormatRGBA16Unorm);
    case VK_FORMAT_R16G16B16A16_SNORM:
      return uint32_t(MTLPixelFormatRGBA16Snorm);
    case VK_FORMAT_R16G16B16A16_UINT:
      return uint32_t(MTLPixelFormatRGBA16Uint);
    case VK_FORMAT_R16G16B16A16_SINT:
      return uint32_t(MTLPixelFormatRGBA16Sint);
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return uint32_t(MTLPixelFormatRGBA16Float);
    case VK_FORMAT_R32_UINT:
      return uint32_t(MTLPixelFormatR32Uint);
    case VK_FORMAT_R32_SINT:
      return uint32_t(MTLPixelFormatR32Sint);
    case VK_FORMAT_R32_SFLOAT:
      return uint32_t(MTLPixelFormatR32Float);
    case VK_FORMAT_R32G32_UINT:
      return uint32_t(MTLPixelFormatRG32Uint);
    case VK_FORMAT_R32G32_SINT:
      return uint32_t(MTLPixelFormatRG32Sint);
    case VK_FORMAT_R32G32_SFLOAT:
      return uint32_t(MTLPixelFormatRG32Float);
    case VK_FORMAT_R32G32B32A32_UINT:
      return uint32_t(MTLPixelFormatRGBA32Uint);
    case VK_FORMAT_R32G32B32A32_SINT:
      return uint32_t(MTLPixelFormatRGBA32Sint);
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      return uint32_t(MTLPixelFormatRGBA32Float);
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      return uint32_t(MTLPixelFormatBC1_RGBA);
    case VK_FORMAT_BC2_UNORM_BLOCK:
      return uint32_t(MTLPixelFormatBC2_RGBA);
    case VK_FORMAT_BC3_UNORM_BLOCK:
      return uint32_t(MTLPixelFormatBC3_RGBA);
    case VK_FORMAT_BC4_UNORM_BLOCK:
      return uint32_t(MTLPixelFormatBC4_RUnorm);
    case VK_FORMAT_BC5_UNORM_BLOCK:
      return uint32_t(MTLPixelFormatBC5_RGUnorm);
    case VK_FORMAT_D16_UNORM:
      return uint32_t(MTLPixelFormatDepth16Unorm);
    case VK_FORMAT_D32_SFLOAT:
      return uint32_t(MTLPixelFormatDepth32Float);
    case VK_FORMAT_X8_D24_UNORM_PACK32:
      return uint32_t(MTLPixelFormatDepth32Float);
    case VK_FORMAT_D24_UNORM_S8_UINT:
      return uint32_t(MTLPixelFormatDepth32Float_Stencil8);
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return uint32_t(MTLPixelFormatDepth32Float_Stencil8);
    default:
      return uint32_t(MTLPixelFormatInvalid);
  }
}

uint32_t VertexFormat(Format format) {
  switch (VkFormat(format)) {
    case VK_FORMAT_R8G8B8A8_UNORM:
      return uint32_t(MTLVertexFormatUChar4Normalized);
    case VK_FORMAT_R8G8B8A8_SNORM:
      return uint32_t(MTLVertexFormatChar4Normalized);
    case VK_FORMAT_R8G8B8A8_UINT:
      return uint32_t(MTLVertexFormatUChar4);
    case VK_FORMAT_R8G8B8A8_SINT:
      return uint32_t(MTLVertexFormatChar4);
    case VK_FORMAT_R8G8B8A8_USCALED:
      return uint32_t(MTLVertexFormatUChar4);
    case VK_FORMAT_R8G8B8A8_SSCALED:
      return uint32_t(MTLVertexFormatChar4);
    case VK_FORMAT_B8G8R8A8_UNORM:
      return uint32_t(MTLVertexFormatUChar4Normalized_BGRA);
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return uint32_t(MTLVertexFormatUInt1010102Normalized);
    case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
      return uint32_t(MTLVertexFormatInt1010102Normalized);
    case VK_FORMAT_R16G16_UNORM:
      return uint32_t(MTLVertexFormatUShort2Normalized);
    case VK_FORMAT_R16G16_SNORM:
      return uint32_t(MTLVertexFormatShort2Normalized);
    case VK_FORMAT_R16G16_UINT:
      return uint32_t(MTLVertexFormatUShort2);
    case VK_FORMAT_R16G16_SINT:
      return uint32_t(MTLVertexFormatShort2);
    case VK_FORMAT_R16G16_SFLOAT:
      return uint32_t(MTLVertexFormatHalf2);
    case VK_FORMAT_R16G16B16A16_UNORM:
      return uint32_t(MTLVertexFormatUShort4Normalized);
    case VK_FORMAT_R16G16B16A16_SNORM:
      return uint32_t(MTLVertexFormatShort4Normalized);
    case VK_FORMAT_R16G16B16A16_UINT:
      return uint32_t(MTLVertexFormatUShort4);
    case VK_FORMAT_R16G16B16A16_SINT:
      return uint32_t(MTLVertexFormatShort4);
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return uint32_t(MTLVertexFormatHalf4);
    case VK_FORMAT_R32_UINT:
      return uint32_t(MTLVertexFormatUInt);
    case VK_FORMAT_R32_SINT:
      return uint32_t(MTLVertexFormatInt);
    case VK_FORMAT_R32_SFLOAT:
      return uint32_t(MTLVertexFormatFloat);
    case VK_FORMAT_R32G32_UINT:
      return uint32_t(MTLVertexFormatUInt2);
    case VK_FORMAT_R32G32_SINT:
      return uint32_t(MTLVertexFormatInt2);
    case VK_FORMAT_R32G32_SFLOAT:
      return uint32_t(MTLVertexFormatFloat2);
    case VK_FORMAT_R32G32B32_UINT:
      return uint32_t(MTLVertexFormatUInt3);
    case VK_FORMAT_R32G32B32_SFLOAT:
      return uint32_t(MTLVertexFormatFloat3);
    case VK_FORMAT_R32G32B32A32_UINT:
      return uint32_t(MTLVertexFormatUInt4);
    case VK_FORMAT_R32G32B32A32_SINT:
      return uint32_t(MTLVertexFormatInt4);
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      return uint32_t(MTLVertexFormatFloat4);
    default:
      return uint32_t(MTLVertexFormatInvalid);
  }
}

}  // namespace bo2zm::native::gpu::metal
