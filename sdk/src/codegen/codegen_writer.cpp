/**
 * @file        codegen/codegen_writer.cpp
 * @brief       Consolidated codegen output writer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/codegen_writer.h>
#include "codegen_flags.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <inja/inja.hpp>

#include <rex/codegen/function_graph.h>
#include <rex/codegen/output_partition.h>
#include <rex/codegen/template_registry.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmodule.h>
#include <rex/system/xmemory.h>

#include "codegen_logging.h"
#include "file_io.h"
#include "template_registry_internal.h"

namespace {

nlohmann::json buildTemplateData(const rex::codegen::CodegenContext& ctx,
                                 const std::vector<const rex::codegen::FunctionNode*>& functions,
                                 const std::unordered_map<uint32_t, std::string>& rexcrtByAddr) {
  const auto& cfg = ctx.Config();

  // Compute code_base and code_size from binary sections
  size_t codeMin = ~size_t(0);
  size_t codeMax = 0;
  for (const auto& section : ctx.binary().sections()) {
    if (section.executable) {
      if (section.baseAddress < codeMin)
        codeMin = section.baseAddress;
      if ((section.baseAddress + section.size) > codeMax)
        codeMax = section.baseAddress + section.size;
    }
  }

  // Build functions JSON array
  nlohmann::json functionsJson = nlohmann::json::array();
  for (const auto* fn : functions) {
    std::string funcName;
    bool isRexcrt = false;

    auto crtIt = rexcrtByAddr.find(static_cast<uint32_t>(fn->base()));
    if (crtIt != rexcrtByAddr.end()) {
      funcName = crtIt->second;
      isRexcrt = true;
    } else if (fn->base() == ctx.analysisState().entryPoint) {
      funcName = "xstart";
    } else if (!fn->name().empty()) {
      funcName = fn->name();
    } else {
      funcName = fmt::format("sub_{:08X}", fn->base());
    }

    functionsJson.push_back({
        {"address", fmt::format("0x{:X}", fn->base())},
        {"name", funcName},
        {"is_rexcrt", isRexcrt},
        {"below_code_base", (fn->base() < codeMin)},
        {"is_import", fn->authority() == rex::codegen::FunctionAuthority::IMPORT},
    });
  }

  // Build config flags
  nlohmann::json configFlags = {
      {"skip_lr", cfg.skipLr},
      {"ctr_as_local", cfg.ctrAsLocalVariable},
      {"xer_as_local", cfg.xerAsLocalVariable},
      {"reserved_as_local", cfg.reservedRegisterAsLocalVariable},
      {"skip_msr", cfg.skipMsr},
      {"cr_as_local", cfg.crRegistersAsLocalVariables},
      {"non_argument_as_local", cfg.nonArgumentRegistersAsLocalVariables},
      {"non_volatile_as_local", cfg.nonVolatileRegistersAsLocalVariables},
      {"vector_scratch_as_local", cfg.vectorScratchRegistersAsLocalVariables},
      {"abi_poison_checks", cfg.abiPoisonChecks},
      {"no_phys_alias", cfg.noPhysAlias},
      {"reference_functions", !cfg.referenceFunctions.empty() || cfg.referenceSampleEvery != 0},
  };

  return {
      {"project", cfg.projectName},
      {"image_base", fmt::format("0x{:X}", ctx.binary().baseAddress())},
      {"image_size", fmt::format("0x{:X}", ctx.binary().imageSize())},
      {"code_base", fmt::format("0x{:X}", codeMin)},
      {"code_size", fmt::format("0x{:X}", codeMax - codeMin)},
      {"rexcrt_heap", cfg.rexcrtFunctions.contains("RtlAllocateHeap") ? 1 : 0},
      {"thunk_reserve_size", fmt::format("0x{:X}", 0x10000u)},
      {"has_dll_modules", ctx.hasDllModules()},
      {"is_dll", ctx.isDllModule()},
      {"config_flags", configFlags},
      {"functions", functionsJson},
      {"recomp_files", nlohmann::json::array()},
  };
}

/// A store statement with `ctx.` dropped and any trailing comment removed, so
/// the reference copy's `REX_STORE_U64(ctx.r1.u32 + -16, ctx.r31.u64)` and a
/// localized body's `REX_STORE_U64(r1.u32 + -16, r31.u64)` match.
std::string NormalizedStore(std::string_view line) {
  const size_t first = line.find_first_not_of('\t');
  if (first == std::string_view::npos)
    return {};
  std::string_view code = line.substr(first);
  const size_t comment = code.find("  //");
  if (comment != std::string_view::npos)
    code = code.substr(0, comment);
  if (!code.starts_with("REX_STACK_STORE_") && !code.starts_with("REX_STORE_"))
    return {};
  std::string out;
  out.reserve(code.size());
  for (size_t i = 0; i < code.size(); ++i) {
    if (code.compare(i, 4, "ctx.") == 0) {
      i += 3;
      continue;
    }
    out += code[i];
  }
  return out;
}

/// Lockstep verify: the reference copy inlines the save/restore helpers, and
/// keeps the link-register slot store, but the optimized function may keep
/// those registers in host locals instead (or leave the helper a real call)
/// and never write those frame slots. Every save-area store of the copy (an
/// inlined helper line, or the LR slot) that the optimized body does not
/// contain is emitted as REX_REF_STORE_UNCHECKED_*: logged and rolled back,
/// never compared. Data stores are not touched, so they are all still
/// compared.
size_t UncheckSaveAreaStores(std::string& reference, const std::string& optimized) {
  std::unordered_set<std::string> present;
  for (size_t pos = 0; pos < optimized.size();) {
    size_t end = optimized.find('\n', pos);
    if (end == std::string::npos)
      end = optimized.size();
    std::string store = NormalizedStore(std::string_view(optimized).substr(pos, end - pos));
    if (!store.empty())
      present.insert(std::move(store));
    pos = end + 1;
  }
  size_t unchecked = 0;
  std::string out;
  out.reserve(reference.size() + 256);
  for (size_t pos = 0; pos < reference.size();) {
    size_t end = reference.find('\n', pos);
    const bool last = end == std::string::npos;
    if (last)
      end = reference.size();
    std::string line = reference.substr(pos, end - pos);
    pos = end + 1;
    const bool saveArea = line.ends_with("// nv-helper") ||
                          line.find("(ctx.r1.u32 + -8, ctx.r12.u32);") != std::string::npos;
    const std::string store = saveArea ? NormalizedStore(line) : std::string();
    if (!store.empty() && !present.count(store)) {
      for (std::string_view macro : {"REX_STACK_STORE_U32(", "REX_STACK_STORE_U64(",
                                     "REX_STORE_U32(", "REX_STORE_U64("}) {
        const size_t at = line.find(macro);
        if (at == std::string::npos)
          continue;
        const bool wide = macro.find("U64") != std::string_view::npos;
        line.replace(at, macro.size(),
                     wide ? "REX_REF_STORE_UNCHECKED_U64(" : "REX_REF_STORE_UNCHECKED_U32(");
        ++unchecked;
        break;
      }
    }
    out += line;
    if (!last)
      out += '\n';
  }
  reference = std::move(out);
  return unchecked;
}

}  // namespace

namespace rex::codegen {

bool IsGeneratedOutputName(std::string_view filename, std::string_view projectName) {
  auto dot = filename.rfind('.');
  if (dot == std::string_view::npos)
    return false;
  auto ext = filename.substr(dot);
  if (ext != ".cpp" && ext != ".h" && ext != ".cmake")
    return false;

  if (filename == "sources.cmake")
    return true;

  constexpr std::array<std::string_view, 4> kPrefixes{"ppc_recomp", "ppc_func_mapping",
                                                      "function_table_init", "ppc_config"};
  for (auto prefix : kPrefixes) {
    if (filename.starts_with(prefix))
      return true;
  }

  std::string projectPrefix(projectName);
  projectPrefix += '_';
  return filename.starts_with(projectPrefix);
}

constexpr size_t kOutputBufferReserveSize = 32 * 1024 * 1024;  // 32 MB

CodegenWriter::CodegenWriter(CodegenContext& ctx, Runtime* runtime)
    : ctx_(ctx), runtime_(runtime) {}

// Convenience accessors
FunctionGraph& CodegenWriter::graph() {
  return ctx_.graph;
}
const FunctionGraph& CodegenWriter::graph() const {
  return ctx_.graph;
}
const BinaryView& CodegenWriter::binary() const {
  return ctx_.binary();
}
RecompilerConfig& CodegenWriter::config() {
  return ctx_.Config();
}
const RecompilerConfig& CodegenWriter::config() const {
  return ctx_.Config();
}
AnalysisState& CodegenWriter::analysisState() {
  return ctx_.analysisState();
}
const AnalysisState& CodegenWriter::analysisState() const {
  return ctx_.analysisState();
}

bool CodegenWriter::write(bool force) {
  deletedFiles_.clear();
  writtenFiles_.clear();
  unchangedFiles_.clear();

  // --- Validation gate (from recompile.cpp) ---
  if (ctx_.errors.HasErrors() && !force) {
    REXCODEGEN_ERROR("Code generation blocked: {} validation errors. Use --force to override.",
                     ctx_.errors.Count());
    return false;
  }

  // --- Output directory setup (from recompile.cpp) ---
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;
  REXCODEGEN_TRACE("Output path: {}", outputPath.string());
  std::filesystem::create_directories(outputPath);

  // --- Everything below from recompiler.cpp recompile() ---
  REXCODEGEN_TRACE("Recompile: starting");
  out.reserve(kOutputBufferReserveSize);

  // Build sorted function list from graph
  std::vector<const FunctionNode*> functions;
  functions.reserve(graph().functionCount());
  for (const auto& [addr, node] : graph().functions()) {
    functions.push_back(node.get());
  }
  std::sort(functions.begin(), functions.end(),
            [](const auto* a, const auto* b) { return a->base() < b->base(); });

  // Build rexcrt reverse map and rename graph nodes
  std::unordered_map<uint32_t, std::string> rexcrtByAddr;
  for (const auto& [name, addr] : config().rexcrtFunctions) {
    auto crtName = fmt::format("rexcrt_{}", name);
    rexcrtByAddr[addr] = crtName;
    if (auto* node = graph().getFunction(addr)) {
      node->setName(std::move(crtName));
    }
  }

  const std::string& projectName = config().projectName;

  TemplateRegistry registry;
  if (!config().templateDir.empty())
    registry.loadOverrides(config().templateDir);

  auto tmplData = buildTemplateData(ctx_, functions, rexcrtByAddr);
  // Guest DLL ordinals are absent from the kernel export resolver. Bind their
  // local import thunks through guest dispatch rather than a host symbol from
  // another shared library (whose recompiled functions have hidden visibility).
  tmplData["guest_imports"] = nlohmann::json::array();
  if (runtime_) {
    for (const auto& sym : binary().importSymbols()) {
      const auto* node = graph().getFunction(sym.address);
      if (!node || node->authority() != FunctionAuthority::IMPORT ||
          !node->name().starts_with("sub_"))
        continue;
      auto at = sym.name.find('@');
      if (at == std::string::npos)
        continue;
      auto library = sym.name.substr(0, at);
      auto module = runtime_->kernel_state()->GetModule(library, true);
      if (!module)
        module = runtime_->kernel_state()->GetModule(library + ".dll", true);
      if (!module)
        continue;
      auto address = module->GetProcAddressByOrdinal(
          static_cast<uint16_t>(std::stoul(sym.name.substr(at + 1))));
      if (address)
        tmplData["guest_imports"].push_back({{"name", node->name()},
                                            {"address", fmt::format("0x{:08X}", address)}});
    }
  }


  // Generate {project}_pch.h (config + macros, stable enough to precompile)
  REXCODEGEN_TRACE("Recompile: generating {}_pch.h", projectName);
  out = renderWithJson(registry, "codegen/pch_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_pch.h", projectName));

  // Generate {project}_funcs.h (every guest function declaration)
  REXCODEGEN_TRACE("Recompile: generating {}_funcs.h", projectName);
  out = renderWithJson(registry, "codegen/funcs_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_funcs.h", projectName));

  // Generate {project}_init.h (the full surface, for init.cpp and consumers)
  REXCODEGEN_TRACE("Recompile: generating {}_init.h", projectName);
  out = renderWithJson(registry, "codegen/init_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_init.h", projectName));

  // Generate {project}_init.cpp (PPCImageConfig + PPCFuncMappings)
  REXCODEGEN_TRACE("Recompile: generating {}_init.cpp", projectName);
  out = renderWithJson(registry, "codegen/init_cpp", tmplData);
  SaveCurrentOutData(fmt::format("{}_init.cpp", projectName));

  // Generate {project}_register.cpp (registration function for hash-based dispatch)
  REXCODEGEN_TRACE("Recompile: generating {}_register.cpp", projectName);
  tmplData["is_dll"] = ctx_.isDllModule();
  out = renderWithJson(registry, "codegen/register_cpp", tmplData);
  SaveCurrentOutData(fmt::format("{}_register.cpp", projectName));

  // Filter out imports and rexcrt functions before recompilation
  std::erase_if(functions, [](const FunctionNode* fn) {
    return fn->authority() == FunctionAuthority::IMPORT;
  });
  std::erase_if(functions, [&rexcrtByAddr](const FunctionNode* fn) {
    return rexcrtByAddr.contains(static_cast<uint32_t>(fn->base()));
  });

  // Build EmitContext -- resolver is now properly connected
  EmitContext emitCtx{binary(), config(), graph(),
                      static_cast<uint32_t>(analysisState().entryPoint), nullptr};
  if (runtime_)
    emitCtx.resolver = runtime_->export_resolver();

  REXCODEGEN_TRACE("Recompiling {} functions...", functions.size());

  std::vector<std::string> bodies;
  std::vector<FunctionSize> sizes;
  std::vector<std::unordered_set<std::string>> references;
  bodies.reserve(functions.size());
  sizes.reserve(functions.size());
  references.reserve(functions.size());

  const size_t maxFileBytes = REXCVAR_GET(max_file_size_bytes);
  std::unordered_set<std::string> referenced;
  emitCtx.referenced = &referenced;

  // Reference copies (reference_functions, reference_sample_every): the
  // listed functions and every Nth leaf function are emitted a second time as
  // `ref_sub_X` with the reference options and logged stores, for a host
  // verifier that runs both and compares. Their direct callers go through the
  // weak `sub_` thunk, so the verifier's strong `sub_X` intercepts every call.
  std::unordered_set<uint32_t> referenceSet(config().referenceFunctions.begin(),
                                            config().referenceFunctions.end());
  if (config().referenceSampleEvery) {
    size_t leaf = 0;
    for (const auto* fn : functions) {
      if (fn->authority() == FunctionAuthority::IMPORT || !fn->calls().empty() ||
          !fn->tailCalls().empty() || fn->instructions().size() < 4 ||
          !fn->name().starts_with("sub_"))
        continue;
      if (leaf++ % config().referenceSampleEvery == 0)
        referenceSet.insert(static_cast<uint32_t>(fn->base()));
    }
  }
  RecompilerConfig referenceConfig = config();
  referenceConfig.crRegistersAsLocalVariables = false;
  referenceConfig.ctrAsLocalVariable = false;
  referenceConfig.xerAsLocalVariable = false;
  referenceConfig.reservedRegisterAsLocalVariable = false;
  referenceConfig.nonArgumentRegistersAsLocalVariables = false;
  referenceConfig.nonVolatileRegistersAsLocalVariables = false;
  referenceConfig.vectorScratchRegistersAsLocalVariables = false;
  referenceConfig.skipLr = false;
  referenceConfig.abiPoisonChecks = false;
  EmitContext referenceCtx{binary(), referenceConfig, graph(),
                           static_cast<uint32_t>(analysisState().entryPoint), emitCtx.resolver};
  referenceCtx.referenced = &referenced;
  referenceCtx.namePrefix = "ref_";
  std::vector<std::pair<uint32_t, std::string>> referenceCopies;
  size_t uncheckedReferenceStores = 0;
  static const char* const kStoreMacros[] = {"REX_STORE_U8", "REX_STORE_U16", "REX_STORE_U32",
                                             "REX_STORE_U64"};
  static const char* const kRefMacros[] = {"REX_REF_STORE_U8", "REX_REF_STORE_U16",
                                           "REX_REF_STORE_U32", "REX_REF_STORE_U64"};

  for (const auto* fn : functions) {
    referenced.clear();
    std::string code = fn->emitCpp(emitCtx);
    if (referenceSet.count(static_cast<uint32_t>(fn->base())) &&
        fn->authority() != FunctionAuthority::IMPORT) {
      bool blocked = false;
      referenceCtx.referenceBlocked = &blocked;
      std::string reference = fn->emitCpp(referenceCtx);
      referenceCtx.referenceBlocked = nullptr;
      if (blocked) {
        REXCODEGEN_DEBUG("Reference copy of 0x{:08X} dropped: an instruction the verifier cannot "
                         "roll back",
                         fn->base());
      } else {
        // The optimized function may keep the saved registers in host locals
        // (or leave the helper a real call) and never write the save slots:
        // compare everything else (see UncheckSaveAreaStores).
        uncheckedReferenceStores += UncheckSaveAreaStores(reference, code);
        std::string wrapped = "\n// Reference copy for the lockstep verifier (rex/ppc/reference.h)\n";
        for (size_t i = 0; i < std::size(kStoreMacros); ++i)
          wrapped += fmt::format("#pragma push_macro(\"{}\")\n#undef {}\n#define {}(x, y) {}(x, y)\n",
                                 kStoreMacros[i], kStoreMacros[i], kStoreMacros[i], kRefMacros[i]);
        wrapped += reference;
        for (size_t i = 0; i < std::size(kStoreMacros); ++i)
          wrapped += fmt::format("#pragma pop_macro(\"{}\")\n", kStoreMacros[i]);
        code += wrapped;
        referenceCopies.emplace_back(static_cast<uint32_t>(fn->base()), fn->name());
      }
    }
    if (code.size() > maxFileBytes) {
      REXCODEGEN_WARN("Function 0x{:08X} is {} bytes, exceeds max_file_size_bytes ({})", fn->base(),
                      code.size(), maxFileBytes);
    }
    sizes.push_back({static_cast<uint32_t>(fn->base()), code.size()});
    bodies.push_back(std::move(code));
    references.push_back(referenced);
  }
  emitCtx.referenced = nullptr;
  REXCODEGEN_TRACE("Emitted {} reference copies", referenceCopies.size());
  if (uncheckedReferenceStores)
    REXCODEGEN_INFO("reference copies: {} register save-area stores the optimized functions "
                    "leave to helpers or host locals are rolled back but not compared",
                    uncheckedReferenceStores);

  // The reference functions, for the host verifier: one macro line each.
  if (!referenceCopies.empty()) {
    println("// Generated by rexglue: the functions emitted with a reference copy.");
    println("// The includer defines REX_REFERENCE_FUNCTION(address, name).");
    println("#pragma once\n");
    println("#define REX_REFERENCE_COUNT {}", referenceCopies.size());
    for (const auto& [address, name] : referenceCopies)
      println("REX_REFERENCE_FUNCTION(0x{:08X}, {})", address, name);
    SaveCurrentOutData(fmt::format("{}_reference.h", projectName));
  }

  auto partition = OutputPartition::Load(outputPath / kPartitionFileName);
  auto buckets = partition.Assign(sizes, maxFileBytes);

  for (size_t index = 0; index < buckets.size(); ++index) {
    // Buckets are address-ordered, so a call can precede its definition.
    std::unordered_set<std::string> needed;
    for (size_t entry : buckets[index]) {
      needed.insert(references[entry].begin(), references[entry].end());
    }

    std::vector<std::string> ordered(needed.begin(), needed.end());
    std::sort(ordered.begin(), ordered.end());
    println("#pragma once\n");
    println("#include \"{}_pch.h\"\n", projectName);
    for (const auto& name : ordered) {
      println("DECLARE_REX_FUNC({});", name);
    }
    SaveCurrentOutData(fmt::format("{}_funcs.{}.h", projectName, index));

    println("#include \"{}_funcs.{}.h\"\n", projectName, index);
    for (size_t entry : buckets[index]) {
      out += bodies[entry];
    }
    SaveCurrentOutData(fmt::format("{}_recomp.{}.cpp", projectName, index));
  }

  out = partition.Serialize();
  SaveCurrentOutData(kPartitionFileName);

  REXCODEGEN_TRACE("Recompilation complete.");

  // Generate sources.cmake
  REXCODEGEN_TRACE("Recompile: generating sources.cmake");
  {
    auto& recompFiles = tmplData["recomp_files"];
    recompFiles = nlohmann::json::array();
    for (size_t i = 0; i < buckets.size(); ++i) {
      recompFiles.push_back(fmt::format("{}_recomp.{}.cpp", projectName, i));
    }
    out = renderWithJson(registry, "codegen/sources_cmake", tmplData);
    SaveCurrentOutData("sources.cmake");
  }

  // The loaded image (title update applied) this code came from, for tools that read its data (metal360).
  if (runtime_) {
    auto* image = runtime_->memory()->TranslateVirtual<const char*>(ctx_.binary().baseAddress());
    pendingWrites.emplace_back("image.bin", std::string(image, ctx_.binary().imageSize()));
  }

  // Write all buffered files to disk
  return FlushPendingWrites();
}

void CodegenWriter::SaveCurrentOutData(const std::string_view name) {
  if (!out.empty()) {
    pendingWrites.emplace_back(std::string(name), std::move(out));
    out.clear();
  }
}

bool CodegenWriter::FlushPendingWrites() {
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;

  std::unordered_set<std::string> emitted;
  emitted.reserve(pendingWrites.size());

  // A swallowed failure would be stamped as success and skipped on the next run.
  bool ok = true;

  for (const auto& [filename, content] : pendingWrites) {
    emitted.insert(filename);
    auto filePath = outputPath / filename;

    switch (WriteIfChanged(filePath, content)) {
      case WriteOutcome::Failed:
        REXCODEGEN_ERROR("Failed to write {}", filePath.string());
        ok = false;
        break;
      case WriteOutcome::Unchanged:
        REXCODEGEN_TRACE("Unchanged, skipping write: {}", filePath.string());
        unchangedFiles_.push_back(filename);
        break;
      case WriteOutcome::Written:
        REXCODEGEN_TRACE("Wrote {} bytes to {}", content.size(), filePath.string());
        writtenFiles_.push_back(filename);
        break;
    }
  }

  for (const auto& entry : std::filesystem::directory_iterator(outputPath)) {
    if (!entry.is_regular_file())
      continue;
    auto filename = entry.path().filename().string();
    if (emitted.contains(filename))
      continue;
    if (!IsGeneratedOutputName(filename, config().projectName))
      continue;

    std::error_code ec;
    std::filesystem::remove(entry.path(), ec);
    if (ec) {
      REXCODEGEN_ERROR("Failed to delete stale output {}: {}", entry.path().string(), ec.message());
      continue;
    }
    REXCODEGEN_TRACE("Deleted stale output: {}", entry.path().string());
    deletedFiles_.push_back(filename);
  }

  pendingWrites.clear();
  return ok;
}

}  // namespace rex::codegen
