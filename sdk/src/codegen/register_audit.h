/**
 * @file        rexcodegen/register_audit.h
 * @brief       Registers a localized function reads before any path from its entry writes them
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rex::codegen {

/// One register (as its local's name, e.g. "cr6", "ctr", "r31") that the generated function
/// reads while some path from its entry has not written it yet, with the first such line.
struct EarlyRead {
  std::string reg;
  std::string line;
};

/// Reads of localized registers (r14-r31, f14-f31, v0, v14-v127, cr0-cr7, ctr, xer as bare C++
/// locals) that may see the zero a local starts with instead of the value the PowerPC code
/// would see. Such a function is a piece of a split function (its owner jumped in with
/// registers set) or reads its caller's registers, and has to keep them in ctx
/// (share_registers). Harmless reads are not reported: stores of a register to the stack
/// before it is written (prologue saves), mfcr/mfocrf copies of CR (a CR save), and the
/// summary-overflow bit copied into compares. Port of tools/read_before_write.py's analysis.
/// body: the text of one DEFINE_REX_FUNC, as FunctionNode::emitCpp writes it.
std::vector<EarlyRead> FindEarlyReads(std::string_view body);

/// The game's setjmp saves CR, r14-r31, f14-f31 and v64-v127 and its longjmp puts them back; as C++
/// locals they are indeterminate after the host longjmp. Rewrites each setjmp site of one function
/// so the locals it declares are copied to constants before the setjmp and back after a longjmp
/// (port of tools/setjmp_locals.py --fix, same text, so that tool's --verify accepts it).
std::string ProtectSetjmpLocals(std::string code);

}  // namespace rex::codegen
