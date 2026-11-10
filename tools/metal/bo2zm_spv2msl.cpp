// bo2zm_spv2msl: the shader pack's SPIR-V to Metal Shading Language, ahead of time.
//
// Reads a bo2zm .spvpack (pack v1, app/src/native/render/bo2zm_shader_library.cpp) and writes one .metal file per
// shader and variant with SPIRV-Cross, under one fixed binding contract shared by every shader, so the Metal backend
// binds the same way for all of them (no per-shader reflection at draw time):
//
//   [[buffer(0..3)]]  sets 0-3, the bindless heaps (2D, 3D, cube, samplers): tier 2 argument buffers in the device
//                     address space (the arrays are unsized)
//   [[buffer(4..6)]]  set 4 bindings 0-2: g_UboVertex, g_UboPixel, g_UboShared (SPEC_CONSTANT_CONSTANTS_UBO path)
//   [[buffer(7)]]     push constants: three uint64 addresses (the default constants path)
//   [[buffer(8..)]]   free for the vertex buffers of the MTLVertexDescriptor
//   [[function_constant(0)]]  g_SpecConstants (Vulkan specialization constant 0)
//
// Variants: every shader as it is (vs_<hash>, ps_<hash>); for pixel shaders that do not write depth, a copy with
// [[early_fragment_tests]] (ps_<hash>_ez), the counterpart of the renderer's EarlyFragmentTests module copy.
// Vertex shaders get Y flipped back (dxc built them with -fvk-invert-y for Vulkan's Y-down clip space; Metal's is
// Y-up like Direct3D).
//
// Output: <out>/msl/<name>.metal, and <out>/spv2msl.tsv (name, stage, result, message). The output is derived from
// game data: keep it outside the repository.
//
// Usage: bo2zm_spv2msl <pack.spvpack> <out_dir> [--only <hash16>]...

#include <spirv_msl.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Entry {
  uint64_t hash = 0;
  bool vertex = false;
  std::vector<uint32_t> spirv;
};

bool ReadPack(const std::filesystem::path& path, std::vector<Entry>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  uint8_t header[24];
  if (!f.read(reinterpret_cast<char*>(header), sizeof(header))) return false;
  if (std::memcmp(header, "BZMSPV\0\0", 8) != 0) return false;
  uint32_t version, count;
  std::memcpy(&version, header + 8, 4);
  std::memcpy(&count, header + 12, 4);
  if (version != 1) return false;
  out.resize(count);
  for (Entry& e : out) {
    uint32_t sizes[2];
    if (!f.read(reinterpret_cast<char*>(sizes), 8) || !f.read(reinterpret_cast<char*>(&e.hash), 8)) return false;
    std::vector<uint8_t> container(sizes[0]);
    if (!f.read(reinterpret_cast<char*>(container.data()), sizes[0])) return false;
    e.vertex = sizes[0] >= 4 && (container[3] & 1) != 0;  // big-endian signature, bit 0 = vertex shader
    e.spirv.resize(sizes[1]);
    if (!f.read(reinterpret_cast<char*>(e.spirv.data()), std::streamsize(sizes[1]) * 4)) return false;
  }
  return true;
}

constexpr uint32_t kArgumentBufferSets = 4;    // sets 0-3, buffers 0-3
constexpr uint32_t kUboSet = 4;                // set 4, bindings 0-2 -> buffers 4-6
constexpr uint32_t kUboFirstBuffer = 4;
constexpr uint32_t kPushConstantBuffer = 7;

std::string Convert(const std::vector<uint32_t>& spirv, bool vertex, const std::string& name, bool early_z,
                    bool& writes_depth) {
  spirv_cross::CompilerMSL msl(spirv);
  const spv::ExecutionModel model = vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;

  spirv_cross::CompilerMSL::Options options;
  options.platform = spirv_cross::CompilerMSL::Options::macOS;
  options.set_msl_version(3, 1);
  options.argument_buffers = true;
  options.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
  msl.set_msl_options(options);

  spirv_cross::CompilerGLSL::Options common = msl.get_common_options();
  common.vertex.flip_vert_y = vertex;
  msl.set_common_options(common);

  for (uint32_t set = 0; set < kArgumentBufferSets; ++set) {
    msl.set_argument_buffer_device_address_space(set, true);
    spirv_cross::MSLResourceBinding b;
    b.stage = model;
    b.desc_set = set;
    b.binding = spirv_cross::kArgumentBufferBinding;
    b.msl_buffer = set;
    msl.add_msl_resource_binding(b);
  }
  msl.add_discrete_descriptor_set(kUboSet);
  for (uint32_t binding = 0; binding < 3; ++binding) {
    spirv_cross::MSLResourceBinding b;
    b.stage = model;
    b.desc_set = kUboSet;
    b.binding = binding;
    b.msl_buffer = kUboFirstBuffer + binding;
    msl.add_msl_resource_binding(b);
  }
  {
    spirv_cross::MSLResourceBinding b;
    b.stage = model;
    b.desc_set = spirv_cross::kPushConstDescSet;
    b.binding = spirv_cross::kPushConstBinding;
    b.msl_buffer = kPushConstantBuffer;
    msl.add_msl_resource_binding(b);
  }

  const auto entries = msl.get_entry_points_and_stages();
  if (entries.empty()) throw std::runtime_error("no entry point");
  msl.rename_entry_point(entries[0].name, name, entries[0].execution_model);
  writes_depth = msl.get_execution_mode_bitset().get(spv::ExecutionModeDepthReplacing);
  if (early_z) msl.set_execution_mode(spv::ExecutionModeEarlyFragmentTests);
  std::string source = msl.compile();

  // Every binding must have landed where the contract says (a shader that ignored a remap would bind wrong).
  for (const spirv_cross::Resource& r : msl.get_shader_resources().push_constant_buffers) {
    if (!msl.is_msl_resource_binding_used(model, spirv_cross::kPushConstDescSet, spirv_cross::kPushConstBinding))
      throw std::runtime_error("push constants not remapped: " + r.name);
  }
  return source;
}

std::string OneLine(std::string s) {
  for (char& c : s)
    if (c == '\n' || c == '\t') c = ' ';
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <pack.spvpack> <out_dir> [--only <hash16>]...\n", argv[0]);
    return 2;
  }
  std::set<uint64_t> only;
  for (int i = 3; i + 1 < argc; i += 2)
    if (std::strcmp(argv[i], "--only") == 0) only.insert(std::stoull(argv[i + 1], nullptr, 16));

  std::vector<Entry> entries;
  if (!ReadPack(argv[1], entries)) {
    std::fprintf(stderr, "%s: not a readable bo2zm shader pack v1\n", argv[1]);
    return 1;
  }
  const std::filesystem::path out = argv[2];
  std::filesystem::create_directories(out / "msl");

  std::mutex lock;
  std::vector<std::string> rows;
  std::atomic<size_t> next{0};
  const auto work = [&]() {
    for (size_t i = next++; i < entries.size(); i = next++) {
      const Entry& e = entries[i];
      if (!only.empty() && !only.count(e.hash)) continue;
      char base[32];
      std::snprintf(base, sizeof(base), "%s_%016llx", e.vertex ? "vs" : "ps",
                    static_cast<unsigned long long>(e.hash));
      const auto emit = [&](const std::string& name, bool early_z, bool& writes_depth) {
        std::string row;
        try {
          const std::string source = Convert(e.spirv, e.vertex, name, early_z, writes_depth);
          std::ofstream(out / "msl" / (name + ".metal")) << source;
          row = name + "\t" + (e.vertex ? "vs" : "ps") + "\tok\t";
        } catch (const std::exception& ex) {
          row = name + "\t" + (e.vertex ? "vs" : "ps") + "\tspirv-cross\t" + OneLine(ex.what());
        }
        std::lock_guard<std::mutex> g(lock);
        rows.push_back(std::move(row));
      };
      bool writes_depth = false;
      emit(base, false, writes_depth);
      if (!e.vertex && !writes_depth) {
        bool ignored = false;
        emit(std::string(base) + "_ez", true, ignored);
      }
    }
  };
  std::vector<std::thread> threads;
  const unsigned n = std::max(1u, std::thread::hardware_concurrency());
  for (unsigned t = 0; t < n; ++t) threads.emplace_back(work);
  for (std::thread& t : threads) t.join();

  std::sort(rows.begin(), rows.end());
  std::ofstream tsv(out / "spv2msl.tsv");
  size_t ok = 0;
  for (const std::string& r : rows) {
    tsv << r << "\n";
    ok += r.find("\tok\t") != std::string::npos;
  }
  std::printf("%zu of %zu converted to MSL (%zu shaders in the pack)\n", ok, rows.size(), entries.size());
  return ok == rows.size() ? 0 : 1;
}
