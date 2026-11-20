// Loader-only regression; no guest game code is executed.
// Args: extracted-disc-root isolated-user-root module-path shared-library-target.
#include <rex/runtime.h>
#include <rex/kernel/init.h>
#include <rex/system/user_module.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ppc/function.h>
#include <cstdio>
static void LiveFunction(PPCContext&, uint8_t*) {}

int main(int argc, char** argv) {
  if (argc != 5) return 64;
  rex::Runtime runtime(argv[1], argv[2]);
  if (runtime.Setup(rex::RuntimeConfig{.kernel_init=rex::kernel::InitializeKernel,.tool_mode=true})) return 1;
  if (runtime.LoadXexImage("game:\\default.xex")) return 2;
  auto* ks = runtime.kernel_state();
  auto* dispatcher = runtime.function_dispatcher();
  ks->RegisterRecompiledModule(argv[3], argv[3], argv[4]);
  if (!dispatcher->InitializeFunctionTable(0x823E0000, 0xE0F59C, 0x82000000, 0x1620000, true)) return 9;
  auto path = std::string("game:\\") + argv[3];
  uint32_t live_slot = dispatcher->AllocateThunk(&LiveFunction, 0x823E0000);
  if (!live_slot) return 10;
  auto* kernel_function = rex::ppc::FindPPCFuncByName("__imp__NtClose");
  if (!kernel_function) return 13;
  uint32_t kernel_slot = dispatcher->AllocateThunk(kernel_function, 0x823E0000);
  if (!kernel_slot) return 14;
  uint32_t first_slot = 0;
  for (unsigned cycle = 0; cycle < 64; ++cycle) {
    auto module = ks->LoadUserModule(path, false);
    if (!module) return 3;
    uint32_t slot = 0;
    bool stable = true;
    for (unsigned lookup = 0; lookup < 20000; ++lookup) {
      auto address = module->GetProcAddressByOrdinal(6, 0x823E0000);
      if (!address) {
        std::printf("FAIL: thunk exhausted cycle=%u lookup=%u\n", cycle, lookup);
        return 4;
      }
      if (!slot) slot = address;
      stable &= address == slot;
      if (!dispatcher->GetFunction(address)) return 5;
    }
    if (!stable) return 6;
    if (!first_slot) first_slot = slot;
    if (slot != first_slot) return 7;
    auto dll_slot = dispatcher->AllocateThunk(&LiveFunction, module->GetProcAddressByOrdinal(6, 0));
    if (!dll_slot || dll_slot == live_slot) return 11;
    ks->UnloadUserModule(module, false);
    module.reset();
    if (dispatcher->GetFunction(slot) || dispatcher->GetFunction(dll_slot)) return 8;
    if (dispatcher->GetFunction(kernel_slot) != kernel_function) return 15;
    if (dispatcher->GetFunction(live_slot) != &LiveFunction ||
        dispatcher->AllocateThunk(&LiveFunction, 0x823E0000) != live_slot) return 12;
  }
  std::puts("PASS: 64 load/unload cycles, 20000 lookups each, stable/recycled caller stub, cleared dispatch on unload");
}
