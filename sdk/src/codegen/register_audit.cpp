/**
 * @file        rexcodegen/register_audit.cpp
 * @brief       Registers a localized function reads before any path from its entry writes them
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "register_audit.h"

#include <bitset>
#include <cctype>
#include <string>
#include <unordered_map>

namespace rex::codegen {
namespace {

// Register ids: r14-r31 0-17, f14-f31 18-35, v0-v127 36-163, cr0-cr7 164-171, ctr 172, xer 173.
constexpr int kRegs = 174;
using Regs = std::bitset<kRegs>;

bool IsWord(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

int Number(std::string_view s) {
  if (s.empty() || s.size() > 3)
    return -1;
  int n = 0;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c)))
      return -1;
    n = n * 10 + (c - '0');
  }
  return (s.size() > 1 && s[0] == '0') ? -1 : n;
}

/// The id of a localizable register's local name, else -1 (v1-v13 are never locals).
int RegId(std::string_view id) {
  if (id == "ctr")
    return 172;
  if (id == "xer")
    return 173;
  if (id.size() < 2)
    return -1;
  if (id.starts_with("cr")) {
    int n = Number(id.substr(2));
    return n >= 0 && n <= 7 ? 164 + n : -1;
  }
  int n = Number(id.substr(1));
  if (n < 0)
    return -1;
  switch (id[0]) {
    case 'r':
      return n >= 14 && n <= 31 ? n - 14 : -1;
    case 'f':
      return n >= 14 && n <= 31 ? 18 + n - 14 : -1;
    case 'v':
      return (n == 0 || (n >= 14 && n <= 127)) ? 36 + n : -1;
    default:
      return -1;
  }
}

std::string RegName(int id) {
  if (id < 18)
    return "r" + std::to_string(id + 14);
  if (id < 36)
    return "f" + std::to_string(id - 18 + 14);
  if (id < 164)
    return "v" + std::to_string(id - 36);
  if (id < 172)
    return "cr" + std::to_string(id - 164);
  return id == 172 ? "ctr" : "xer";
}

/// Bare identifiers (not a member after '.') that name a localizable register.
Regs Find(std::string_view s) {
  Regs out;
  for (size_t i = 0; i < s.size();) {
    if (!IsWord(s[i])) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < s.size() && IsWord(s[j]))
      ++j;
    if ((i == 0 || s[i - 1] != '.') && !std::isdigit(static_cast<unsigned char>(s[i]))) {
      int id = RegId(s.substr(i, j - i));
      if (id >= 0)
        out.set(id);
    }
    i = j;
  }
  return out;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
}

/// The leading identifier of s and its length, or "" if s does not start with one.
std::string_view LeadingId(std::string_view s) {
  size_t j = 0;
  while (j < s.size() && IsWord(s[j]))
    ++j;
  return s.substr(0, j);
}

/// REX_STORE_Un / REX_STACK_STORE_Un of rN/fN to ctx.r1/ctx.r12/ea: the register's id, else -1.
int StackStore(std::string_view s) {
  if (!s.starts_with("REX_STORE_U") && !s.starts_with("REX_STACK_STORE_U"))
    return -1;
  size_t open = s.find('(');
  size_t comma = s.find(',');
  if (open == std::string_view::npos || comma == std::string_view::npos || comma < open)
    return -1;
  std::string_view addr = Trim(s.substr(open + 1, comma - open - 1));
  auto base = [&](std::string_view b) {
    return addr.starts_with(b) && (addr.size() == b.size() || !IsWord(addr[b.size()]));
  };
  if (!base("ctx.r1.u32") && !base("ctx.r12.u32") && !base("ea"))
    return -1;
  std::string_view value = Trim(s.substr(comma + 1));
  if (!value.ends_with(");"))
    return -1;
  value = Trim(value.substr(0, value.size() - 2));
  size_t dot = value.find('.');
  if (dot == std::string_view::npos || (value[0] != 'r' && value[0] != 'f'))
    return -1;
  std::string_view field = value.substr(dot + 1);
  if (field != "u8" && field != "u16" && field != "u32" && field != "u64" && field != "f64" &&
      field != "s32" && field != "s64")
    return -1;
  return RegId(value.substr(0, dot));
}

struct Effect {
  Regs reads, writes;
  bool save = false;
};

Effect EffectOf(std::string s) {
  Effect e;
  // Declarations, the prologue and the copies around a call that shares registers.
  if (s.starts_with("PPC") || s.starts_with("uint") || s.starts_with("REX_FUNC_PROLOGUE") ||
      s.starts_with("const auto s_"))
    return e;
  {  // mr rN,rN
    std::string_view id = LeadingId(s);
    if (!id.empty() && s == std::string(id) + ".u64 = " + std::string(id) + ".u64;")
      return e;
  }
  if (int id = StackStore(s); id >= 0) {
    e.reads.set(id);
    e.save = true;
    return e;
  }
  if (s.starts_with("simde_mm_store") && (s.find("(simde__m128i*)REX_RAW_ADDR(") != std::string::npos ||
                                          s.find("(simde__m128i*)(base +") != std::string::npos)) {
    size_t store = s.find('(');
    std::string_view rest = std::string_view(s).substr(store + 1);
    if (rest.starts_with("(simde__m128i*)REX_RAW_ADDR(") || rest.starts_with("(simde__m128i*)(base +")) {
      Regs r = Find(s);
      bool vectorsOnly = r.any();
      for (int i = 0; i < 36 && vectorsOnly; ++i)
        vectorsOnly = !r.test(i);
      for (int i = 164; i < kRegs && vectorsOnly; ++i)
        vectorsOnly = !r.test(i);
      if (vectorsOnly) {
        e.reads = r;
        e.save = true;
        return e;
      }
    }
  }
  // The summary-overflow bit: only the overflow-enabled instructions set xer.so.
  const bool compare = s.find(".compare") != std::string::npos;
  for (size_t p; (p = s.find(".so = xer.so;")) != std::string::npos;) {
    size_t start = p;
    while (start > 0 && IsWord(s[start - 1]))
      --start;
    s.erase(start, p + 13 - start);
  }
  if (compare) {
    for (size_t p = s.find(", xer)"); p != std::string::npos; p = s.find(", xer)"))
      s.replace(p, 6, ")");
    for (size_t p = s.find(",xer)"); p != std::string::npos; p = s.find(",xer)"))
      s.replace(p, 5, ")");
  }
  std::string_view sv = s;
  std::string_view lead = LeadingId(sv);
  // A CR save: mfcr / mfocrf copy each field into a GPR ("rN.u64 |= crK.lt ? 0x.. : 0;").
  if (sv.find(" ? 0x") != std::string_view::npos && sv.ends_with(" : 0;")) {
    size_t eq = sv.find("= ");
    if (eq != std::string_view::npos) {
      std::string_view rhs = sv.substr(eq + 2);
      std::string_view crid = LeadingId(rhs);
      if (crid.starts_with("cr") && RegId(crid) >= 164) {
        e.reads.set(RegId(crid));
        e.save = true;
        if (int w = RegId(lead); w >= 0 && sv.substr(lead.size()).starts_with(".u64 = "))
          e.writes.set(w);
        return e;
      }
    }
  }
  // Methods that write a whole CR field.
  if (int id = RegId(lead); id >= 164 && id < 172 &&
                            (sv.substr(lead.size()).starts_with(".compare") ||
                             sv.substr(lead.size()).starts_with(".setFromMask"))) {
    e.reads = Find(sv.substr(lead.size()));
    e.writes.set(id);
    return e;
  }
  // A vector store into a register: simde_mm_store*((simde__m128i*)vN.u8, value).
  if (sv.starts_with("simde_mm_store")) {
    size_t open = sv.find('(');
    std::string_view rest = sv.substr(open + 1);
    if (rest.starts_with("("))
      rest.remove_prefix(1);
    if (rest.starts_with("simde__m128i*)"))
      rest.remove_prefix(14);
    rest = Trim(rest);
    std::string_view vid = LeadingId(rest);
    int id = RegId(vid);
    if (id >= 36 && id < 164 && rest.substr(vid.size()).starts_with(".")) {
      size_t comma = rest.find(',');
      if (comma != std::string_view::npos) {
        e.reads = Find(rest.substr(comma + 1));
        e.writes.set(id);
        return e;
      }
    }
  }
  // lhs(.member)* = rhs (not ==, not a compound assignment).
  if (!lead.empty() && !std::isdigit(static_cast<unsigned char>(lead[0]))) {
    size_t i = lead.size();
    for (;;) {
      if (i < sv.size() && sv[i] == '.') {
        ++i;
        while (i < sv.size() && (IsWord(sv[i]) || sv[i] == '[' || sv[i] == ']'))
          ++i;
        continue;
      }
      break;
    }
    size_t j = i;
    while (j < sv.size() && std::isspace(static_cast<unsigned char>(sv[j])))
      ++j;
    if (j < sv.size() && sv[j] == '=' && (j + 1 >= sv.size() || sv[j + 1] != '=')) {
      e.reads = Find(sv.substr(j + 1));
      if (int id = RegId(lead); id >= 0)
        e.writes.set(id);
      return e;
    }
  }
  e.reads = Find(sv);
  return e;
}

bool IsLabel(std::string_view s, std::string_view* name) {
  if (!s.starts_with("loc_") || !s.ends_with(":"))
    return false;
  for (char c : s.substr(4, s.size() - 5))
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return false;
  *name = s.substr(0, s.size() - 1);
  return true;
}

// "maybe unwritten" set: all registers but `except` when `all`, else `regs`.
struct Maybe {
  bool reached = false;
  bool all = false;
  Regs regs;  // all ? the written ones : the unwritten ones
  bool test(int id) const { return all ? !regs.test(id) : regs.test(id); }
  bool operator==(const Maybe&) const = default;
};

Maybe Merge(const Maybe& a, const Maybe& b) {
  if (!a.reached)
    return b;
  Maybe m{true, a.all || b.all, {}};
  if (a.all && b.all)
    m.regs = a.regs & b.regs;
  else if (a.all)
    m.regs = a.regs & ~b.regs;
  else if (b.all)
    m.regs = b.regs & ~a.regs;
  else
    m.regs = a.regs | b.regs;
  return m;
}

}  // namespace

std::vector<EarlyRead> FindEarlyReads(std::string_view body) {
  struct Stmt {
    std::string text;
    Effect e;
  };
  std::vector<std::vector<Stmt>> blocks(1);
  std::vector<std::vector<std::string>> gotos(1);
  std::vector<bool> openEnd{true};
  std::unordered_map<std::string, size_t> labels;

  bool inFunction = false;
  for (size_t pos = 0; pos < body.size();) {
    size_t nl = body.find('\n', pos);
    std::string_view raw = body.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = nl == std::string_view::npos ? body.size() : nl + 1;
    if (!inFunction) {
      inFunction = raw.starts_with("DEFINE_REX_FUNC(");
      continue;
    }
    if (raw.starts_with("}"))
      break;
    std::string_view s = Trim(raw);
    if (s.empty() || s.starts_with("//"))
      continue;
    std::string_view label;
    if (IsLabel(s, &label)) {
      if (!blocks.back().empty() || !labels.empty()) {
        blocks.emplace_back(), gotos.emplace_back(), openEnd.push_back(true);
      }
      labels[std::string(label)] = blocks.size() - 1;
      continue;
    }
    if (!openEnd.back())  // unreachable text after a top-level goto/return
      blocks.emplace_back(), gotos.emplace_back(), openEnd.push_back(true);
    blocks.back().push_back({std::string(s), EffectOf(std::string(s))});
    for (size_t g = s.find("goto loc_"); g != std::string_view::npos; g = s.find("goto loc_", g + 1)) {
      if (g > 0 && IsWord(s[g - 1]))
        continue;
      size_t e = g + 5;
      while (e < s.size() && IsWord(s[e]))
        ++e;
      gotos.back().emplace_back(s.substr(g + 5, e - g - 5));
    }
    const bool top = raw.starts_with("\t") && !raw.starts_with("\t\t");
    if (top && (s.starts_with("goto ") || s == "return;" || s.starts_with("__builtin_trap")))
      openEnd.back() = false;
    else if (!gotos.back().empty())  // a conditional jump leaves before the block's later writes
      blocks.emplace_back(), gotos.emplace_back(), openEnd.push_back(true);
  }

  const size_t n = blocks.size();
  std::vector<std::vector<size_t>> edges(n);
  std::vector<Regs> written(n);
  for (size_t i = 0; i < n; ++i) {
    for (const auto& t : gotos[i])
      if (auto it = labels.find(t); it != labels.end())
        edges[i].push_back(it->second);
    if (openEnd[i] && i + 1 < n)
      edges[i].push_back(i + 1);
    for (const auto& st : blocks[i])
      written[i] |= st.e.writes;
  }
  std::vector<Maybe> in(n);
  in[0] = {true, true, {}};
  std::vector<size_t> work{0};
  while (!work.empty()) {
    size_t i = work.back();
    work.pop_back();
    Maybe out = in[i];
    if (out.all)
      out.regs |= written[i];
    else
      out.regs &= ~written[i];
    for (size_t j : edges[i]) {
      Maybe merged = Merge(in[j], out);
      if (!(merged == in[j])) {
        in[j] = merged;
        work.push_back(j);
      }
    }
  }

  std::vector<EarlyRead> found;
  Regs reported;
  for (size_t i = 0; i < n; ++i) {
    if (!in[i].reached)
      continue;
    Regs cleared;
    for (const auto& st : blocks[i]) {
      if (!st.e.save) {
        for (int id = 0; id < kRegs; ++id) {
          if (st.e.reads.test(id) && !cleared.test(id) && !reported.test(id) && in[i].test(id)) {
            reported.set(id);
            found.push_back({RegName(id), st.text.substr(0, 100)});
          }
        }
      }
      cleared |= st.e.writes;
    }
  }
  return found;
}

std::string ProtectSetjmpLocals(std::string code) {
  static constexpr std::string_view kMark =
      "// setjmp_locals.py: the locals the game's setjmp saves, as they were at the setjmp";
  if (code.find("ppc_setjmp(") == std::string::npos)
    return code;
  // The saved locals this function declares ("\tPPCRegister r15{};").
  std::vector<std::string> regs;
  for (size_t pos = 0; pos < code.size();) {
    size_t nl = code.find('\n', pos);
    std::string_view line(code.data() + pos, (nl == std::string::npos ? code.size() : nl) - pos);
    pos = nl == std::string::npos ? code.size() : nl + 1;
    for (std::string_view type : {"\tPPCRegister ", "\tPPCCRRegister ", "\tPPCVRegister "}) {
      if (!line.starts_with(type) || !line.ends_with("{};"))
        continue;
      std::string_view name = line.substr(type.size(), line.size() - type.size() - 3);
      int id = RegId(name);
      // r14-r31, f14-f31, v64-v127, cr0-cr7 (not v0/v14-v63, ctr, xer: the setjmp does not save them)
      if (id >= 0 && (id < 36 || (id >= 36 + 64 && id < 164) || (id >= 164 && id < 172)))
        regs.emplace_back(name);
    }
  }
  if (regs.empty())
    return code;
  for (size_t at = code.find("env = ctx;\n"); at != std::string::npos;
       at = code.find("env = ctx;\n", at + 1)) {
    size_t lineStart = code.rfind('\n', at) + 1;  // npos + 1 == 0
    std::string tab = code.substr(lineStart, at - lineStart);
    if (tab.empty() || tab.find_first_not_of('\t') != std::string::npos)
      continue;
    const std::string head = tab + "env = ctx;\n" + tab + "temp.s64 = ppc_setjmp(";
    if (code.compare(lineStart, head.size(), head) != 0)
      continue;
    size_t argEnd = code.find(");\n", lineStart + head.size());
    if (argEnd == std::string::npos)
      continue;
    std::string r3 = code.substr(lineStart + head.size(), argEnd - lineStart - head.size());
    const std::string tail = tab + "if (temp.s64 != 0) ctx = env;\n";
    size_t tailAt = argEnd + 3;
    if (code.compare(tailAt, tail.size(), tail) != 0)
      continue;
    std::string out = tab + "env = ctx;\n" + tab + "{  " + std::string(kMark) + "\n";
    std::string back;
    for (const auto& r : regs) {
      out += tab + "const auto sj_" + r + " = " + r + ";\n";
      back += " " + r + " = sj_" + r + ";";
    }
    out += tab + "temp.s64 = ppc_setjmp(" + r3 + ");\n";
    out += tab + "if (temp.s64 != 0) { ctx = env;" + back + " }\n" + tab + "}\n";
    code.replace(lineStart, tailAt + tail.size() - lineStart, out);
    at = lineStart + out.size() - 1;
  }
  return code;
}

}  // namespace rex::codegen
