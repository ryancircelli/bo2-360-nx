/**
 * @file        rexcodegen/builders/context.cpp
 * @brief       PPC context instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include <rex/codegen/binary_view.h>
#include <rex/memory/utils.h>
#include <rex/logging.h>

#include "../codegen_logging.h"
#include <rex/system/export_resolver.h>

namespace rex::codegen {

/// eieio instruction encoding (big-endian). Used for MMIO detection:
/// if the next instruction after a load/store is eieio, the access is MMIO.
static constexpr uint32_t kEieioEncoding = 0xAC06007C;

//=============================================================================
// Convenience Accessors
//=============================================================================

const RecompilerConfig& BuilderContext::config() const {
  return emitCtx.config;
}

const FunctionGraph& BuilderContext::graph() const {
  return emitCtx.graph;
}

//=============================================================================
// Register Accessors
//=============================================================================

// Localizing a non-volatile assumes the function owns it across its whole body.
// An SEH funclet does not: it inherits its owner's live registers through ctx.
bool BuilderContext::localizeNonVolatiles() const {
  return config().nonVolatileRegistersAsLocalVariables && !fn.sharesRegisters();
}

bool BuilderContext::isReferenceCopy() const {
  return emitCtx.namePrefix == "ref_";
}

bool BuilderContext::isSaveRestoreHelper(std::string_view name) {
  return name.find("__rest") == 0 || name.find("__save") == 0;
}

// A localized function keeps the registers a save/restore helper would store
// or load in locals the helper cannot see, so the helper's body is inlined
// with the caller's locals in place of ctx: the frame holds exactly what the
// guest code expects, and the restore leaves the locals at their entry values
// for the publish at return. A reference copy (lockstep verify) inlines the
// helpers as they are, so their stack stores go through the logged macros and
// the copy can be rolled back; a call would write the frame behind the log's
// back. The helper's own generated body is the source, rendered once and
// cached, so every helper variant is covered.
void BuilderContext::inlineSaveRestoreHelper(const rex::codegen::FunctionNode& helper,
                                             std::string_view indent, bool tail) {
  static std::mutex cacheMutex;
  static std::unordered_map<std::string, std::vector<std::string>> cache;
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    // Reference copies render the helper with their own options.
    const std::string key = emitCtx.namePrefix + helper.name();
    auto it = cache.find(key);
    if (it == cache.end()) {
      rex::codegen::EmitContext sub = emitCtx;
      sub.referenced = nullptr;
      sub.referenceBlocked = nullptr;
      const std::string body = helper.emitCpp(sub);
      size_t pos = 0;
      while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos)
          end = body.size();
        std::string_view line(body.data() + pos, end - pos);
        pos = end + 1;
        size_t first = line.find_first_not_of('\t');
        if (first == std::string_view::npos)
          continue;
        std::string_view code = line.substr(first);
        if (code.starts_with("//") || code.starts_with("DEFINE_REX_FUNC") ||
            code.starts_with("REX_FUNC_PROLOGUE") || code == "}" || code == "return;" ||
            code.starts_with("PPC") || code.starts_with("uint32_t ea"))
          continue;
        lines.emplace_back(code);
      }
      it = cache.emplace(key, std::move(lines)).first;
    }
    lines = it->second;
  }
  for (const std::string& line : lines) {
    std::string rewritten;
    size_t pos = 0;
    while (pos < line.size()) {
      size_t at = line.find("ctx.", pos);
      if (at == std::string::npos) {
        rewritten.append(line, pos, std::string::npos);
        break;
      }
      rewritten.append(line, pos, at - pos);
      size_t p = at + 4;
      char kind = p < line.size() ? line[p] : 0;
      size_t q = p + 1;
      while (q < line.size() && std::isdigit(static_cast<unsigned char>(line[q])))
        ++q;
      if ((kind == 'r' || kind == 'f' || kind == 'v') && q > p + 1 &&
          (q == line.size() || !std::isalnum(static_cast<unsigned char>(line[q])))) {
        size_t index = std::stoul(std::string(line.substr(p + 1, q - p - 1)));
        std::string name;
        if (kind == 'r' && index >= 14)
          name = r(index);
        else if (kind == 'f' && index >= 14)
          name = f(index);
        else if (kind == 'v' && ((index >= 14 && index < 32) || index >= 64))
          name = v(index);
        if (!name.empty()) {
          rewritten += name;
          pos = q;
          continue;
        }
      }
      rewritten.append("ctx.");
      pos = at + 4;
    }
    if (rewritten.find("ea") != std::string::npos)
      (void)ea();
    if (rewritten.find("temp") != std::string::npos)
      (void)temp();
    // The marker tells the lockstep verifier that a local written here only
    // takes its entry value back (a restore), which ctx still holds.
    if (rewritten.starts_with("ctx.fpscr."))
      println("{}{}", indent, rewritten);  // the flush-mode pass matches these lines exactly
    else
      println("{}{}  // nv-helper", indent, rewritten);
  }
  if (tail)
    println("{}return;", indent);
}

std::string BuilderContext::r(size_t index) {
  const auto& cfg = config();
  if ((cfg.nonArgumentRegistersAsLocalVariables && !fn.sharesRegisters() &&
       (index == 0 || index == 2 || index == 11 || index == 12)) ||
      (localizeNonVolatiles() && index >= 14)) {
    locals.r[index] = true;
    return fmt::format("r{}", index);
  }
  return fmt::format("ctx.r{}", index);
}

std::string BuilderContext::f(size_t index) {
  const auto& cfg = config();
  if ((cfg.nonArgumentRegistersAsLocalVariables && index == 0) ||
      (localizeNonVolatiles() && index >= 14)) {
    locals.f[index] = true;
    return fmt::format("f{}", index);
  }
  return fmt::format("ctx.f{}", index);
}

// The scratch vector registers, v0 and v32-v63 (VMX128's volatile half), are never arguments or return values: a
// function may keep them in locals whatever it calls, since no callee reads them and the caller cannot count on
// them after a call. (v1 is not one in BO2: 55 functions read it as an input, tools/read_before_write.py.) A split
// piece (share_registers) still reads its owner's through ctx.
std::string BuilderContext::v(size_t index) {
  const auto& cfg = config();
  const bool vmx128_volatile = index >= 32 && index <= 63;
  const bool scratch = index == 0 || vmx128_volatile;
  if ((cfg.nonArgumentRegistersAsLocalVariables && vmx128_volatile) ||
      (cfg.vectorScratchRegistersAsLocalVariables && scratch && !fn.sharesRegisters()) ||
      (localizeNonVolatiles() && ((index >= 14 && index <= 31) || (index >= 64 && index <= 127)))) {
    locals.v[index] = true;
    return fmt::format("v{}", index);
  }
  return fmt::format("ctx.v{}", index);
}

std::string BuilderContext::cr(size_t index) {
  if (config().crRegistersAsLocalVariables && !fn.sharesRegisters()) {
    locals.cr[index] = true;
    return fmt::format("cr{}", index);
  }
  return fmt::format("ctx.cr{}", index);
}

const char* BuilderContext::ctr() {
  if (config().ctrAsLocalVariable && !fn.sharesRegisters()) {
    locals.ctr = true;
    return "ctr";
  }
  return "ctx.ctr";
}

const char* BuilderContext::xer() {
  if (config().xerAsLocalVariable && !fn.sharesRegisters()) {
    locals.xer = true;
    return "xer";
  }
  return "ctx.xer";
}

const char* BuilderContext::reserved() {
  if (config().reservedRegisterAsLocalVariable) {
    locals.reserved = true;
    return "reserved";
  }
  return "ctx.reserved";
}

const char* BuilderContext::temp() {
  locals.temp = true;
  return "temp";
}

const char* BuilderContext::v_temp() {
  locals.v_temp = true;
  return "vTemp";
}

const char* BuilderContext::env() {
  locals.env = true;
  return "env";
}

const char* BuilderContext::ea() {
  locals.ea = true;
  return "ea";
}

//=============================================================================
// Output Helpers
//=============================================================================

// Template implementations in header, but we need explicit instantiations
// for common format strings to avoid link errors in some cases

//=============================================================================
// Code Generation Helpers
//=============================================================================

bool BuilderContext::mmio_check_d_form() {
  if (base + 4 < fn.end() && *(data + 1) == kEieioEncoding)
    return true;
  return locals.is_mmio_base(insn.operands[2]);
}

bool BuilderContext::mmio_check_x_form() {
  if (base + 4 < fn.end() && *(data + 1) == kEieioEncoding)
    return true;
  return locals.is_mmio_base(insn.operands[1]) || locals.is_mmio_base(insn.operands[2]);
}

bool BuilderContext::stack_d_form() {
  return insn.operands[2] == 1 && !mmio_check_d_form();
}

bool BuilderContext::stack_x_form() {
  return insn.operands[1] == 1 && !mmio_check_x_form();
}

std::string BuilderContext::stack_macro(std::string_view plain) {
  for (std::string_view kind : {"REX_LOAD_", "REX_STORE_"}) {
    if (plain.starts_with(kind)) {
      return "REX_STACK_" + std::string(plain.substr(4));
    }
  }
  return std::string(plain);
}

const CallTarget* BuilderContext::findCallTarget(uint32_t site) const {
  // Search in calls (bl instructions)
  for (const auto& edge : fn.calls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  // Search in tail calls (b instructions to other functions)
  for (const auto& edge : fn.tailCalls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  return nullptr;
}

void BuilderContext::emit_call_to_function(const FunctionNode* targetFn, bool isTail) {
  const auto& cfg = config();
  const auto& name = targetFn->name();

  // Handle save/restore helpers. Gated on the global setting, not this
  // function's: the helper bodies are emitted under it too, so once they
  // spill to locals they are no-ops that scribble zeros on the caller's
  // frame. A share_registers function still has to elide them.
  if (cfg.nonVolatileRegistersAsLocalVariables &&
      (name.find("__rest") == 0 || name.find("__save") == 0)) {
    // print nothing - these are handled by local variable tracking
    return;
  }

  // A reference copy (lockstep verify) inlines the helpers as they are,
  // so their stack stores go through the logged macros and the copy can
  // be rolled back; a call would write the frame behind the log's back.
  // A tail branch's return; comes from the caller (build_b).
  if (isReferenceCopy() && isSaveRestoreHelper(name)) {
    inlineSaveRestoreHelper(*targetFn, "\t", false);
    return;
  }

  // An SEH funclet runs on its owner's frame and reads whatever non-volatiles
  // the owner left live, so hand it the localized copies through ctx and take
  // them back afterwards. Only registers already localized here can be live at
  // this point, so that set is the whole live-in the funclet can see.
  if (targetFn->sharesRegisters() && localizesAnything()) {
    emitCtx.reference(name);
    // A bl comes back here and the caller goes on with its registers; a b (tail
    // call) is followed by a return, so there is nothing to take back.
    emit_call_sharing_registers(fmt::format("{}(ctx, base);", name), !isTail, "\t");
    return;
  }

  // The save/restore helpers take the frame pointer in r12 and are not
  // ABI calls, so the checks skip them.
  const bool poison = cfg.abiPoisonChecks &&
                      !(name.find("__rest") == 0 || name.find("__save") == 0);
  emitCtx.reference(name);
  if (poison)
    println("\tREX_ABI_POISON_PRE(0x{:08X});", base);
  println("\t{}(ctx, base);", name);
  if (poison)
    println("\tREX_ABI_POISON_POST(0x{:08X});", base);
  return;
}

// A linker thunk whose only instruction is `b setjmp` / `b longjmp` is that
// function (after rexglue cc04cca27, BSD-3-Clause).
uint32_t BuilderContext::resolveJumpThunk(uint32_t address) const {
  const auto& cfg = config();
  if (address == cfg.longJmpAddress || address == cfg.setJmpAddress)
    return address;
  const auto* thunk = emitCtx.graph.getFunction(address);
  if (!thunk || thunk->isImport() || thunk->size() != 4)
    return address;
  const auto* data = emitCtx.binary.translate(address);
  if (!data)
    return address;
  const uint32_t word = rex::memory::load_and_swap<uint32_t>(data);
  if ((word >> 26) != 18 || (word & 3) != 0)  // b, not ba / bl
    return address;
  const uint32_t target = address + (static_cast<int32_t>(word << 6) >> 6 & ~3);
  if (target != 0 && (target == cfg.longJmpAddress || target == cfg.setJmpAddress))
    return target;
  return address;
}

void BuilderContext::emit_function_call(uint32_t address) {
  const auto& cfg = config();
  address = resolveJumpThunk(address);

  if (address == cfg.longJmpAddress) {
    // Use custom ppc_longjmp that uses guest address as key (not for storage)
    println("\tppc_longjmp({}.u32, {}.s32);", r(3), r(4));
    return;
  }

  if (address == cfg.setJmpAddress) {
    // Save PPCContext for restoration after longjmp
    println("\t{} = ctx;", env());
    // Use custom ppc_setjmp that uses guest address as key
    println("\t{}.s64 = ppc_setjmp({}.u32);", temp(), r(3));
    // Restore PPCContext if returning from longjmp
    println("\tif ({}.s64 != 0) ctx = {};", temp(), env());
    println("\t{} = {};", r(3), temp());
    return;
  }

  // Try to use pre-resolved call target from FunctionGraph
  if (const auto* target = findCallTarget(base)) {
    if (target->isFunction()) {
      bool isTail = false;
      for (const auto& edge : fn.tailCalls())
        if (edge.site == base)
          isTail = true;
      emit_call_to_function(target->asFunction(), isTail);
      return;
    }

    if (target->isImport()) {
      const auto& importTarget = std::get<CallTarget::ToImport>(target->value);
      std::string func_name;

      // Try to resolve ordinal to actual function name
      auto at_pos = importTarget.name.find('@');
      if (at_pos != std::string::npos && emitCtx.resolver) {
        auto lib_name = importTarget.name.substr(0, at_pos);
        auto ordinal_str = importTarget.name.substr(at_pos + 1);
        uint16_t ordinal = static_cast<uint16_t>(std::stoul(ordinal_str));

        auto* exp = emitCtx.resolver->GetExportByOrdinal(lib_name + ".xex", ordinal);
        if (!exp)
          exp = emitCtx.resolver->GetExportByOrdinal(lib_name, ordinal);

        if (exp) {
          func_name = "__imp__" + std::string(exp->name);
        }
      }

      if (func_name.empty()) {
        func_name = "__imp__" + importTarget.name;
        std::replace(func_name.begin(), func_name.end(), '@', '_');
        std::replace(func_name.begin(), func_name.end(), '.', '_');
      }

      emitCtx.reference(func_name);
      if (cfg.abiPoisonChecks)
        println("\tREX_ABI_POISON_PRE(0x{:08X});", base);
      println("\t{}(ctx, base);", func_name);
      if (cfg.abiPoisonChecks)
        println("\tREX_ABI_POISON_POST(0x{:08X});", base);
      return;
    }

    // Unresolved target from graph
    REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X}", address, base);
    println("\t// FATAL: unresolved function 0x{:08X}", address);
    println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
    return;
  }

  // No pre-resolved target found - this is an error
  REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X} (no CallTarget in FunctionNode)",
                   address, base);
  println("\t// FATAL: unresolved function 0x{:08X} (no CallTarget in FunctionNode)", address);
  println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
}

bool BuilderContext::localizesAnything() const {
  if (fn.sharesRegisters())
    return false;
  const auto& cfg = config();
  return cfg.nonVolatileRegistersAsLocalVariables || cfg.crRegistersAsLocalVariables ||
         cfg.ctrAsLocalVariable || cfg.xerAsLocalVariable || cfg.vectorScratchRegistersAsLocalVariables;
}

void BuilderContext::emit_call_sharing_registers(const std::string& call, bool copyBack,
                                                 const char* indent) {
  if (!localizesAnything()) {
    println("{}{}", indent, call);
    return;
  }
  // Every register the callee may read: those this function has localized so far. A
  // fragment reads what its owner set up, and the owner set it up before jumping. Copying
  // only those keeps the cost down on indirect tail jumps, which all go through here.
  std::vector<std::string> regs;
  for (size_t i = 14; i < 32; ++i)
    if (locals.r[i])
      regs.push_back(r(i));
  for (size_t i = 14; i < 32; ++i)
    if (locals.f[i])
      regs.push_back(f(i));
  for (size_t i = 0; i < 128; ++i)
    if (locals.v[i])
      regs.push_back(v(i));
  for (size_t i = 0; i < 8; ++i)
    if (locals.cr[i])
      regs.push_back(cr(i));
  if (locals.ctr)
    regs.push_back(ctr());
  if (locals.xer)
    regs.push_back(xer());
  if (regs.empty()) {
    println("{}{}", indent, call);
    return;
  }

  println("{}{{", indent);
  for (const auto& reg : regs)
    println("{}\tconst auto s_{} = ctx.{}; ctx.{} = {};", indent, reg, reg, reg, reg);
  println("{}\t{}", indent, call);
  for (const auto& reg : regs) {
    const bool back = copyBack && (reg[0] == 'r' || reg[0] == 'f' || reg[0] == 'v');
    if (back)
      println("{}\t{} = ctx.{};", indent, reg, reg);
    println("{}\tctx.{} = s_{};", indent, reg, reg);
  }
  println("{}}}", indent);
}

void BuilderContext::emit_conditional_branch(bool not_, std::string_view cond) {
  uint32_t target = insn.operands[1];

  // Use classifyTarget for consistent branch classification
  // false = branch instruction (not a call), so own-base means loop back
  auto kind = graph().classifyTarget(target, base, false);

  switch (kind) {
    case TargetKind::InternalLabel:
      // Target is within this function - local goto
      println("\tif ({}{}.{}) goto loc_{:08X};", not_ ? "!" : "", cr(insn.operands[0]), cond,
              target);
      break;

    case TargetKind::Function:
    case TargetKind::Import:
      // Conditional tail call to another function - check pre-resolved call target
      if (const auto* callTarget = findCallTarget(base)) {
        if (callTarget->isFunction()) {
          auto* targetFn = callTarget->asFunction();
          emitCtx.reference(targetFn->name());
          println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
          if (targetFn->sharesRegisters())
            emit_call_sharing_registers(fmt::format("{}(ctx, base);", targetFn->name()), false,
                                        "\t\t");
          else
            println("\t\t{}(ctx, base);", targetFn->name());
          println("\t\treturn;");
          println("\t}}");
        } else if (callTarget->isImport()) {
          const auto& importTarget = std::get<CallTarget::ToImport>(callTarget->value);
          std::string func_name = "__imp__" + importTarget.name;
          std::replace(func_name.begin(), func_name.end(), '@', '_');
          std::replace(func_name.begin(), func_name.end(), '.', '_');
          emitCtx.reference(func_name);
          println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
          println("\t\t{}(ctx, base);", func_name);
          println("\t\treturn;");
          println("\t}}");
        }
      } else {
        REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X} (no CallTarget)",
                         target, base);
        println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
                not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
      }
      break;

    case TargetKind::Unknown:
      // getFunctionContaining resolves the caller through a by-base lookup that an
      // overlapping node with a higher base can shadow, so a target that is one of
      // OUR OWN blocks can land here instead of InternalLabel. Fall back to our own
      // blocks - the same fallback build_b has for unconditional branches.
      if (fn.containsAddress(target)) {
        println("\tif ({}{}.{}) goto loc_{:08X};", not_ ? "!" : "", cr(insn.operands[0]), cond,
                target);
        break;
      }
      REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X}", target, base);
      println("\t// ERROR: conditional branch to unknown address 0x{:08X}", target);
      println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
              not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
      break;
  }
}

void BuilderContext::emit_set_flush_mode(bool enable) {
  auto newState = enable ? CSRState::VMX : CSRState::FPU;
  if (csrState != newState) {
    auto prefix = enable ? "enable" : "disable";
    auto suffix = csrState != CSRState::Unknown ? "Unconditional" : "";
    println("\tctx.fpscr.{}FlushMode{}();", prefix, suffix);

    csrState = newState;
  }
}

bool BuilderContext::has_mid_asm_hook() const {
  return config().midAsmHooks.find(base) != config().midAsmHooks.end();
}

void BuilderContext::emit_mid_asm_hook() {
  auto midAsmHook = config().midAsmHooks.find(base);
  if (midAsmHook == config().midAsmHooks.end()) {
    return;
  }

  bool returnsBool = midAsmHook->second.returnOnFalse || midAsmHook->second.returnOnTrue ||
                     midAsmHook->second.jumpAddressOnFalse != 0 ||
                     midAsmHook->second.jumpAddressOnTrue != 0;

  print("\t");
  if (returnsBool)
    print("if (");

  // Build call -- no ctx/base prefix, just register arguments resolved through accessors
  print("{}(", midAsmHook->second.name);
  for (auto& reg : midAsmHook->second.registers) {
    if (out.back() != '(')
      out += ", ";

    switch (reg[0]) {
      case 'c':
        if (reg == "ctr")
          out += ctr();
        else
          out += cr(std::atoi(reg.c_str() + 2));
        break;
      case 'x':
        out += xer();
        break;
      case 'r':
        if (reg == "reserved")
          out += reserved();
        else
          out += r(std::atoi(reg.c_str() + 1));
        break;
      case 'f':
        if (reg == "fpscr")
          out += "ctx.fpscr";
        else
          out += f(std::atoi(reg.c_str() + 1));
        break;
      case 'v':
        out += v(std::atoi(reg.c_str() + 1));
        break;
    }
  }

  if (returnsBool) {
    println(")) {{");

    if (midAsmHook->second.returnOnTrue)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnTrue != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnTrue);

    println("\t}}");

    println("\telse {{");

    if (midAsmHook->second.returnOnFalse)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnFalse != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnFalse);

    println("\t}}");
  } else {
    println(");");

    if (midAsmHook->second.ret)
      println("\treturn;");
    else if (midAsmHook->second.jumpAddress != 0)
      println("\tgoto loc_{:X};", midAsmHook->second.jumpAddress);
  }
}

//=============================================================================
// Vector (SIMD) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_vec_fp_binary(const char* simd_op) {
  println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_{}_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)));",
      v(insn.operands[0]), simd_op, v(insn.operands[1]), v(insn.operands[2]));
}

void BuilderContext::emit_vec_fp_unary_expr(std::string_view simd_expr) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);

  // Replace {vA} placeholder in expression with actual register
  std::string expr(simd_expr);
  size_t pos;
  while ((pos = expr.find("{vA}")) != std::string::npos) {
    expr.replace(pos, 4, vA);
  }

  println("\tsimde_mm_store_ps({}.f32, {});", vD, expr);
}

void BuilderContext::emit_vec_int_binary(const char* simd_op, const char* element_type) {
  println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      v(insn.operands[0]), element_type, simd_op, v(insn.operands[1]), element_type,
      v(insn.operands[2]), element_type);
}

void BuilderContext::emit_vec_int_binary_swapped(const char* simd_op, const char* element_type) {
  // Swapped: op(vB, vA) instead of op(vA, vB) - useful for andnot which has reversed operand
  // semantics
  println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      v(insn.operands[0]), element_type, simd_op, v(insn.operands[2]), element_type,
      v(insn.operands[1]), element_type);
}

void BuilderContext::emit_vec_var_shift(const char* shift_dir, const char* element_type,
                                        uint32_t mask_value) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);
  auto vB = v(insn.operands[2]);
  println("\t{{");
  println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", vB);
  println("\t\tsimde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_{}(0x{:X}));", element_type,
          mask_value);
  println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_{}_{}"
      "(a, shift));",
      vD, shift_dir, element_type);
  println("\t}}");
}

//=============================================================================
// Memory (Load/Store) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_load_d_form(const char* load_macro, const char* dest_type,
                                      bool check_mmio) {
  // D-form: rD = LOAD(rA + D) where operands[0]=rD, operands[1]=D, operands[2]=rA
  // load_macro should be like "REX_LOAD_U8" - we replace REX_LOAD with REX_MM_LOAD for MMIO
  const char* macro = load_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_d_form()) {
    // Replace "REX_LOAD_" with "REX_MM_LOAD_"
    if (strncmp(load_macro, "REX_LOAD_", 9) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_LOAD_%s", load_macro + 9);
      macro = mm_macro;
    }
  }

  std::string stack_name;
  if (check_mmio && stack_d_form()) {
    stack_name = stack_macro(load_macro);
    macro = stack_name.c_str();
  }

  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{});", static_cast<int32_t>(insn.operands[1]));
}

void BuilderContext::emit_load_x_form(const char* load_macro, const char* dest_type,
                                      bool check_mmio) {
  // X-form: rD = LOAD(rA + rB) where operands[0]=rD, operands[1]=rA, operands[2]=rB
  // load_macro should be like "REX_LOAD_U8" - we replace REX_LOAD with REX_MM_LOAD for MMIO
  const char* macro = load_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_x_form()) {
    // Replace "REX_LOAD_" with "REX_MM_LOAD_"
    if (strncmp(load_macro, "REX_LOAD_", 9) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_LOAD_%s", load_macro + 9);
      macro = mm_macro;
    }
  }

  std::string stack_name;
  if (check_mmio && stack_x_form()) {
    stack_name = stack_macro(load_macro);
    macro = stack_name.c_str();
  }

  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32);", r(insn.operands[2]));
}

void BuilderContext::emit_store_d_form(const char* store_macro, const char* src_type,
                                       bool check_mmio) {
  // D-form: STORE(rA + D, rS) where operands[0]=rS, operands[1]=D, operands[2]=rA
  // store_macro should be like "REX_STORE_U8" - we replace REX_STORE with REX_MM_STORE for MMIO
  const char* macro = store_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_d_form()) {
    // Replace "REX_STORE_" with "REX_MM_STORE_"
    if (strncmp(store_macro, "REX_STORE_", 10) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_STORE_%s", store_macro + 10);
      macro = mm_macro;
    }
  }

  std::string stack_name;
  if (check_mmio && stack_d_form()) {
    stack_name = stack_macro(store_macro);
    macro = stack_name.c_str();
  }

  print("\t{}(", macro);
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{}, {}.{});", static_cast<int32_t>(insn.operands[1]), r(insn.operands[0]), src_type);
}

void BuilderContext::emit_store_x_form(const char* store_macro, const char* src_type,
                                       bool check_mmio) {
  // X-form: STORE(rA + rB, rS) where operands[0]=rS, operands[1]=rA, operands[2]=rB
  // store_macro should be like "REX_STORE_U8" - we replace REX_STORE with REX_MM_STORE for MMIO
  const char* macro = store_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_x_form()) {  // Use X-form specific check (operands[1] is base)
    // Replace "REX_STORE_" with "REX_MM_STORE_"
    if (strncmp(store_macro, "REX_STORE_", 10) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_STORE_%s", store_macro + 10);
      macro = mm_macro;
    }
  }

  std::string stack_name;
  if (check_mmio && stack_x_form()) {
    stack_name = stack_macro(store_macro);
    macro = stack_name.c_str();
  }

  print("\t{}(", macro);
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32, {}.{});", r(insn.operands[2]), r(insn.operands[0]), src_type);
}

}  // namespace rex::codegen
