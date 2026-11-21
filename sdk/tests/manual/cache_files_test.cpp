// Runtime cache-drive regression. Args: extracted-disc-root isolated-user-root.
#include <rex/runtime.h>
#include <rex/kernel/init.h>
#include <rex/filesystem/vfs.h>
#include <rex/filesystem/file.h>
#include <array>
#include <cstdio>
int main(int argc, char** argv) {
  if (argc != 3) return 64;
  rex::Runtime runtime(argv[1], argv[2]);
  if (runtime.Setup(rex::RuntimeConfig{.kernel_init=rex::kernel::InitializeKernel,.tool_mode=true})) return 1;
  using namespace rex;
  using namespace rex::filesystem;
  auto* fs = runtime.file_system();
  File* file = nullptr; FileAction action{};
  auto open = [&](FileDisposition disposition) {
    return fs->OpenFile(nullptr, "cache:\\replay_stream", disposition,
        FileAccess::kGenericRead | FileAccess::kGenericWrite, false, true, &file, &action);
  };
  if (open(FileDisposition::kOverwriteIf)) return 2;
  const std::array<uint8_t,4> bytes{1,37,98,255}; size_t count=0;
  if (file->WriteSync(bytes, 4096, &count) || count != bytes.size()) return 3;
  file->Destroy(); file=nullptr;
  if (open(FileDisposition::kOpen)) return 4;
  std::array<uint8_t,4> read{};
  if (file->ReadSync(read, 4096, &count) || count != bytes.size() || read != bytes) return 5;
  file->Destroy();file=nullptr;
  if (open(FileDisposition::kCreate) != X_STATUS_OBJECT_NAME_COLLISION) return 6;
  if (open(FileDisposition::kOverwriteIf)) return 7;
  if (std::filesystem::file_size(std::filesystem::path(argv[2]) / "cache/files/replay_stream") != 0) return 8;
  file->Destroy();
  puts("PASS: cache create, seek/write, close/reopen/read, name collision and overwrite truncation");
}
