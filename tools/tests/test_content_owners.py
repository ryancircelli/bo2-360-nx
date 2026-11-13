"""Run production content-manager filesystem operations with a small VFS boundary."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    "memory.h": r"""#pragma once
#include <rex/types.h>
#include <string>
namespace rex::memory {
template<class T> void copy_and_swap(T* out, const T* in, size_t count) {
  for (size_t i = 0; i < count; ++i) out[i] = rex::byte_swap(in[i]);
}
template<class T> T load_and_swap(const void* data) {
  T result;
  const auto* units = static_cast<const typename T::value_type*>(data);
  while (*units) result.push_back(rex::byte_swap(*units++));
  return result;
}
}
""",
    "logging.h": "#pragma once\n#define REXSYS_INFO(...) ((void)0)\n#define REXSYS_WARN(...) ((void)0)\n#define REXSYS_ERROR(...) ((void)0)\n",
    "cvar.h": "#pragma once\n#include <string>\n#define REXCVAR_DEFINE_STRING(name, value, ...) std::string name = value\n#define REXCVAR_GET(name) name\n#define REXCVAR_ALIAS(alias, name) static_assert(true)\n",
    "filesystem/devices/host_path_device.h": r"""#pragma once
#include <filesystem>
#include <string>
namespace rex::filesystem {
class HostPathDevice {
 public:
  HostPathDevice(const std::string&, const std::filesystem::path&, bool, bool) {}
  bool Initialize() { return true; }
};
}
""",
    "filesystem/devices/stfs_container_device.h": "#pragma once\n",
    "system/xfile.h": "#pragma once\n#include <rex/system/kernel_state.h>\n",
    "system/xobject.h": "#pragma once\n#include <rex/system/kernel_state.h>\n",
    "system/kernel_state.h": r"""#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <rex/filesystem/devices/host_path_device.h>
namespace rex::system {
struct XObject { enum class Type { File }; };
struct EntryBoundary { std::string absolute_path() { return {}; } };
struct XFile {
  EntryBoundary* entry() const { return nullptr; }
  void ReleaseHandle() {}
};
template<class T> using object_ref = std::shared_ptr<T>;
struct ObjectsBoundary {
  template<class T> std::vector<object_ref<T>> GetObjectsByType(XObject::Type) { return {}; }
};
struct FilesystemBoundary {
  void RegisterDevice(std::unique_ptr<rex::filesystem::HostPathDevice>) {}
  void UnregisterDevice(const std::string&) {}
  void RegisterSymbolicLink(const std::string& name, const std::string& device) { links[name] = device; }
  void UnregisterSymbolicLink(const std::string& name) { links.erase(name); }
  void FindSymbolicLink(const std::string& name, std::string& result) { result = links.at(name); }
  std::unordered_map<std::string, std::string> links;
};
struct ProfileBoundary { uint64_t xuid() const { return 0xB13EBABEBABEBABE; } };
struct KernelState {
  uint32_t title_id() const { return 0x415608C3; }
  FilesystemBoundary* file_system() { return &fs; }
  ObjectsBoundary* object_table() { return &objects; }
  ProfileBoundary* user_profile() { return &primary; }
  FilesystemBoundary fs;
  ObjectsBoundary objects;
  ProfileBoundary primary;
};
}
""",
}


def main():
    compiler = os.environ.get("CXX") or shutil.which("g++") or "C:/msys64/ucrt64/bin/g++.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env["PATH"]
    with tempfile.TemporaryDirectory(prefix="bo2-content-owners-") as directory:
        temporary = Path(directory)
        includes = temporary / "include" / "rex"
        for name, text in STUBS.items():
            file = includes / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(text, encoding="utf-8")
        # Compile every production manager/package method involved in saving,
        # backing up, opening, deleting and unmounting, against the real header.
        # Only the unrelated STFS installer below this boundary is excluded.
        source = (ROOT / "sdk/src/system/xam/content_manager.cpp").read_text(encoding="utf-8")
        core = temporary / "content_manager.cpp"
        core.write_text(source[:source.index("static X_RESULT ExtractEntry(")] + "\n} } }\n", encoding="utf-8")
        executable = temporary / "content-owners.exe"
        subprocess.run([compiler, "-std=c++23", "-DFMT_HEADER_ONLY", "-pthread",
                        "-I" + str(includes.parent), "-I" + str(ROOT / "sdk/include"),
                        "-I" + str(ROOT / "sdk/thirdparty/fmt/include"),
                        str(ROOT / "tools/tests/content_owners_test.cpp"), str(core),
                        "-o", str(executable)], check=True, env=env, cwd=ROOT)
        subprocess.run([str(executable), str(temporary / "content")], check=True, env=env, cwd=ROOT)


if __name__ == "__main__":
    main()
