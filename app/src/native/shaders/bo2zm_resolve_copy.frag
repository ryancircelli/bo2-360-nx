// A resolve that cannot be a bit-for-bit copy (C2, CopyConverted in
// bo2zm_native_targets.cpp). Reads the render target texel, scales it by 2^exp_bias and packs it the
// way RB_COPY_DEST_INFO's number format says, into an image in the destination's own host format:
//   mode 0  unsigned repeating fraction (UNORM)   clamp to [0, 1]
//   mode 1  signed repeating fraction (SNORM)      clamp to [-1, 1]
//   mode 2  unsigned integer, into a UNORM16 image round, clamp to [0, 65535], / 65535
//   mode 3  signed integer, into an SNORM16 image  round, clamp to [-32767, 32767], / 32767
//   mode 4  float                                  as is
// BO2 resolves its luminance chain (16_16_16_16_FLOAT render targets) to 16_16_16_16 UINT with bias 11;
// the draws then sample it as UNORM16 and the CPU reads the integers back for the auto exposure.
#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform texture2D source;

layout(push_constant) uniform Constants {
  layout(offset = 16) ivec2 displacement;  // source texel = destination pixel + this
  float scale;                              // 2^exp_bias
  uint mode;
} c;

layout(location = 0) out vec4 color;

void main() {
  vec4 v = texelFetch(source, ivec2(gl_FragCoord.xy) + c.displacement, 0) * c.scale;
  if (c.mode == 0u) {
    color = clamp(v, 0.0, 1.0);
  } else if (c.mode == 1u) {
    color = clamp(v, -1.0, 1.0);
  } else if (c.mode == 2u) {
    color = clamp(round(v), 0.0, 65535.0) / 65535.0;
  } else if (c.mode == 3u) {
    color = clamp(round(v), -32767.0, 32767.0) / 32767.0;
  } else {
    color = v;
  }
}
//
// Its MSL for the Metal mirror (render/shaders/bo2zm_resolve_copy_msl.h) with the commands in that header.
