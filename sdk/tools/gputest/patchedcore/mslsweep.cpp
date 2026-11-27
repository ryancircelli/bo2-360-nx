/**
 * rexgpu-patched-mslsweep: the shader sweep for the Metal port's translation path. Takes the same
 * microcode dumps as rexgpu-shadersweep (refxenos --dump-shaders: vs_<hash>.bin / ps_<hash>.bin) and
 * runs each through the patched DXBC translator (bindings) and the Metal MSL translator, exactly as
 * the plugin does, without Metal. Prints one line per file; the "OK" lines carry the MSL size and a
 * hash so a run can be diffed against another. Exit 1 if any shader fails, 2 on bad usage.
 */
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "rex/graphics/metal/metal_msl_translator.h"
#include "rex/graphics/pipeline/shader/dxbc.h"
#include "rex/graphics/pipeline/shader/dxbc_translator.h"
#include "rex/string/buffer.h"

using namespace rex::graphics;

int main(int argc, char** argv) {
  if (argc < 2) {
    fmt::print(stderr, "usage: rexgpu-patched-mslsweep FILE...\n");
    return 2;
  }
  int bad = 0;
  for (int i = 1; i < argc; ++i) {
    std::string path = argv[i];
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    if (bytes.empty() || bytes.size() % 4) {
      fmt::print("FAIL {} unreadable or not dword sized\n", path);
      ++bad;
      continue;
    }
    std::vector<uint32_t> dwords(bytes.size() / 4);
    std::memcpy(dwords.data(), bytes.data(), bytes.size());
    std::string base = path.substr(path.find_last_of('/') + 1);
    bool is_vs = base.rfind("ps_", 0) != 0;
    DxbcShader shader(is_vs ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel, 0, dwords.data(),
                      dwords.size(), std::endian::little);
    rex::string::StringBuffer disasm;
    shader.AnalyzeUcode(disasm);
    DxbcShaderTranslator dxbc(rex::ui::GraphicsProvider::GpuVendorID::kApple, true, false, true, true, 1, 1,
                              false);
    uint32_t dyn_regs = shader.GetDynamicAddressableRegisterCount(63);
    DxbcShaderTranslator::Modification mod(is_vs ? dxbc.GetDefaultVertexShaderModification(dyn_regs)
                                                 : dxbc.GetDefaultPixelShaderModification(dyn_regs));
    if (is_vs) {
      mod.vertex.interpolator_mask = shader.writes_interpolators();
    } else {
      uint32_t n = std::min<uint32_t>(std::max<uint32_t>(1, shader.register_static_address_bound()),
                                      xenos::kMaxInterpolators);
      mod.pixel.interpolator_mask = (1u << n) - 1;
      mod.pixel.interpolators_centroid = 0;
    }
    Shader::Translation* t = shader.GetOrCreateTranslation(mod.value);
    if (!dxbc.TranslateAnalyzedShader(*t)) {
      fmt::print("FAIL {} dxbc translation failed\n", base);
      ++bad;
      continue;
    }
    metal::MslTranslatorConfig config;
    config.gamma_render_target_as_unorm8 = true;
    metal::MslShaderTranslator msl(config);
    auto result = msl.Translate(shader, mod.value);
    if (!result.ok) {
      fmt::print("FAIL {} fallback: {}\n", base, result.reason);
      ++bad;
      continue;
    }
    fmt::print("OK   {} {} dwords, {} msl bytes, fnv {:016x}\n", base, dwords.size(), result.source.size(),
               [&] {
                 uint64_t h = 1469598103934665603ull;
                 for (unsigned char c : result.source) h = (h ^ c) * 1099511628211ull;
                 return h;
               }());
  }
  fmt::print("{} shaders, {} failed\n", argc - 1, bad);
  return bad ? 1 : 0;
}
