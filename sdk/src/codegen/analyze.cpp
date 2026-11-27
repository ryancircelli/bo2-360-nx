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
    if (ends && !ctx.Config().functions.contains(e.address))
      out.push_back(e.address);
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
    const auto targets = result ? std::vector<uint32_t>{} : tailBranchTargets(ctx);
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
