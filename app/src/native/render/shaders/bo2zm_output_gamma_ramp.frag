#version 450
// Native renderer: output pass with the game's gamma ramp and the optional postprocessing.
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// The Xbox 360 does not output the frontbuffer as is: the display passes each 8-bit channel through the
// 256-entry, 10-bit table the game loads (DC_LUT_30_COLOR, register_table.inc). A game can load a table
// that is not the identity, and without it its midtones and shadows come out darker.
//
// Specialization constants:
//   - 0, kExact (the normal case: output the same size as the source): reads the pixel's texel with
//     texelFetch, unfiltered. It is cheaper than bilinear sampling and pays for the three table reads.
//     Without kExact, it samples like the SDK's guest_output_bilinear_ps, for outputs of another size.
//   - 1, kGrading (postprocessing with saturation, vibrance, vignette or scanlines): the table carries
//     the game's ramp with temperature, brightness, contrast and tint, and saturation, vibrance, gamma,
//     vignette and scanlines are done here, in the same order as the public-domain grading shader of
//     another static recompilation project. Without kGrading, everything single-channel is already in
//     the table (bo2zm_native_targets.cpp, RecomputeRamp) and costs nothing.
//   - 2, kFxaa (bo2zm_antialiasing = fxaa): smooths the edges of the game image before the table, with
//     the SDK's FXAA at normal quality (GetSwapFxaaComputeSource in vulkan/command_processor.cpp): five
//     luma reads and, only on edges, four color reads. It partly replaces the Xbox 360's 4x MSAA, which
//     the Switch lacks; it costs some GPU time.
// The push constants are those of guest_output_bilinear_ps: the offset from byte 16 and the inverse
// size from byte 24. From byte 32, the FPS counter (bo2zm_show_fps): x = frames per second
// times 10, y = screen pixels per font dot, z bit 0 = draw it. It goes in the top-left corner, over the image.
// z bit 1 = draw the per-view third person crosshairs described in the ramp UBO.
// w = screen pixels per font dot of the preset note (bo2zm_preset), 0 = no note; its text is at byte 48, up to
// 20 characters of 6 bits, 5 per word from the low bits (codes in kNoteFont). It goes in the top-right corner.
// banner (bytes 64-95) and banner_info (byte 96): a message of up to 40 characters, the same codes, centred at the top
// of the image (the four cores' failsafe, bo2zm_preset.cpp ShowBanner); banner_info.x = screen pixels per font dot,
// 0 = no banner.
//
// The header is generated with:
//   glslangValidator -V --target-env vulkan1.0 --vn bo2zm_output_gamma_ramp_ps -o bo2zm_output_gamma_ramp_ps.h
//     bo2zm_output_gamma_ramp.frag
// and its MSL (bo2zm_output_gamma_ramp_msl.h, the Metal presenter's) with:
//   glslangValidator -V --target-env vulkan1.0 -o ramp.spv bo2zm_output_gamma_ramp.frag
//   spirv-cross ramp.spv --msl --msl-version 20100 --msl-decoration-binding
//     --rename-entry-point main bo2zm_output_ramp frag --output ramp.metal
//   then ramp.metal inside the R"msl( ... )msl" string of the header.

layout(constant_id = 0) const bool kExact = false;
layout(constant_id = 1) const bool kGrading = false;
layout(constant_id = 2) const bool kFxaa = false;

layout(push_constant) uniform Constants {
  layout(offset = 16) ivec2 displacement;
  layout(offset = 24) vec2 inverse;
  layout(offset = 32) uvec4 counter;
  layout(offset = 48) uvec4 note;
  layout(offset = 64) uvec4 banner[2];
  layout(offset = 96) uvec4 banner_info;
} constants;

layout(set = 0, binding = 0) uniform texture2D source;
layout(set = 0, binding = 1) uniform sampler tex_sampler;
layout(set = 0, binding = 2, std140) uniform Ramp {
  // Red, green and blue of each entry (10-bit value / 1023, with the single-channel postprocessing).
  vec4 inputs[256];
  vec4 blend;   // saturation, vibrance, 1 / gamma (kGrading)
  vec4 effects;  // vignette, lines (kGrading)
  vec4 crosshair_rects[4];  // normalized x, y, width, height in the complete image
  uvec4 crosshair_styles[4];  // enabled, type, color, reserved
} ramp;

layout(location = 0) out vec4 color;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

float Luma(vec2 uv) {
  return dot(textureLod(sampler2D(source, tex_sampler), uv, 0.0).rgb, kLumaWeights);
}

// FXAA at normal quality, like the SDK's (edge threshold 0.166, minimum 0.0833 and an 8-texel reach).
vec3 Fxaa(vec2 uv) {
  vec2 step = constants.inverse;
  vec3 rgb_center = textureLod(sampler2D(source, tex_sampler), uv, 0.0).rgb;
  float luma_center = dot(rgb_center, kLumaWeights);
  float luma_nw = Luma(uv + vec2(-step.x, -step.y));
  float luma_ne = Luma(uv + vec2(step.x, -step.y));
  float luma_so = Luma(uv + vec2(-step.x, step.y));
  float luma_ = Luma(uv + vec2(step.x, step.y));
  float luma_min = min(luma_center, min(min(luma_nw, luma_ne), min(luma_so, luma_)));
  float luma_max = max(luma_center, max(max(luma_nw, luma_ne), max(luma_so, luma_)));
  if (luma_max - luma_min < max(0.0833, luma_max * 0.166)) {
    return rgb_center;
  }
  vec2 address = vec2(-((luma_nw + luma_ne) - (luma_so + luma_)), (luma_nw + luma_so) - (luma_ne + luma_));
  float reduction = max((luma_nw + luma_ne + luma_so + luma_) * (0.25 * 0.125), 1.0 / 128.0);
  float inverse_min = 1.0 / (min(abs(address.x), abs(address.y)) + reduction);
  address = clamp(address * inverse_min, vec2(-8.0), vec2(8.0)) * step;
  vec3 rgb_a = 0.5 * (textureLod(sampler2D(source, tex_sampler), uv + address * (1.0 / 3.0 - 0.5), 0.0).rgb +
                      textureLod(sampler2D(source, tex_sampler), uv + address * (2.0 / 3.0 - 0.5), 0.0).rgb);
  vec3 rgb_b = rgb_a * 0.5 + 0.25 * (textureLod(sampler2D(source, tex_sampler), uv + address * -0.5, 0.0).rgb +
                                     textureLod(sampler2D(source, tex_sampler), uv + address * 0.5, 0.0).rgb);
  float luma_b = dot(rgb_b, kLumaWeights);
  return (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
}

// The FPS counter's 3x5 font: 0-9 and '.', one bit per dot, row by row from the top left (bit 14).
const uint kFont[11] = uint[11](0x7B6Fu, 0x2C97u, 0x73E7u, 0x73CFu, 0x5BC9u, 0x79CFu, 0x79EFu, 0x7249u, 0x7BEFu,
                                  0x7BCFu, 0x0002u);

// "9.7", "29.9" or "123.4" on a dark box 8 pixels from the top-left corner.
vec3 Counter(vec3 c) {
  int e = int(max(constants.counter.y, 1u));
  ivec2 q = ivec2(gl_FragCoord.xy) - ivec2(8);
  uint v = min(constants.counter.x, 9999u);
  int n = v >= 1000u ? 5 : v >= 100u ? 4 : 3;
  if (q.x < 0 || q.y < 0 || q.x >= (n * 4 + 1) * e || q.y >= 7 * e) {
    return c;
  }
  ivec2 point = q / e - ivec2(1);  // font dots, after a one-dot margin
  if (point.x >= 0 && point.y >= 0 && point.y < 5 && (point.x % 4) < 3 && point.x / 4 < n) {
    int r = n - 1 - point.x / 4;  // characters from the right: tenths, '.', units, tens, hundreds
    uint g = r == 0 ? v % 10u : r == 1 ? 10u : (v / (r == 2 ? 10u : r == 3 ? 100u : 1000u)) % 10u;
    if (((kFont[g] >> uint(14 - (point.y * 3 + point.x % 4))) & 1u) != 0u) {
      return vec3(1.0, 1.0, 0.4);
    }
  }
  return c * 0.25;
}

// The third person crosshair in the middle of the image, outlined in black so it shows on bright and dark
// scenes alike: a small round dot (smoothed edges), or a small cross of four one-pixel arms around an open middle
// pixel. e = screen pixels per font dot (1/240 of the height).
const vec3 kCrosshairColors[7] = vec3[7](vec3(1.0), vec3(1.0, 0.15, 0.15), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.9, 1.0),
                                         vec3(1.0, 0.95, 0.2), vec3(1.0, 0.3, 1.0), vec3(1.0, 0.6, 0.1));

vec3 Crosshair(vec3 c, int index) {
  vec4 rect = ramp.crosshair_rects[index];
  uvec4 style = ramp.crosshair_styles[index];
  if (style.x == 0u) return c;
  ivec2 size = ivec2(round(1.0 / constants.inverse));
  vec2 first = vec2(constants.displacement) + rect.xy * vec2(size);
  vec2 extent = rect.zw * vec2(size);
  if (any(lessThan(gl_FragCoord.xy, first)) || any(greaterThanEqual(gl_FragCoord.xy, first + extent))) return c;
  int e = max(2, int(extent.y / 240.0));
  ivec2 middle = ivec2(round(first + extent * 0.5));
  vec3 fill = kCrosshairColors[min(style.z, 6u)];
  if (style.y == 1u) {
    ivec2 d = abs(ivec2(gl_FragCoord.xy) - middle);
    int arm = int(max(float(e), 2.0) * 0.9);
    int along = max(d.x, d.y), across = min(d.x, d.y);
    if (along == 0 || along > arm + 1 || across > 1) {
      return c;
    }
    return (along <= arm && across == 0) ? fill : vec3(0.0);
  }
  float radius = max(float(e) * 0.25, 0.75);
  float dist = length(gl_FragCoord.xy - (vec2(middle) + 0.5));
  vec3 color = mix(c, vec3(0.0), clamp(radius + 1.5 - dist, 0.0, 1.0) * 0.85);
  return mix(color, fill, clamp(radius + 0.5 - dist, 0.0, 1.0));
}

// The preset note's font, by code: 0 end, 1 space, 2-11 digits, 12-37 A-Z, then ':', '.', '-', '%',
// '/'. Same 3x5 layout as kFont.
const uint kNoteFont[43] = uint[43](
    0x0000u, 0x0000u, 0x7B6Fu, 0x2C97u, 0x73E7u, 0x73CFu, 0x5BC9u, 0x79CFu, 0x79EFu, 0x7249u, 0x7BEFu, 0x7BCFu,
    0x2BEDu, 0x6BAEu, 0x3923u, 0x6B6Eu, 0x79A7u, 0x79A4u, 0x396Bu, 0x5BEDu, 0x7497u, 0x126Au, 0x5BADu, 0x4927u,
    0x5FEDu, 0x6B6Du, 0x2B6Au, 0x6BA4u, 0x2B73u, 0x6BADu, 0x388Eu, 0x7492u, 0x5B6Fu, 0x5B6Au, 0x5BFDu, 0x5AADu,
    0x5A92u, 0x72A7u, 0x0410u, 0x0002u, 0x01C0u, 0x52A5u, 0x12A4u);

uint NoteCode(int i) {
  return (constants.note[i / 5] >> uint(6 * (i % 5))) & 63u;
}

// "PRESET: LOW" and the like on a dark box, 8 pixels from the top-right corner of the image (the top left is where
// the console overlays draw). The image's right edge: the offset plus its width (inverse is 1 / width).
vec3 ScreenNote(vec3 c) {
  int e = int(constants.counter.w);
  int y = int(gl_FragCoord.y) - 8;
  if (y < 0 || y >= 7 * e) {
    return c;
  }
  int n = 0;
  while (n < 20 && NoteCode(n) != 0u) {
    ++n;
  }
  int width = (n * 4 + 1) * e;
  int right = constants.displacement.x + int(round(1.0 / constants.inverse.x));
  ivec2 q = ivec2(int(gl_FragCoord.x) - (right - 8 - width), y);
  if (q.x < 0 || q.x >= width) {
    return c;
  }
  ivec2 point = q / e - ivec2(1);  // font dots, after a one-dot margin
  if (point.x >= 0 && point.y >= 0 && point.y < 5 && (point.x % 4) < 3 && point.x / 4 < n) {
    uint g = kNoteFont[min(NoteCode(point.x / 4), 42u)];
    if (((g >> uint(14 - (point.y * 3 + point.x % 4))) & 1u) != 0u) {
      return vec3(1.0, 1.0, 0.4);
    }
  }
  return c * 0.25;
}

uint BannerCode(int i) {
  return (constants.banner[i / 20][(i % 20) / 5] >> uint(6 * (i % 5))) & 63u;
}

// A message on a dark box, centred 8 pixels below the top of the image, in a warning colour.
vec3 ScreenBanner(vec3 c) {
  int e = int(constants.banner_info.x);
  int y = int(gl_FragCoord.y) - 8;
  if (y < 0 || y >= 7 * e) {
    return c;
  }
  int n = 0;
  while (n < 40 && BannerCode(n) != 0u) {
    ++n;
  }
  int width = (n * 4 + 1) * e;
  int middle = constants.displacement.x + int(round(0.5 / constants.inverse.x));
  ivec2 q = ivec2(int(gl_FragCoord.x) - (middle - width / 2), y);
  if (q.x < 0 || q.x >= width) {
    return c;
  }
  ivec2 point = q / e - ivec2(1);  // font dots, after a one-dot margin
  if (point.x >= 0 && point.y >= 0 && point.y < 5 && (point.x % 4) < 3 && point.x / 4 < n) {
    uint g = kNoteFont[min(BannerCode(point.x / 4), 42u)];
    if (((g >> uint(14 - (point.y * 3 + point.x % 4))) & 1u) != 0u) {
      return vec3(1.0, 0.55, 0.35);
    }
  }
  return c * 0.25;
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy) - constants.displacement;
  vec3 texel;
  if (kFxaa) {
    texel = Fxaa((vec2(uvec2(pixel)) + 0.5) * constants.inverse);
  } else if (kExact) {
    texel = texelFetch(sampler2D(source, tex_sampler), pixel, 0).rgb;
  } else {
    texel = textureLod(sampler2D(source, tex_sampler), (vec2(uvec2(pixel)) + 0.5) * constants.inverse, 0.0).rgb;
  }
  uvec3 i = uvec3(texel * 255.0 + 0.5);
  vec3 c = vec3(ramp.inputs[i.r].r, ramp.inputs[i.g].g, ramp.inputs[i.b].b);
  if (kGrading) {
    // Saturation around luma, and vibrance (boosts the least saturated pixels more).
    float luma = dot(max(c, 0.0), vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, ramp.blend.x);
    float saturated = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    c = mix(vec3(luma), c, 1.0 + ramp.blend.y * (1.0 - clamp(saturated, 0.0, 1.0)));
    c = pow(max(c, vec3(0.0001)), vec3(ramp.blend.z));
    // Vignette: four bands of 42 % from each edge, dark at the edge and transparent inward, which add up
    // in the corners.
    vec2 uv = (vec2(uvec2(pixel)) + 0.5) * constants.inverse;
    float v = ramp.effects.x;
    c *= (1.0 - v * max(0.0, 1.0 - uv.y / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.y) / 0.42)) *
         (1.0 - v * max(0.0, 1.0 - uv.x / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.x) / 0.42));
    // Scanlines: one dark row out of every three.
    if ((uint(pixel.y) % 3u) == 0u) {
      c *= 1.0 - ramp.effects.y * 0.7;
    }
    c = clamp(c, 0.0, 1.0);
  }
  if ((constants.counter.z & 1u) != 0u) {
    c = Counter(c);
  }
  if ((constants.counter.z & 2u) != 0u) {
    for (int view = 0; view < 4; ++view) c = Crosshair(c, view);
  }
  if (constants.counter.w != 0u) {
    c = ScreenNote(c);
  }
  if (constants.banner_info.x != 0u) {
    c = ScreenBanner(c);
  }
  color = vec4(c, 1.0);
}
