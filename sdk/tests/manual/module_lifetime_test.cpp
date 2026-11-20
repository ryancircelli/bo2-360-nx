// Loader-only integration test; executes no guest game code.
// Args: extracted-disc-root isolated-user-root module-path shared-library-target.
#include <rex/runtime.h>
#include <rex/kernel/init.h>
#include <rex/system/user_module.h>
#include <rex/ppc/context.h>
#include <cstdio>
#include <cstring>
extern "C" void __imp__XexLoadImage(PPCContext&, uint8_t*);
extern "C" void __imp__XexUnloadImage(PPCContext&, uint8_t*);
int main(int argc, char** argv) {
  if (argc != 5) return 64;
  rex::Runtime runtime(argv[1], argv[2]);
  if (runtime.Setup(rex::RuntimeConfig{.kernel_init=rex::kernel::InitializeKernel,.tool_mode=true})) return 1;
  if (runtime.LoadXexImage("game:\\default.xex")) return 2;
  auto* ks = runtime.kernel_state();
  ks->RegisterRecompiledModule(argv[3], argv[3], argv[4]);
  auto* memory = runtime.memory();
  auto storage = memory->SystemHeapAlloc(512);
  auto* bytes = memory->TranslateVirtual<uint8_t*>(storage);
  auto* output = memory->TranslateVirtual<rex::be<uint32_t>*>(storage + 384);
  auto path = std::string("game:\\") + argv[3];
  if (path.size() >= 384) return 3;
  std::memcpy(bytes, path.c_str(), path.size() + 1);
  auto load = [&]() {
    PPCContext ctx{};ctx.r3.u32=storage;ctx.r6.u32=storage+384;
    __imp__XexLoadImage(ctx,memory->virtual_membase());
    std::printf("LOAD status=%08X handle=%08X\n",ctx.r3.u32,uint32_t(*output));
    return ctx.r3.u32==0?uint32_t(*output):0;
  };
  auto unload = [&](uint32_t handle) {
    PPCContext ctx{};ctx.r3.u32=handle;
    __imp__XexUnloadImage(ctx,memory->virtual_membase());return ctx.r3.u32;
  };
  auto first=load();if(!first)return 4;
  auto second=load();if(second!=first)return 5;
  auto* ldr=memory->TranslateVirtual<rex::system::X_LDR_DATA_TABLE_ENTRY*>(first);
  if(ldr->load_count!=2)return 6;
  {
    auto lookup=ks->GetModule(path);if(!lookup || lookup->hmodule_ptr()!=first || ldr->load_count!=2)return 7;
    auto alias=ks->LoadUserModule(lookup->path(),false);
    if(!alias || alias->hmodule_ptr()!=first)return 8;
    auto repeated=ks->LoadUserModule(path,false);
    if(!repeated || repeated->hmodule_ptr()!=first)return 9;
    if(!ks->FindRecompiledModule(lookup->path())){std::puts("UNLOAD IDENTITY MISSING");return 10;}
  }
  if(unload(first) || ldr->load_count!=1 || !ks->GetModule(path))return 11;
  if(unload(first) || ks->GetModule(path))return 12;
  auto reloaded=load();if(!reloaded)return 13;
  ldr=memory->TranslateVirtual<rex::system::X_LDR_DATA_TABLE_ENTRY*>(reloaded);
  if(ldr->load_count!=1 || unload(reloaded))return 14;
  memory->SystemHeapFree(storage);
  std::puts("PASS: same handle, count 1->2->1->0, path aliases, final unload and reload");
}
