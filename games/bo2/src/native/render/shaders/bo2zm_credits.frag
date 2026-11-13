#version 450
// Native renderer: the home screen's credits line (bo2zm_credits.cpp), drawn over the output pass's image with
// alpha blending, one texel per pixel. The output pass's vertex shader places the quad (its rectangle push constant,
// in NDC); this shader reads the texel under each pixel.
//
// Push constants (from byte 16, in the output pass's fragment range): origin = the image's top-left pixel on the
// output, source = its top-left texel in the credits picture (the 720-line and 1080-line images, one above the
// other), alpha = the fade (0 to 1).
//
// The header is generated with:
//   glslangValidator -V --target-env vulkan1.0 --vn bo2zm_credits_ps -o bo2zm_credits_ps.h bo2zm_credits.frag

layout(push_constant) uniform Constants {
  layout(offset = 16) ivec2 origin;
  ivec2 source;
  float alpha;
} constants;

layout(set = 0, binding = 0) uniform texture2D picture;
layout(set = 0, binding = 1) uniform sampler tex_sampler;

layout(location = 0) out vec4 color;

void main() {
  ivec2 texel = constants.source + ivec2(gl_FragCoord.xy) - constants.origin;
  vec4 t = texelFetch(sampler2D(picture, tex_sampler), texel, 0);
  color = vec4(t.rgb, t.a * constants.alpha);
}
