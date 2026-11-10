#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)
// BO2ZM: constants arrive through a dynamic UBO (a constant bank on Maxwell) instead of being read
// through a 64-bit pointer. High bit so as not to clash with the UNLEASHED_RECOMP ones.
#define SPEC_CONSTANT_CONSTANTS_UBO    (1 << 8)
// The tfetch offset is scaled with 1/size taken from the shared constants instead of querying the
// texture for its size. A size query is another texture unit operation, paid on every offset sample.
#define SPEC_CONSTANT_INV_TEX_SIZE    (1 << 9)
// Cheap PCF. The shaders that sample the shadow map use a 3x3 pattern at half-texel offsets: nine
// samples per pixel, more in some. With this bit the eight outer offsets are
// set to zero: the nine samples become identical, DXC merges them into one and the shadow goes from
// 3x3 filtered to a single texel. Edge smoothness is lost.
#define SPEC_CONSTANT_PCF_CHEAP        (1 << 15)
/*
 * The alpha test function, specialized (bits 16-18).
 *
 * g_AlphaFunction used to come from the shared constants, that is, at run time, so alphaTestValue
 * was a 7-case switch: ~8 branches in every pixel shader that does alpha testing, even in shaders
 * with a single sample and a handful of operations.
 * And it was not needed at all: the app already knows the function when it creates the pipeline (it
 * comes from RB_COLORCONTROL, which is already in the key), so with three more bits the compiler
 * keeps the one comparison that applies and drops the other six. The output is bit-identical.
 */
/*
 * The radial blur of the final composition.
 *
 * The final composition is one full-screen quad with no overdraw and twelve samples, the most
 * expensive part of the post-processing.
 *
 * Of those twelve, seven are offset DIFFUSEMAP taps (plus one HEIGHTMAP tap that only feeds the
 * factor): the radial blur. They are blended with `r5*factor + r3`, where r3 is the center tap,
 * so with factor 0 the output is exactly the center tap and the eight samples are dead: DXC and the
 * driver remove them when specializing.
 *
 * The blur at the edges of the screen is lost. The image is sharper.
 */
#define SPEC_CONSTANT_NO_BLUR    (1 << 19)
/*
 * The world shadow map as the minimum of two textures (bo2zm_native_shadow_min).
 *
 * A shadow pass can resolve the same map twice: without its overlay (sampled by the overlay's own
 * materials) and with the overlay drawn on top (sampled by the world). With this option the renderer
 * draws the overlay onto a render target cleared to 1.0, without copying the world underneath, and the
 * shaders that sample the full map take the minimum of the two textures: with a LESS/LEQUAL depth test,
 * min(world, overlay) is exactly what drawing the overlay on top produces. The second texture arrives in the
 * 3D index word of the same register (the shadow map is 2D: that word is unused). With the bit off the
 * code is that of tfetch2DShadow.
 */
#define SPEC_CONSTANT_SHADOW_MIN     (1 << 23)
/*
 * The app knows the library has tfetch2DShadowMin because this constant appears in the SPIR-V of the
 * shaders that use it (OpConstant) and nowhere else. The specialization constant never reaches this
 * value (bits 24-30).
 */
#define BO2ZM_SHADOW_MIN_MARKER       0x5E3B1A84u
#define SPEC_CONSTANT_ALPHA_FUNC_SHIFT  16
#define SPEC_CONSTANT_ALPHA_FUNC_MASK   (7 << 16)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

// BO2ZM: the same blocks of the upload buffer, also as dynamic UBOs in set 4. With
// -fvk-use-dx-layout each float4 takes 16 contiguous bytes, so any 4-byte word of the shared block is
// a component: v[B / 16][(B % 16) / 4], and asuint reads it without changing a bit.
struct Bo2zmBlockVs { float4 v[256]; };
// BO2ZM_RECOMP: BO2's post-processing reads pixel constants up to c250 (the color-grading table and the
// tonemap). With 224, everything above read as zero and the in-game image was black.
struct Bo2zmBlockPs { float4 v[256]; };
// BO2ZM_RECOMP: 35, not 23. The renderer fills 31 (1/size of the 16 slots ends at byte 488) and BO2 adds the
// slots' fetch scales (bytes 488-551). With 23, slot 2 and up read 1/size past the end of the array.
// 54 since the loop counts (bytes 688-719) and the vertex shaders' texture slots (bytes 720-863).
struct Bo2zmBlockShared { float4 v[54]; };  // kWordsShared / 4 in bo2zm_native_draws.cpp
[[vk::binding(0, 4)]] ConstantBuffer<Bo2zmBlockVs> g_UboVertex;
[[vk::binding(1, 4)]] ConstantBuffer<Bo2zmBlockPs> g_UboPixel;
[[vk::binding(2, 4)]] ConstantBuffer<Bo2zmBlockShared> g_UboShared;
#define BO2ZM_UBO ((g_SpecConstants & SPEC_CONSTANT_CONSTANTS_UBO) != 0)
#define BO2ZM_SHARED_UINT(B)  asuint(g_UboShared.v[(B) / 16][((B) % 16) / 4])
#define BO2ZM_SHARED_FLOAT(B) g_UboShared.v[(B) / 16][((B) % 16) / 4]

#define g_Booleans                 (BO2ZM_UBO ? BO2ZM_SHARED_UINT(256) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256))
#define g_SwappedTexcoords         (BO2ZM_UBO ? BO2ZM_SHARED_UINT(260) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260))
#define g_HalfPixelOffset          (BO2ZM_UBO ? float2(BO2ZM_SHARED_FLOAT(264), BO2ZM_SHARED_FLOAT(268)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264))
#define g_AlphaThreshold           (BO2ZM_UBO ? BO2ZM_SHARED_FLOAT(272) : vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272))
// BO2ZM: alpha test function (RB_COLORCONTROL.alpha_func): 0 never, 1 <, 2 ==, 3 <=,
// 4 >, 5 !=, 6 >=, 7 always.
#define g_AlphaFunction            (BO2ZM_UBO ? BO2ZM_SHARED_UINT(276) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 276))
// BO2ZM: position to host clip space, like ndc_scale/ndc_offset in the
// emulation (graphics/util/draw.cpp). (1, 1) and (0, 0) for normal draws;
// with Xenos clipping disabled it converts from pixels to NDC.
#define g_NdcScale                 (BO2ZM_UBO ? float2(BO2ZM_SHARED_FLOAT(280), BO2ZM_SHARED_FLOAT(284)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 280))
#define g_NdcOffset                (BO2ZM_UBO ? float2(BO2ZM_SHARED_FLOAT(288), BO2ZM_SHARED_FLOAT(292)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 288))
// BO2ZM: where each component of the vertex input at that location comes from
// (D3D patches the fetch swizzle according to the declaration). 0xFFF = as is.
#define g_InputRemap(LOC)          (BO2ZM_UBO ? BO2ZM_SHARED_UINT(296 + (LOC) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 296 + (LOC) * 4))

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    uint g_AlphaFunction : packoffset(c17.y); \
    float2 g_NdcScale : packoffset(c17.z); \
    float2 g_NdcOffset : packoffset(c18.x);

uint g_SpecConstants();

#define g_InputRemap(LOC) 0xFFF

#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

// BO2ZM_RECOMP: a fetch with unnormalized (texel) coordinates: to 0..1 with the host image's size (1/size from
// the shared constants when the pipeline provides it).
float2 normalize2D(uint resourceDescriptorIndex, float2 texCoord, float2 invSize)
{
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TEX_SIZE)
        return texCoord * invSize;
    return texCoord / getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]);
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    // With the bit set, the size comes from a constant and the texture does not have to be queried. It
    // is the same computation with the same number: the renderer writes 1/size of the host image. With a
    // ternary, DXC evaluates both branches and the size query stays: an if with [branch] is needed for
    // the dead branch to disappear when the pipeline is specialized.
    float2 texelOffset;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TEX_SIZE)
        texelOffset = offset * invSize;
    else
        texelOffset = offset / getTexture2DDimensions(texture);
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + texelOffset);
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

// BO2ZM_RECOMP: the translator passes 1/size to every 2D fetch, getWeights included
// (BO2 uses GetTextureWeights in its shadow-map filters). Same
// rule as tfetch2D: the size comes from the constant when the pipeline says so.
float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    float2 texSize;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TEX_SIZE)
        texSize = 1.0 / invSize;
    else
        texSize = getTexture2DDimensions(texture);
    return select(isnan(texCoord), 0.0, frac(texCoord * texSize + offset - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

// Shadow map sampling. The translator emits tfetch2D for everything; the library rewrite step
// changes the calls whose sampler is SHADOWMAP_SAMPLER to this function, since those are the only
// ones meant to be made cheaper. With the bit set, all the 3x3 samples land on the same texel and
// the compiler keeps only one.
float4 tfetch2DShadow(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_PCF_CHEAP)
        return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, float2(0.0, 0.0), invSize);
    return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
}

// The shadow map with the minimum of its pair (bo2zm_native_shadow_min). The library step changes
// all shadow map calls to this function and passes it the 3D index of that same register, which is
// where the app puts the pair: the world map (or the texture itself, which gives the same texel, while
// the app is checking). Both are sampled the same way (same point sampler, same coordinates and
// offsets), so the minimum is per texel. The first comparison is the library marker: it is never
// true and the driver removes it when specializing.
float4 tfetch2DShadowMin(uint resourceDescriptorIndex, uint pairDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() == BO2ZM_SHADOW_MIN_MARKER)
        return float4(0.0, 0.0, 0.0, 0.0);
    float4 minSample = tfetch2DShadow(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_SHADOW_MIN)
        minSample = min(minSample, tfetch2DShadow(pairDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize));
    return minSample;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

// BO2ZM_RECOMP: 1D fetches (BO2's ExemplarNodeData lookups) read row 0 of the slot's 2D texture, at level 0 so the
// same code is valid in vertex shaders. The renderer has no 1D textures yet and binds an empty one for them.
float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord)
{
    return g_Texture2DDescriptorHeap[resourceDescriptorIndex].SampleLevel(
        g_SamplerDescriptorHeap[samplerDescriptorIndex], float2(texCoord, 0.5), 0.0);
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

// BO2ZM_RECOMP: fetches without computed LOD (use_computed_lod = 0) sample at an explicit level: the LOD bias, plus
// the SetTextureLod register with use_register_lod. The translator emits the *Level variants for them.
float4 tfetch2DLevel(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize, float lod)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    float2 texelOffset;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TEX_SIZE)
        texelOffset = offset * invSize;
    else
        texelOffset = offset / getTexture2DDimensions(texture);
    return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + texelOffset, lod);
}

float4 tfetch3DLevel(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float lod)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord, lod);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

// BO2ZM_RECOMP: see tfetch2DLevel.
float4 tfetchCubeLevel(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData, float lod)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z], lod);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}

// BO2ZM: 3 bits per component: 0-3 = data component, 4 = 0, 5 = 1, 7 = unchanged.
float4 remapInput(float4 value, uint code)
{
    if (code == 0xFFF)
        return value;

    float4 result = value;
    for (uint i = 0; i < 4; i++)
    {
        uint source = (code >> (i * 3)) & 7;
        if (source < 4)
            result[i] = value[source];
        else if (source == 4)
            result[i] = 0.0;
        else if (source == 5)
            result[i] = 1.0;
    }
    return result;
}

// BO2ZM: Xenos alpha test with its comparison function. Positive if the pixel passes.
float alphaTestValue(float alpha)
{
    bool pass = true;
    switch ((g_SpecConstants() & SPEC_CONSTANT_ALPHA_FUNC_MASK) >> SPEC_CONSTANT_ALPHA_FUNC_SHIFT)
    {
    case 0: pass = false; break;
    case 1: pass = alpha < g_AlphaThreshold; break;
    case 2: pass = alpha == g_AlphaThreshold; break;
    case 3: pass = alpha <= g_AlphaThreshold; break;
    case 4: pass = alpha > g_AlphaThreshold; break;
    case 5: pass = alpha != g_AlphaThreshold; break;
    case 6: pass = alpha >= g_AlphaThreshold; break;
    default: break;
    }
    return pass ? 1.0 : -1.0;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
