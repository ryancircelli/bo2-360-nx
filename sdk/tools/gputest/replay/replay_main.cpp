/**
 * rexgpu-replay: replays a captured .xtr GPU trace through a GPU plugin with
 * no game running, and writes per-draw render target readbacks plus a JSON
 * record per draw. See tools/gputest/replay/README.md.
 *
 * Usage:
 *   rexgpu-replay <trace.xtr> -o <out_dir> [--plugin <path or name>]
 *                 [--backend vulkan] [--label name] [--no-color] [--no-depth]
 *                 [--full] [--frames N] [--scratch 0xADDR]
 *                 [--draws FIRST:COUNT] [--log-level info]
 */

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/trace/replay_api.h>
#include <rex/logging/api.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

namespace {

void Usage() {
  std::fprintf(stderr,
               "usage: rexgpu-replay <trace.xtr> -o <out_dir> [--plugin path|name]\n"
               "       [--backend vulkan|any] [--label name] [--no-color] [--no-depth]\n"
               "       [--full] [--frames N] [--scratch 0xADDR] [--draws FIRST:COUNT]\n"
               "       [--texcheck] [--race] [--race-frames N]\n"
               "       [--log-level trace|debug|info|warn|error] [--cvar name=value]...\n");
}

std::filesystem::path ResolvePlugin(const std::string& spec) {
  std::filesystem::path p(spec);
  if (spec.find('/') != std::string::npos || p.has_extension()) {
    return p;
  }
#if defined(__APPLE__)
  std::string file = "librexgpu-" + spec + ".dylib";
#else
  std::string file = "librexgpu-" + spec + ".so";
#endif
  return rex::filesystem::GetExecutableFolder() / file;
}

}  // namespace

int main(int argc, char** argv) {
  std::string trace_path, out_dir = "replay_out", plugin = "xenos", backend = "vulkan", label;
  std::string log_level = "info";
  uint32_t race_frames = 0;
  uint32_t flags = kRexGpuTraceReplayReadbackColor | kRexGpuTraceReplayReadbackDepth;
  uint32_t frames = 0, scratch = 0, first_draw = 0, draw_count = 0;
  std::vector<std::pair<std::string, std::string>> cvars;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        Usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-o" || a == "--out") {
      out_dir = next();
    } else if (a == "--plugin") {
      plugin = next();
    } else if (a == "--backend") {
      backend = next();
    } else if (a == "--label") {
      label = next();
    } else if (a == "--no-color") {
      flags &= ~kRexGpuTraceReplayReadbackColor;
    } else if (a == "--no-depth") {
      flags &= ~kRexGpuTraceReplayReadbackDepth;
    } else if (a == "--full") {
      flags |= kRexGpuTraceReplayFullImages;
    } else if (a == "--texcheck") {
      flags |= kRexGpuTraceReplayTexCheck;
    } else if (a == "--race") {
      flags |= kRexGpuTraceReplayTexCheck | kRexGpuTraceReplayRace;
    } else if (a == "--race-frames") {
      race_frames = uint32_t(std::stoul(next()));
    } else if (a == "--frames") {
      frames = uint32_t(std::stoul(next()));
    } else if (a == "--scratch") {
      scratch = uint32_t(std::stoul(next(), nullptr, 0));
    } else if (a == "--draws") {
      std::string v = next();
      auto colon = v.find(':');
      first_draw = uint32_t(std::stoul(v.substr(0, colon)));
      draw_count = colon == std::string::npos ? 1 : uint32_t(std::stoul(v.substr(colon + 1)));
    } else if (a == "--log-level") {
      log_level = next();
    } else if (a == "--cvar") {
      std::string v = next();
      auto eq = v.find('=');
      if (eq == std::string::npos) {
        Usage();
        return 2;
      }
      cvars.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    } else if (a == "-h" || a == "--help") {
      Usage();
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      Usage();
      return 2;
    } else {
      trace_path = a;
    }
  }
  if (trace_path.empty()) {
    Usage();
    return 2;
  }
  if (label.empty()) {
    label = plugin + ":" + backend;
  }

  char arg0[] = "rexgpu-replay";
  char* fake_argv[] = {arg0, nullptr};
  rex::cvar::Init(1, fake_argv);
  rex::cvar::ApplyEnvironment();
  for (auto& [name, value] : cvars) {
    if (!rex::cvar::SetFlagByName(name, value)) {
      std::fprintf(stderr, "unknown cvar %s\n", name.c_str());
      return 2;
    }
  }
  rex::cvar::FinalizeInit();
  rex::InitLogging({}, spdlog::level::from_str(log_level));

  auto plugin_path = ResolvePlugin(plugin);
  void* lib = dlopen(plugin_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    std::fprintf(stderr, "cannot load plugin %s: %s\n", plugin_path.c_str(), dlerror());
    return 1;
  }
  auto abi_fn = reinterpret_cast<rex::system::GpuAbiVersionFn>(
      dlsym(lib, rex::system::kGpuAbiVersionSymbol));
  auto create_fn =
      reinterpret_cast<rex::system::GpuCreateFn>(dlsym(lib, rex::system::kGpuCreateSymbol));
  auto replay_fn =
      reinterpret_cast<PFN_rex_gpu_trace_replay>(dlsym(lib, "rex_gpu_trace_replay"));
  if (!abi_fn || !create_fn) {
    std::fprintf(stderr, "%s is not a rexglue GPU plugin\n", plugin_path.c_str());
    return 1;
  }
  if (!replay_fn) {
    std::fprintf(stderr,
                 "%s has no rex_gpu_trace_replay export; build it with "
                 "-DREXGLUE_BUILD_GPUTEST=ON\n",
                 plugin_path.c_str());
    return 1;
  }
  if (abi_fn() != rex::system::kGpuPluginAbiVersion) {
    std::fprintf(stderr, "plugin ABI %u, host expects %u\n", abi_fn(),
                 rex::system::kGpuPluginAbiVersion);
    return 1;
  }
  rex::system::GpuCreateInfo create_info{};
  create_info.struct_size = sizeof(create_info);
  create_info.backend = backend.c_str();
  rex::system::IGraphicsSystem* gs = create_fn(rex::system::kGpuPluginAbiVersion, &create_info);
  if (!gs) {
    std::fprintf(stderr, "plugin returned no graphics system for backend %s\n", backend.c_str());
    return 1;
  }

  auto runtime = std::make_unique<rex::Runtime>(std::filesystem::path{});
  rex::RuntimeConfig config;
  config.graphics.reset(gs);
  if (runtime->Setup(std::move(config)) != 0) {
    std::fprintf(stderr, "runtime setup failed\n");
    return 1;
  }

  RexGpuTraceReplayOptions options{};
  options.struct_size = sizeof(options);
  options.trace_path = trace_path.c_str();
  options.output_dir = out_dir.c_str();
  options.backend_label = label.c_str();
  options.flags = flags;
  options.max_frames = frames;
  options.scratch_address = scratch;
  options.first_draw = first_draw;
  options.draw_count = draw_count;
  options.race_frames = race_frames;
  int result = replay_fn(runtime->graphics_system(), &options);

  runtime->Shutdown();
  runtime.reset();
  // The plugin stays loaded until exit: static destructors in it may run
  // after Shutdown.
  std::fflush(stdout);
  std::_Exit(result);
}
