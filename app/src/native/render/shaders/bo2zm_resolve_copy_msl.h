// Generated from the SDK's guest_output_triangle_strip_rect_vs (sdk/src/ui/shaders/vulkan_spirv) and
// bo2zm_resolve_copy.frag (see the commands there): the Metal mirror's own pass for the converted copy of the
// resolves. Entry points bo2zm_rect_vertex (the rectangle at buffer 0: float2 origin, float2 size, push constants
// bytes 0 to 15; Y flipped for Metal's NDC) and bo2zm_resolve_copy (constants at buffer 0, 16 bytes of padding then
// the push constants from byte 16; the source at texture 0, read with no sampler).
// The vertex shader was made from the SPIR-V words of the SDK header, written to vs.spv, with:
//   spirv-cross vs.spv --msl --msl-version 20100 --msl-decoration-binding --flip-vert-y
//     --rename-entry-point main bo2zm_rect_vertex vert --output vs.metal
// and the fragment shader with:
//   glslangValidator -V --target-env vulkan1.0 -o fs.spv bo2zm_resolve_copy.frag
//   spirv-cross fs.spv --msl --msl-version 20100 --msl-decoration-binding
//     --rename-entry-point main bo2zm_resolve_copy frag --output fs.metal
// then vs.metal and fs.metal inside the R"msl( ... )msl" string of the header (the fragment one without its
// repeated includes).
#pragma once
static const char kMslBo2zmResolveCopy[] = R"msl(
#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct _1030
{
    float2 _m0;
    float2 _m1;
};

struct bo2zm_rect_vertex_out
{
    float4 gl_Position [[position]];
};

vertex bo2zm_rect_vertex_out bo2zm_rect_vertex(constant _1030& _3305 [[buffer(0)]], uint gl_VertexIndex [[vertex_id]])
{
    bo2zm_rect_vertex_out out = {};
    out.gl_Position = float4(_3305._m0 + (float2((uint2(uint(int(gl_VertexIndex))) >> uint2(0u, 1u)) & uint2(1u)) * _3305._m1), 0.0, 1.0);
    out.gl_Position.y = -(out.gl_Position.y);    // Invert Y-axis for Metal
    return out;
}



struct Constants
{
    char _m0_pad[16];
    int2 displacement;
    float scale;
    uint mode;
};

struct bo2zm_resolve_copy_out
{
    float4 color [[color(0)]];
};

fragment bo2zm_resolve_copy_out bo2zm_resolve_copy(constant Constants& c [[buffer(0)]], texture2d<float> source [[texture(0)]], float4 gl_FragCoord [[position]])
{
    bo2zm_resolve_copy_out out = {};
    float4 v = source.read(uint2((int2(gl_FragCoord.xy) + c.displacement)), 0) * c.scale;
    if (c.mode == 0u)
    {
        out.color = fast::clamp(v, float4(0.0), float4(1.0));
    }
    else
    {
        if (c.mode == 1u)
        {
            out.color = fast::clamp(v, float4(-1.0), float4(1.0));
        }
        else
        {
            if (c.mode == 2u)
            {
                out.color = fast::clamp(round(v), float4(0.0), float4(65535.0)) / float4(65535.0);
            }
            else
            {
                if (c.mode == 3u)
                {
                    out.color = fast::clamp(round(v), float4(-32767.0), float4(32767.0)) / float4(32767.0);
                }
                else
                {
                    out.color = v;
                }
            }
        }
    }
    return out;
}

)msl";
