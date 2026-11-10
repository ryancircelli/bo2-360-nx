// Build: clang++ -std=c++17 -fobjc-arc half_bits_tile_test.mm -framework Metal -framework Foundation -o half_bits_tile_test
// Synthetic test: do all 65536 half bit patterns survive an RGBA16Float
// attachment load into tile memory, read in a tile kernel as half4 and
// reinterpreted with as_type<ushort4>? Mirrors resolve_tile_entry.inc.
#import <Metal/Metal.h>
#include <cstdio>
#include <vector>
static const char* kSrc = R"(
#include <metal_stdlib>
using namespace metal;
struct C { half4 color [[color(0)]]; };
kernel void k(imageblock<C, imageblock_layout_implicit> blk,
              device ushort4* out [[buffer(0)]],
              constant uint& width [[buffer(1)]],
              ushort2 lid [[thread_position_in_threadgroup]],
              ushort2 tg [[threadgroup_position_in_grid]],
              ushort2 tpt [[threads_per_threadgroup]]) {
  uint2 p = uint2(tg) * uint2(tpt) + uint2(lid);
  out[p.y * width + p.x] = as_type<ushort4>(blk.read(lid).color);
}
)";
int main() {
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSError* err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSrc) options:nil error:&err];
    if (!lib) { printf("lib: %s\n", err.description.UTF8String); return 1; }
    const uint32_t W = 128, H = 128;  // 16384 px * 4 = 65536 halves
    std::vector<uint16_t> in(W * H * 4);
    for (uint32_t i = 0; i < in.size(); ++i) in[i] = uint16_t(i);
    MTLTextureDescriptor* td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    [tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:in.data() bytesPerRow:W * 8];
    MTLTileRenderPipelineDescriptor* pd = [MTLTileRenderPipelineDescriptor new];
    pd.tileFunction = [lib newFunctionWithName:@"k"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    pd.threadgroupSizeMatchesTileSize = YES;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithTileDescriptor:pd options:0 reflection:nil error:&err];
    if (!ps) { printf("pso: %s\n", err.description.UTF8String); return 1; }
    id<MTLBuffer> out = [dev newBufferWithLength:in.size() * 2 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.tileWidth = 16; rp.tileHeight = 16;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps];
    [e setTileBuffer:out offset:0 atIndex:0];
    [e setTileBytes:&W length:4 atIndex:1];
    [e dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)];
    [e endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { printf("cb: %s\n", cb.error.description.UTF8String); return 1; }
    const uint16_t* o = (const uint16_t*)out.contents;
    std::vector<uint16_t> back(in.size());
    [tex getBytes:back.data() bytesPerRow:W * 8 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    uint32_t bad = 0, bad_den = 0, bad_nan = 0, bad_other = 0, den = 0, nan = 0, store_bad = 0;
    for (uint32_t i = 0; i < in.size(); ++i) {
      uint16_t v = in[i], ex = v & 0x7C00, m = v & 0x3FF;
      bool isden = ex == 0 && m, isnan = ex == 0x7C00 && m;
      den += isden; nan += isnan;
      if (back[i] != v) ++store_bad;
      if (o[i] != v) {
        if (bad < 8) printf("mismatch 0x%04X -> 0x%04X\n", v, o[i]);
        ++bad; bad_den += isden; bad_nan += isnan; bad_other += !isden && !isnan;
      }
    }
    printf("%s: 65536 patterns (%u denormal, %u NaN): tile read mismatches %u "
           "(denormal %u, NaN %u, other %u); store-back mismatches %u\n",
           dev.name.UTF8String, den, nan, bad, bad_den, bad_nan, bad_other, store_bad);
    return bad ? 2 : 0;
  }
}
