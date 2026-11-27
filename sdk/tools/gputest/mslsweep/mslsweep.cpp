/**
 * rexgpu-mslsweep: translate every given Xenos microcode file (refxenos --dump-shaders output, host-endian,
 * named vs_<hash>.bin / ps_<hash>.bin) with the Metal plugin's native Xenos -> MSL translator, the only shader
 * route of the no-converter Metal build: a shader it refuses is a dropped draw there. Prints one line per file
 * and a count per refusal reason; exits 1 if any shader fails. No GPU or Metal toolchain needed (the MSL is not
 * compiled here: --out DIR writes it for `xcrun metal -c` on a Mac).
 *   --domain quad|tri   translate vertex shaders as patch-indexed domain shaders (tessellated draws)
 *   --out DIR           write <name>.metal for every translated shader
 */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "rex/graphics/metal/metal_msl_translator.h"
#include "rex/graphics/pipeline/shader/dxbc.h"
#include "rex/graphics/pipeline/shader/dxbc_translator.h"
#include "rex/string/buffer.h"

using namespace rex;
using namespace rex::graphics;

int main(int argc, char** argv) {
  Shader::HostVertexShaderType host_vs = Shader::HostVertexShaderType::kVertex;
  std::string out_dir;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--domain" && i + 1 < argc) {
      std::string d = argv[++i];
      host_vs = d == "tri" ? Shader::HostVertexShaderType::kTriangleDomainPatchIndexed
                           : Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
    } else if (a == "--out" && i + 1 < argc) {
      out_dir = argv[++i];
      std::filesystem::create_directories(out_dir);
    } else {
      files.push_back(a);
    }
  }
  if (files.empty()) {
    fmt::print(stderr, "usage: rexgpu-mslsweep [--domain quad|tri] [--out DIR] FILE...\n");
    return 2;
  }
  int bad = 0, skipped = 0;
  std::map<std::string, int> reasons;
  for (const std::string& path : files) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    if (bytes.empty() || bytes.size() % 4) {
      fmt::print("FAIL {} unreadable or not dword sized\n", path);
      ++bad;
      continue;
    }
    std::vector<uint32_t> dwords(bytes.size() / 4);
    std::memcpy(dwords.data(), bytes.data(), bytes.size());
    std::string base = path.substr(path.find_last_of('/') + 1);
    // A trace without the shader's memory dumps zeros, which translate to an empty shader: not a pass.
    if (std::all_of(dwords.begin(), dwords.end(), [](uint32_t d) { return d == 0; })) {
      fmt::print("SKIP {} {} dwords: all zero (shader memory missing from the trace)\n", base, dwords.size());
      ++skipped;
      continue;
    }
    bool is_vs = base.rfind("ps_", 0) != 0;
    DxbcShader shader(is_vs ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel, 0, dwords.data(),
                      dwords.size(), std::endian::little);
    string::StringBuffer disasm;
    shader.AnalyzeUcode(disasm);
    // As msl_xlate_tool.cc: Apple vendor, ROV off, gamma as unorm8, scale 1.
    DxbcShaderTranslator dxbc(ui::GraphicsProvider::GpuVendorID::kApple, true, false, true, true, 1, 1, false);
    uint32_t dyn_regs = shader.GetDynamicAddressableRegisterCount(63);
    DxbcShaderTranslator::Modification mod(is_vs ? dxbc.GetDefaultVertexShaderModification(dyn_regs, host_vs)
                                                 : dxbc.GetDefaultPixelShaderModification(dyn_regs));
    if (is_vs) {
      mod.vertex.interpolator_mask = shader.writes_interpolators();
    } else {
      // Xenos copies interpolants into the first registers, bounded by the register count.
      uint32_t n = std::min<uint32_t>(std::max<uint32_t>(1, shader.register_static_address_bound()),
                                      xenos::kMaxInterpolators);
      mod.pixel.interpolator_mask = (1u << n) - 1;
    }
    Shader::Translation* translation = shader.GetOrCreateTranslation(mod.value);
    std::string reason;
    size_t msl_bytes = 0;
    if (!dxbc.TranslateAnalyzedShader(*translation) || !translation->is_valid()) {
      reason = "DXBC translation failed";
    } else {
      metal::MslShaderTranslator msl{metal::MslTranslatorConfig{}};
      metal::MslTranslationResult r = msl.Translate(shader, mod.value);
      if (!r.ok) {
        reason = r.reason;
      } else {
        msl_bytes = r.source.size();
        if (!out_dir.empty()) {
          std::ofstream(out_dir + "/" + base.substr(0, base.find_last_of('.')) + ".metal") << r.source;
        }
      }
    }
    if (reason.empty()) {
      fmt::print("OK   {} {} dwords, {} msl bytes\n", base, dwords.size(), msl_bytes);
    } else {
      fmt::print("FAIL {} {} dwords: {}\n", base, dwords.size(), reason);
      ++reasons[reason];
      ++bad;
    }
  }
  for (const auto& [reason, n] : reasons) {
    fmt::print("  {:5} {}\n", n, reason);
  }
  fmt::print("{} shaders, {} failed, {} skipped (all zero)\n", files.size(), bad, skipped);
  return bad ? 1 : 0;
}

// Referenced by the patched SDK sources this tool compiles.
extern "C" void fable2_ring_writeback_stored() {}
// The plugin's patch moves this definition to the host SDK, which this tool does not link.
REXCVAR_DEFINE_BOOL(draw_resolution_scaled_texture_offsets, true, "GPU/Shader",
                    "Scale texture offsets with draw resolution");
