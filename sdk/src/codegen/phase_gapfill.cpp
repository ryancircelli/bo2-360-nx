/**
 * @file        codegen/phase_gapfill.cpp
 * @brief       GapFill phase: find uncovered code regions and register them as functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/instruction.h"
#include "ppc/disasm.h"
#include "codegen_flags.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <dis-asm.h>

#include <unordered_set>

#include <rex/codegen/phases.h>
#include "phase_helpers.h"

#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>

#include <ppc.h>

using rex::codegen::ppc::decode_instruction;
using rex::codegen::ppc::Opcode;
using rex::codegen::ppc::Disassemble;
using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

//=============================================================================
// GapFill to register uncovered code regions
//=============================================================================

// Check if address looks like exception handler data (handler ptr + rdata ptr)
bool looksLikeExceptionData(const BinaryView& binary, const FunctionGraph& graph, uint32_t addr) {
  const uint8_t* data = binary.translate(addr);
  if (!data)
    return false;

  // Exception handler data pattern:
  // [addr+0]: pointer to __C_specific_handler (entry point)
  // [addr+4]: pointer to scope table in .rdata
  uint32_t firstDword = load_and_swap<uint32_t>(data);
  uint32_t secondDword = load_and_swap<uint32_t>(data + 4);

  // Check if first dword is a known entry point (like __C_specific_handler)
  if (!graph.isEntryPoint(firstDword)) {
    return false;
  }

  // Check if second dword points to .rdata section
  auto* rdataSection = binary.findSectionByName(".rdata");
  if (!rdataSection)
    return false;

  uint32_t rdataStart = rdataSection->baseAddress;
  uint32_t rdataEnd = rdataStart + rdataSection->size;

  if (secondDword >= rdataStart && secondDword < rdataEnd) {
    REXCODEGEN_TRACE(
        "GapFill: 0x{:08X} looks like exception data (handler=0x{:08X}, scope=0x{:08X}), skipping",
        addr, firstDword, secondDword);
    return true;
  }

  return false;
}

class Coverage {
 public:
  /**
   * Adds a range.
   *
   * @param start First address of the range.
   * @param end Address 1 past the range.
   */
  void Add(uint32_t start, uint32_t end) {
    if (end <= start)
      return;
    auto it = ranges_.upper_bound(start);
    if (it != ranges_.begin()) {
      auto prev = std::prev(it);
      if (prev->second >= start) {
        start = prev->first;
        end = std::max(end, prev->second);
        it = ranges_.erase(prev);
      }
    }
    while (it != ranges_.end() && it->first <= end) {
      end = std::max(end, it->second);
      it = ranges_.erase(it);
    }
    ranges_[start] = end;
  }

  /**
   * Finds the end of the range that holds an address.
   *
   * @param addr Address to look up.
   * @return The address 1 past the range, or 0 when no range holds the address.
   */
  uint32_t CoveredUntil(uint32_t addr) const {
    auto it = ranges_.upper_bound(addr);
    if (it == ranges_.begin())
      return 0;
    --it;
    return addr < it->second ? it->second : 0;
  }

 private:
  std::map<uint32_t, uint32_t> ranges_;
};

/**
 * Lists the general registers an instruction names, in operand order.
 *
 * @param operands The operand text of the disassembled instruction.
 * @return The register numbers.
 */
std::vector<uint32_t> GetRegisterOperands(std::string_view operands) {
  std::vector<uint32_t> regs;
  for (size_t i = 0; i + 1 < operands.size(); ++i) {
    const bool starts_token = i == 0 || !std::isalnum(static_cast<unsigned char>(operands[i - 1]));
    if (operands[i] != 'r' || !starts_token ||
        !std::isdigit(static_cast<unsigned char>(operands[i + 1])))
      continue;
    uint32_t reg = 0;
    for (++i; i < operands.size() && std::isdigit(static_cast<unsigned char>(operands[i])); ++i)
      reg = reg * 10 + static_cast<uint32_t>(operands[i] - '0');
    regs.push_back(reg);
    --i;
  }
  return regs;
}

/**
 * Reports whether the first instruction of a function reads a nonvolatile register as an address
 * base or an operand before anything wrote it, or writes one before saving it. A store through r1
 * only saves, so it does not count.
 *
 * @param code Pointer to the big endian instruction word.
 * @return True when the word cannot start a function.
 */
bool UsesNonvolatileFirst(const uint8_t* code) {
  constexpr uint32_t kFirstNonvolatile = 14;
  constexpr uint32_t kStackPointer = 1;
  constexpr std::array<std::string_view, 6> kSourceFirst = {"st", "cmp", "mt", "tw", "td", "dcb"};

  ppc_insn insn;
  if (Disassemble(reinterpret_cast<const uint32_t*>(code), 4, 0, insn) <= 0 || insn.opcode == nullptr)
    return false;

  const std::string_view name = insn.opcode->name;
  auto regs = GetRegisterOperands(insn.op_str);
  const bool is_store = name.starts_with("st");
  const bool writes_first =
      std::none_of(kSourceFirst.begin(), kSourceFirst.end(),
                   [&](std::string_view prefix) { return name.starts_with(prefix); });
  if (writes_first && !regs.empty()) {
    if (regs.front() >= kFirstNonvolatile)
      return true;
    regs.erase(regs.begin());
  }
  if (is_store && regs.size() > 1 && regs[1] == kStackPointer)
    regs.erase(regs.begin(), regs.begin() + 2);

  return std::any_of(regs.begin(), regs.end(), [&](uint32_t reg) { return reg >= kFirstNonvolatile; });
}

/**
 * Reports whether an uncovered word is data rather than the start of a function.
 *
 * @param ctx Codegen context holding the function graph after discovery.
 * @param binary Binary view the words are read from.
 * @param region Executable region that holds the word.
 * @param addr Address of the candidate word.
 * @return True when the word does not decode, uses a nonvolatile register before writing it,
 *         is exception handler data, or never reaches a terminator inside the region.
 */
bool IsDataWord(const CodegenContext& ctx, const BinaryView& binary, const CodeRegion& region,
                const Coverage& coverage, uint32_t addr) {
  const uint8_t* first = binary.translate(addr);
  if (!first)
    return true;
  if (UsesNonvolatileFirst(first))
    return true;
  if (looksLikeExceptionData(binary, ctx.graph, addr))
    return true;

  for (uint32_t cursor = addr; cursor < region.end; cursor += 4) {
    // A short run that falls straight into known code is a stray literal word, not a function.
    if (cursor - addr <= 4 && cursor != addr && coverage.CoveredUntil(cursor))
      return true;
    const uint8_t* data = binary.translate(cursor);
    if (!data)
      return true;
    auto decoded = decode_instruction(cursor, load_and_swap<uint32_t>(data));
    if (decoded.opcode == Opcode::kUnknown) {
      // The fork's analysis decoder predates some opcodes accepted by its GNU
      // emitter (notably VMX128). Do not mistake those instructions for data.
      ppc_insn insn;
      if (Disassemble(data, 4, cursor, insn) <= 0 || !insn.opcode)
        return true;
    }
    if (decoded.is_return() || decoded.opcode == Opcode::bcctr || decoded.opcode == Opcode::b ||
        decoded.opcode == Opcode::ba || decoded.isTrapAlways()) {
      return false;
    }
  }
  return true;
}

Coverage BuildCoverage(const FunctionGraph& graph) {
  Coverage coverage;
  for (const auto& [addr, node] : graph.functions()) {
    coverage.Add(addr, addr + node->size());
  }
  return coverage;
}

void DiscoverRegisteredFunctions(CodegenContext& ctx, std::unordered_set<uint32_t>& known) {
  const size_t max_iterations = REXCVAR_GET(max_discovery_iterations);
  for (size_t iteration = 0; iteration < max_iterations; ++iteration) {
    const size_t count = ctx.graph.functionCount();
    if (discoverPendingFunctions(ctx, known) == 0 || ctx.graph.functionCount() == count)
      break;
    known = buildKnownFunctions(ctx.graph);
  }
}

void gapFillCodeRegions(CodegenContext& ctx) {
  REXCODEGEN_TRACE("Analyze: checking for uncovered code regions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();

  Coverage coverage = BuildCoverage(graph);
  auto known = buildKnownFunctions(graph);
  size_t registered = 0;

  for (const auto& region : ctx.scan.codeRegions) {
    uint32_t addr = region.start;
    while (addr < region.end) {
      if (uint32_t end = coverage.CoveredUntil(addr)) {
        addr = end;
        continue;
      }
      if (IsDataWord(ctx, binary, region, coverage, addr)) {
        addr += 4;
        continue;
      }

      const size_t before = graph.functionCount();
      graph.addFunction(addr, 4, FunctionAuthority::GAP_FILL, false);
      known.insert(addr);
      DiscoverRegisteredFunctions(ctx, known);
      REXCODEGEN_TRACE("GapFill: registered sub_{:08X}", addr);
      ++registered;

      if (graph.functionCount() > before + 1) {
        coverage = BuildCoverage(graph);
      } else if (const auto* node = graph.getFunction(addr)) {
        coverage.Add(addr, addr + node->size());
      }
      addr = std::max(addr + 4, coverage.CoveredUntil(addr));
    }
  }

  REXCODEGEN_TRACE("Analyze: registered {} gap functions", registered);
}


//=============================================================================
// Cleanup absorbed GAP_FILL functions
//=============================================================================

void cleanupAbsorbedGapFills(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  std::vector<uint32_t> toRemove;

  for (const auto& [addr, node] : graph.functions()) {
    if (node->authority() != FunctionAuthority::GAP_FILL)
      continue;

    for (const auto& [otherAddr, otherNode] : graph.functions()) {
      if (otherAddr == addr)
        continue;
      if (!otherNode->containsAddress(addr))
        continue;

      // This GAP_FILL is inside another function's blocks
      if (otherNode->authority() != FunctionAuthority::GAP_FILL) {
        // Absorbed by higher authority - remove
        toRemove.push_back(addr);
        break;
      } else if (otherAddr < addr) {
        // Both GAP_FILL, other has lower address - it survives
        toRemove.push_back(addr);
        break;
      }
    }
  }

  for (uint32_t addr : toRemove) {
    graph.removeFunction(addr);
  }

  if (!toRemove.empty()) {
    REXCODEGEN_TRACE("Analyze: removed {} absorbed GAP_FILL functions", toRemove.size());
  }
}

}  // anonymous namespace

namespace phases {

VoidResult GapFill(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  gapFillCodeRegions(ctx);

  // Discover blocks for gap-filled functions
  auto known = buildKnownFunctions(ctx.graph, /*excludeGapFill=*/true);
  size_t discovered = discoverPendingFunctions(ctx, known);
  REXCODEGEN_TRACE("Analyze: discovered blocks for {} gap-filled functions", discovered);

  cleanupAbsorbedGapFills(ctx);

  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
