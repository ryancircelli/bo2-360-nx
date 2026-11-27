/**
 * @file        codegen/analyze.cpp
 * @brief       Analysis pipeline orchestrator
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "decoded_binary.h"

#include <algorithm>
#include <map>

#include <rex/codegen/analysis_errors.h>
#include <rex/codegen/analyze.h>
#include <rex/codegen/phases.h>
#include <rex/memory/utils.h>
#include <rex/logging.h>

#include "codegen_logging.h"

namespace rex::codegen {

namespace {

// Unresolved plain branches (not bl) whose target sits right after a word that ends a function
// (blr, bctr, an unconditional b, or zero padding): thunks reached only by `b`.
std::vector<uint32_t> tailBranchTargets(const CodegenContext& ctx) {
  using rex::memory::load_and_swap;
  std::vector<uint32_t> out;
  for (const auto& e : ctx.errors.Entries()) {
    if (e.category != AnalysisErrors::Category::UnresolvedCall)
      continue;
    const uint8_t* site = ctx.binary().translate(e.secondaryAddress);
    const uint8_t* before = ctx.binary().translate(e.address - 4);
    if (!site || !before || (load_and_swap<uint32_t>(site) & 1))
      continue;
    const uint32_t w = load_and_swap<uint32_t>(before);
    const bool ends = w == 0 || w == 0x4E800020 || w == 0x4E800420 || ((w >> 26) == 18 && !(w & 1));
    // A function whose first instruction is a plain branch is a thunk: its target is a function.
    const bool thunk = ctx.graph.getFunction(e.secondaryAddress) != nullptr;
    if ((ends || thunk) && !ctx.Config().functions.contains(e.address))
      out.push_back(e.address);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

/// Addresses loaded with a lis+addi / lis+ori pair in executable code whose previous word ends a
/// function or is a call. A call to a function that never returns lets the body walk run on into the
/// next function; the pointer is the only mark of where that function starts (this replaced the
/// "lis+addi" rows of tools/codegen_gaps.py).
std::vector<uint32_t> pointerEntries(const CodegenContext& ctx) {
  using rex::memory::load_and_swap;
  const BinaryView& bin = ctx.binary();
  const uint32_t lo = bin.baseAddress(), hi = bin.baseAddress() + bin.imageSize();
  std::vector<uint32_t> out;
  auto consider = [&](uint32_t t) {
    if (t % 4 || !bin.isExecutable(t) || !bin.isExecutable(t - 4) || bin.isInImportExportRange(t) ||
        ctx.Config().functions.contains(t) || ctx.graph.getFunction(t) || !ctx.graph.getFunctionContaining(t))
      return;  // only a target inside another function's body is a swallowed function
    const uint32_t prev = load_and_swap<uint32_t>(bin.translate(t - 4));
    const uint32_t w = load_and_swap<uint32_t>(bin.translate(t));
    const bool exitOrCall = prev == 0x4E800020 || prev == 0x4E800420 || (prev >> 26) == 18;
    if (exitOrCall && w != 0 && !(w >= lo && w < hi))
      out.push_back(t);
  };
  for (const auto& sec : bin.sections()) {
    if (!sec.executable)
      continue;
    for (uint32_t a = sec.baseAddress; a + 4 <= sec.end(); a += 4) {
      const uint32_t w = load_and_swap<uint32_t>(sec.data + (a - sec.baseAddress));
      if ((w >> 26) != 15 || (w & 0xFFFF) < 0x8000 || ((w >> 16) & 31) != 0)
        continue;  // lis rD, hi (addis rD, 0, hi) with a negative hi: an 0x8xxxxxxx address
      const uint32_t rd = (w >> 21) & 31, base = (w & 0xFFFF) << 16;
      for (uint32_t j = 1; j <= 6 && a + 4 * j + 4 <= sec.end(); ++j) {
        const uint32_t v = load_and_swap<uint32_t>(sec.data + (a + 4 * j - sec.baseAddress));
        if ((v >> 26) == 14 && ((v >> 16) & 31) == rd)
          consider(base + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFF))));
        else if ((v >> 26) == 24 && ((v >> 21) & 31) == rd)
          consider(base | (v & 0xFFFF));
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Result<void> AnalyzeOnce(CodegenContext& ctx, ProgressReporter* reporter);

}  // namespace

Result<void> Analyze(CodegenContext& ctx, ProgressReporter* reporter) {
  // A tail-branch target found by Validate becomes a config function and the analysis runs again
  // from the start, as a hand-written `"0x..." = { }` override would (this replaced
  // tools/codegen_fix_unresolved.py). Each round only adds functions, so it ends.
  const AnalysisState start = ctx.analysisState();
  for (int round = 1;; ++round) {
    auto result = AnalyzeOnce(ctx, reporter);
    auto targets = result ? std::vector<uint32_t>{} : tailBranchTargets(ctx);
    if (round == 1) {
      const auto pointers = pointerEntries(ctx);
      REXCODEGEN_INFO("Analyze: {} lis+addi targets inside another function's body", pointers.size());
      targets.insert(targets.end(), pointers.begin(), pointers.end());
    }
    if (targets.empty() || round == 16)
      return result;
    REXCODEGEN_INFO("Analyze: {} tail-branch targets declared as functions, analysing again (round {})",
                    targets.size(), round + 1);
    for (uint32_t t : targets)
      ctx.Config().functions.emplace(t, FunctionConfig{});
    ctx.graph = FunctionGraph{};
    ctx.errors.Clear();
    ctx.scan = {};
    ctx.analysisState() = start;
  }
}

namespace {

Result<void> AnalyzeOnce(CodegenContext& ctx, ProgressReporter* reporter) {
  REXCODEGEN_TRACE("Analyze: starting analysis...");

  ctx.initDecoded();
  REXCODEGEN_TRACE("Analyze: decoded {} instructions across {} code regions",
                   ctx.decoded().instructionCount(), ctx.decoded().codeRegions().size());

  // 1. Register entry points (imports, helpers, config, pdata)
  if (reporter)
    reporter->phaseChanged("Register");
  auto regResult = phases::Register(ctx, reporter);
  if (!regResult) {
    return regResult;
  }

  // 2. Scan binary into code/data regions
  if (reporter)
    reporter->phaseChanged("Scan");
  auto scanResult = phases::Scan(ctx, reporter);
  if (!scanResult) {
    return scanResult;
  }

  // 3. Discover function blocks iteratively (includes vtable scan)
  if (reporter)
    reporter->phaseChanged("Discover");
  auto discoverResult = phases::Discover(ctx, reporter);
  if (!discoverResult) {
    return discoverResult;
  }

  // 3.5. Function pointer scan: find lis/addi pairs loading code addresses
  // TODO(tomc): disabled for now, causes too many false positives
  // functionPointerScan(ctx);

  // 4. Gap fill uncovered regions + discover blocks for gap-filled functions + cleanup
  if (reporter)
    reporter->phaseChanged("GapFill");
  auto gapFillResult = phases::GapFill(ctx, reporter);
  if (!gapFillResult) {
    return gapFillResult;
  }

  // 5. Merge: resolve jumps and seal functions
  if (reporter)
    reporter->phaseChanged("Merge");
  auto mergeResult = phases::Merge(ctx, reporter);
  if (!mergeResult) {
    return mergeResult;
  }

  // 6. Validate
  if (reporter)
    reporter->phaseChanged("Validate");
  auto validateResult = phases::Validate(ctx, reporter);
  if (!validateResult) {
    return validateResult;
  }

  REXCODEGEN_TRACE("Analyze: complete - {} functions ready for code generation",
                   ctx.graph.functionCount());

  return Ok();
}

}  // namespace

//=============================================================================
// AnalysisErrors implementation
//=============================================================================

const char* AnalysisErrors::CategoryName(Category cat) {
  switch (cat) {
    case Category::UnresolvedCall:
      return "UnresolvedCall";
    case Category::MissingJumpTable:
      return "MissingJumpTable";
    case Category::JumpTargetOutOfBounds:
      return "JumpTargetOutOfBounds";
    case Category::DiscontinuousFunction:
      return "DiscontinuousFunction";
    case Category::UnimplementedInsn:
      return "UnimplementedInsn";
    default:
      return "Unknown";
  }
}

void AnalysisErrors::Add(Category cat, uint32_t addr, const std::string& msg) {
  Add(cat, addr, 0, msg);
}

void AnalysisErrors::Add(Category cat, uint32_t addr, uint32_t secondary, const std::string& msg) {
  entries_.push_back({cat, addr, secondary, msg});
}

size_t AnalysisErrors::Count(Category cat) const {
  return std::count_if(entries_.begin(), entries_.end(),
                       [cat](const Entry& e) { return e.category == cat; });
}

void AnalysisErrors::PrintReport() const {
  if (entries_.empty()) {
    return;
  }

  REXCODEGEN_ERROR("=== ANALYSIS ERRORS ===");

  // Group by category
  std::map<Category, std::vector<const Entry*>> byCategory;
  for (const auto& entry : entries_) {
    byCategory[entry.category].push_back(&entry);
  }

  for (const auto& [cat, entries] : byCategory) {
    REXCODEGEN_ERROR("{} ({}):", CategoryName(cat), entries.size());

    for (const auto* entry : entries) {
      if (entry->secondaryAddress != 0) {
        REXCODEGEN_ERROR("  0x{:08X} from 0x{:08X}: {}", entry->address, entry->secondaryAddress,
                         entry->message);
      } else {
        REXCODEGEN_ERROR("  0x{:08X}: {}", entry->address, entry->message);
      }
    }
  }

  REXCODEGEN_ERROR("Total: {} errors", entries_.size());
}

}  // namespace rex::codegen
