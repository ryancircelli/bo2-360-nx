/**
 * @file        system/game_patches.cpp
 * @brief       Xenia-canary style game patch files: parse, match, classify, apply/revert, persist
 *
 * @copyright   Copyright (c) 2026
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Behaviour matched to xenia-canary 352b876 (src/xenia/patcher, kernel/user_module.cc), read for behaviour only:
 *  - file name ^[0-9A-Fa-f]{8}.*\.patch\.toml$; title_name, title_id (hex string) and hash (hex string or array of
 *    them) are required, else the file is skipped.
 *  - per [[patch]]: name, desc, author, is_enabled (default false); writes are read per type in the order
 *    array, be16, be32, be64, be8, f32, f64, string, u16string; a malformed write stops reading that patch's
 *    remaining writes (the ones read so far stay).
 *  - be8 stores the low byte; be16/32/64 big-endian; f32/f64 accept float or integer values, big-endian;
 *    string = the UTF-8 bytes, no terminator; u16string = UTF-16 in HOST (little-endian) order, no terminator
 *    (that is what canary writes, so files tested there expect it); array = hex digits, optional 0x.
 *  - module hash: XXH3-64 over [base + first_code_descriptor_index * page_size,
 *    base + (last_code_descriptor_index + 1) * page_size) — descriptor INDEX times page size, not the descriptors'
 *    page counts — after imports are set up: canary has rewritten every import thunk to `sc 2; blr; nop; nop`,
 *    so we hash a copy with the same stubs.
 */

#include <rex/system/game_patches.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include <fmt/format.h>
#include <toml++/toml.hpp>
#include <xxhash.h>

#include <rex/logging.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

namespace rex::patches {

namespace {

bool IsHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int HexVal(char c) {
  return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

std::optional<uint64_t> ParseHex64(std::string_view s) {
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
  if (s.empty() || s.size() > 16) return std::nullopt;
  uint64_t v = 0;
  for (char c : s) {
    if (!IsHex(c)) return std::nullopt;
    v = (v << 4) | uint64_t(HexVal(c));
  }
  return v;
}

template <typename T>
void PushBE(std::vector<uint8_t>& out, T v) {
  for (int i = int(sizeof(T)) - 1; i >= 0; --i) out.push_back(uint8_t(uint64_t(v) >> (i * 8)));
}

// UTF-8 to UTF-16 code units (invalid bytes become U+FFFD).
std::u16string Utf16(std::string_view s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    uint8_t c = uint8_t(s[i]);
    uint32_t cp = 0xFFFD;
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 1) {
      cp = c;
    } else if (n && i + n <= s.size()) {
      cp = c & (0x7F >> n);
      for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (uint8_t(s[i + k]) & 0x3F);
    } else {
      n = 1;
    }
    i += n;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(char16_t(0xD800 + (cp >> 10)));
      out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(char16_t(cp));
    }
  }
  return out;
}

// One write type of one patch. False = malformed (stop reading this patch's writes).
bool ReadWrites(const toml::table& patch, std::string_view type, Patch& out) {
  const auto* list = patch.get_as<toml::array>(type);
  if (!list) return true;
  for (const auto& node : *list) {
    const auto* t = node.as_table();
    if (!t) continue;
    const auto addr = t->get_as<int64_t>("address");
    const toml::node* value = t->get("value");
    if (!addr || !value) {
      out.parse_error = fmt::format("{} write without {}", type, addr ? "value" : "integer address");
      return false;
    }
    Write w;
    w.address = uint32_t(addr->get());
    auto ival = value->value_exact<int64_t>();
    auto sval = value->value_exact<std::string>();
    if (type.starts_with("be") && !ival) {
      out.parse_error = fmt::format("{} value at {:08X} is not an integer", type, w.address);
      return false;
    }
    if ((type == "string" || type == "u16string" || type == "array") && !sval) {
      out.parse_error = fmt::format("{} value at {:08X} is not a string", type, w.address);
      return false;
    }
    double fval = value->is_floating_point() ? value->as_floating_point()->get()
                  : value->is_integer()      ? double(value->as_integer()->get())
                                             : 0.0;
    if (type == "be8") {
      w.bytes.push_back(uint8_t(*ival));
    } else if (type == "be16") {
      PushBE(w.bytes, uint16_t(*ival));
    } else if (type == "be32") {
      PushBE(w.bytes, uint32_t(*ival));
    } else if (type == "be64") {
      PushBE(w.bytes, uint64_t(*ival));
    } else if (type == "f32") {
      float f = float(fval);
      uint32_t u;
      std::memcpy(&u, &f, 4);
      PushBE(w.bytes, u);
    } else if (type == "f64") {
      uint64_t u;
      std::memcpy(&u, &fval, 8);
      PushBE(w.bytes, u);
    } else if (type == "string") {
      w.bytes.assign(sval->begin(), sval->end());
    } else if (type == "u16string") {
      for (char16_t c : Utf16(*sval)) {
        w.bytes.push_back(uint8_t(c));  // host (little-endian) order, as canary writes it
        w.bytes.push_back(uint8_t(c >> 8));
      }
    } else {  // array
      std::string_view h = *sval;
      if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h.remove_prefix(2);
      if (h.size() % 2 || !std::all_of(h.begin(), h.end(), IsHex)) {
        out.parse_error = fmt::format("array value at {:08X} is not hex bytes", w.address);
        return false;
      }
      for (size_t i = 0; i < h.size(); i += 2) w.bytes.push_back(uint8_t(HexVal(h[i]) << 4 | HexVal(h[i + 1])));
    }
    out.writes.push_back(std::move(w));
  }
  return true;
}

// ---- runtime state ----------------------------------------------------------

struct CodeRange {
  uint32_t begin, end;
};

struct Module {
  std::string name;
  uint32_t title_id = 0;
  uint64_t hash = 0;
  std::vector<CodeRange> code;
};

struct Live {
  size_t module = 0;  // index into modules (matched files only)
  const File* file = nullptr;
  const Patch* patch = nullptr;
  bool matched = false;
  Kind kind = Kind::kData;
  std::atomic<bool>* flag = nullptr;  // built-in code patch switch
  std::string reason;
  bool on = false;
  std::vector<std::vector<uint8_t>> saved;  // bytes under each write while on
};

struct BuiltIn {
  uint32_t title_id;
  uint64_t hash;
  std::string name;
  std::atomic<bool>* flag;
};

struct State {
  std::mutex mutex;
  bool configured = false;
  bool apply = true;
  std::vector<std::filesystem::path> dirs;
  std::filesystem::path state_dir;
  memory::Memory* memory = nullptr;
  std::vector<std::unique_ptr<File>> files;
  std::vector<Module> modules;
  std::vector<std::unique_ptr<Live>> live;
};

State& S() {
  static State* s = new State();
  return *s;
}
std::vector<BuiltIn>& BuiltIns() {
  static auto* v = new std::vector<BuiltIn>();
  return *v;
}
std::mutex& BuiltInMutex() {
  static auto* m = new std::mutex();
  return *m;
}

std::string FileName(const std::string& path) {
  return std::filesystem::path(path).filename().string();
}

std::filesystem::path StatePath(const Module& m) {
  return S().state_dir / fmt::format("{:08X}-{:016X}.txt", m.title_id, m.hash);
}

// Saved choices: "on|off<TAB>file<TAB>patch name".
std::map<std::string, bool> ReadChoices(const Module& m) {
  std::map<std::string, bool> out;
  std::ifstream in(StatePath(m));
  std::string line;
  while (std::getline(in, line)) {
    auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    out[line.substr(tab + 1)] = line.compare(0, tab, "on") == 0;
  }
  return out;
}

std::string ChoiceKey(const Live& l) {
  return FileName(l.file->path) + "\t" + l.patch->name;
}

void SaveChoices(size_t module) {
  auto& s = S();
  if (s.state_dir.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(s.state_dir, ec);
  std::ofstream out(StatePath(s.modules[module]), std::ios::trunc);
  for (const auto& l : s.live) {
    if (!l->matched || l->module != module || l->reason.size()) continue;
    if (l->on != l->patch->default_on) out << (l->on ? "on\t" : "off\t") << ChoiceKey(*l) << "\n";
  }
}

bool Committed(memory::Memory* mem, uint32_t addr, size_t size) {
  for (uint32_t a : {addr, uint32_t(addr + (size ? size - 1 : 0))}) {
    auto* heap = mem->LookupHeap(a);
    memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(a, &info) || !(info.state & memory::kMemoryAllocationCommit)) return false;
  }
  return true;
}

void WriteAll(memory::Memory* mem, const Patch& p) {
  for (const auto& w : p.writes) std::memcpy(mem->TranslateVirtual(w.address), w.bytes.data(), w.bytes.size());
}

bool Apply(Live& l, bool on, std::string* why) {
  auto& s = S();
  if (!l.reason.empty()) {
    if (why) *why = l.reason;
    return false;
  }
  if (on == l.on) return true;
  if (on) {
    for (const auto& w : l.patch->writes) {
      if (!Committed(s.memory, w.address, w.bytes.size())) {
        if (why) *why = fmt::format("address {:08X} is not mapped", w.address);
        return false;
      }
    }
    l.saved.clear();
    for (const auto& w : l.patch->writes) {
      const uint8_t* p = s.memory->TranslateVirtual(w.address);
      l.saved.emplace_back(p, p + w.bytes.size());
    }
    WriteAll(s.memory, *l.patch);
  } else {
    for (size_t i = l.patch->writes.size(); i-- > 0;) {
      const auto& w = l.patch->writes[i];
      std::memcpy(s.memory->TranslateVirtual(w.address), l.saved[i].data(), l.saved[i].size());
    }
    // shortcut: overlapping patches are re-written over the restored bytes, their own saved bytes are kept.
    for (const auto& other : s.live) {
      if (other.get() != &l && other->on) WriteAll(s.memory, *other->patch);
    }
  }
  if (l.flag) l.flag->store(on, std::memory_order_release);
  l.on = on;
  return true;
}

void LoadFiles() {
  auto& s = S();
  s.files.clear();
  for (const auto& dir : s.dirs) {
    std::error_code ec;
    std::vector<std::filesystem::path> paths;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) paths.push_back(e.path());
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
      if (!IsPatchFileName(path.filename().string())) continue;
      std::ifstream in(path, std::ios::binary);
      std::stringstream text;
      text << in.rdbuf();
      std::string error;
      auto file = ParseFile(text.str(), &error);
      if (!file) {
        REXLOG_WARN("[patches] skipped {}: {}", path.string(), error);
        continue;
      }
      file->path = path.string();
      s.files.push_back(std::make_unique<File>(std::move(*file)));
    }
  }
  REXLOG_INFO("[patches] {} patch files in {} folders", s.files.size(), s.dirs.size());
}

}  // namespace

bool IsPatchFileName(std::string_view n) {
  if (n.size() < 8 + 11 || !n.ends_with(".patch.toml")) return false;
  return std::all_of(n.begin(), n.begin() + 8, IsHex);
}

std::optional<File> ParseFile(std::string_view text, std::string* error) {
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error& e) {
    if (error) *error = std::string(e.description());
    return std::nullopt;
  }
  File f;
  auto name = root.get_as<std::string>("title_name");
  auto id = root.get_as<std::string>("title_id");
  const toml::node* hash = root.get("hash");
  if (!name || !id || !hash) {
    if (error) *error = "title_name, title_id and hash are required";
    return std::nullopt;
  }
  f.title_name = name->get();
  f.title_id = uint32_t(std::strtoul(id->get().c_str(), nullptr, 16));
  auto add_hash = [&](const toml::node& n) {
    if (auto v = n.value_exact<std::string>(); v && !v->empty()) {
      if (auto h = ParseHex64(*v)) f.hashes.push_back(*h);
    }
  };
  if (const auto* arr = hash->as_array()) {
    for (const auto& n : *arr) add_hash(n);
  } else {
    add_hash(*hash);
  }
  if (const auto* patches = root.get_as<toml::array>("patch")) {
    for (const auto& node : *patches) {
      const auto* t = node.as_table();
      if (!t) continue;
      Patch p;
      p.name = (*t)["name"].value_exact<std::string>().value_or("");
      p.desc = (*t)["desc"].value_exact<std::string>().value_or("");
      p.author = (*t)["author"].value_exact<std::string>().value_or("");
      p.default_on = (*t)["is_enabled"].value_exact<bool>().value_or(false);
      for (std::string_view type : {"array", "be16", "be32", "be64", "be8", "f32", "f64", "string", "u16string"}) {
        if (!ReadWrites(*t, type, p)) break;
      }
      f.patches.push_back(std::move(p));
    }
  }
  return f;
}

uint64_t HashCode(const uint8_t* bytes, size_t size) {
  return XXH3_64bits(bytes, size);
}

void Configure(std::vector<std::filesystem::path> dirs, std::filesystem::path state_dir, bool apply) {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  s.apply = apply;
  s.dirs = std::move(dirs);
  s.state_dir = std::move(state_dir);
  s.configured = true;
  LoadFiles();
}

bool Configured() {
  std::lock_guard lock(S().mutex);
  return S().configured;
}

void OnModuleLoaded(memory::Memory* mem, const runtime::XexModule& xex, uint32_t title_id) {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  if (!s.configured) return;
  s.memory = mem;

  const uint32_t base = xex.base_address();
  const auto* heap = mem->LookupHeap(base);
  const auto* sec = xex.xex_security_info();
  if (!heap || !sec) return;
  const uint32_t page = heap->page_size();

  Module m;
  m.name = xex.name();
  m.title_id = title_id;
  int first = -1, last = -1;
  uint32_t at = base;
  for (uint32_t i = 0; i < sec->page_descriptor_count; ++i) {
    xex2_page_descriptor d;
    d.value = rex::byte_swap(sec->page_descriptors[i].value);
    const uint32_t size = d.page_count * page;
    if (d.info == XEX_SECTION_CODE) {
      if (first < 0) first = int(i);
      last = int(i);
      m.code.push_back({at, at + size});
    }
    at += size;
  }
  if (first < 0) return;
  {
    // Canary's range (see file comment), on a copy with canary's import thunk stubs.
    const uint32_t begin = base + uint32_t(first) * page, end = base + uint32_t(last + 1) * page;
    const uint8_t* src = mem->TranslateVirtual(begin);
    std::vector<uint8_t> copy(src, src + (end - begin));
    static constexpr uint8_t kStub[16] = {0x44, 0, 0, 0x42, 0x4E, 0x80, 0, 0x20, 0x60, 0, 0, 0, 0x60, 0, 0, 0};
    for (const auto& lib : *xex.import_libraries()) {
      for (const auto& imp : lib.imports) {
        if (imp.thunk_address >= begin && imp.thunk_address + 16 <= end) {
          std::memcpy(copy.data() + (imp.thunk_address - begin), kStub, 16);
        }
        // Canary marks a function import's variable record 0xDEADC0DE.
        if (imp.thunk_address && imp.value_address >= begin && imp.value_address + 4 <= end) {
          static constexpr uint8_t kDead[4] = {0xDE, 0xAD, 0xC0, 0xDE};
          std::memcpy(copy.data() + (imp.value_address - begin), kDead, 4);
        }
      }
    }
    m.hash = HashCode(copy.data(), copy.size());
  }
  REXLOG_INFO("[patches] module {} title {:08X} hash {:016X}", m.name, m.title_id, m.hash);
  const size_t mi = s.modules.size();
  s.modules.push_back(m);
  const auto choices = ReadChoices(m);

  auto in_code = [&](const Write& w) {
    for (const auto& r : m.code) {
      if (w.address < r.end && w.address + w.bytes.size() > r.begin) return true;
    }
    return false;
  };

  std::vector<BuiltIn> built;
  {
    std::lock_guard bl(BuiltInMutex());
    built = BuiltIns();
  }
  size_t applied = 0, listed = 0;
  for (const auto& f : s.files) {
    if (f->title_id != title_id) continue;
    const bool matched = std::find(f->hashes.begin(), f->hashes.end(), m.hash) != f->hashes.end();
    for (const auto& p : f->patches) {
      auto l = std::make_unique<Live>();
      l->module = mi;
      l->file = f.get();
      l->patch = &p;
      l->matched = matched;
      l->kind = std::any_of(p.writes.begin(), p.writes.end(), in_code) ? Kind::kCode : Kind::kData;
      if (!matched) {
        l->reason = fmt::format("Made for another version of the game (this one is {:016X}).", m.hash);
      } else if (p.writes.empty()) {
        l->reason = p.parse_error.empty() ? "The patch has no writes." : "Malformed: " + p.parse_error + ".";
      } else if (l->kind == Kind::kCode) {
        for (const auto& b : built) {
          if (b.title_id == title_id && b.hash == m.hash && b.name == p.name) l->flag = b.flag;
        }
        if (!l->flag && s.apply) {
          l->reason = "Changes game code; this build does not include it (rebuild with the patch).";
        }
      }
      ++listed;
      Live& ref = *l;
      s.live.push_back(std::move(l));
      if (!ref.reason.empty() || !s.apply) continue;
      auto c = choices.find(ChoiceKey(ref));
      const bool want = c != choices.end() ? c->second : p.default_on;
      std::string why;
      if (want && Apply(ref, true, &why)) {
        ++applied;
        REXLOG_INFO("[patches] on at load: {}", p.name);
      } else if (want) {
        REXLOG_WARN("[patches] could not apply {}: {}", p.name, why);
      }
    }
  }
  if (listed) REXLOG_INFO("[patches] {} patches listed, {} on", listed, applied);
}

std::vector<Entry> List() {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<Entry> out;
  for (size_t i = 0; i < s.live.size(); ++i) {
    const Live& l = *s.live[i];
    Entry e;
    e.id = i;
    e.title = l.patch->name.empty() ? "(unnamed patch)" : l.patch->name;
    e.desc = l.patch->desc;
    e.author = l.patch->author;
    e.file = FileName(l.file->path);
    e.kind = l.kind;
    e.available = l.reason.empty();
    e.built_in = l.flag != nullptr;
    e.on = l.on;
    e.reason = l.reason;
    out.push_back(std::move(e));
  }
  // Usable first, then the file order.
  std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.available > b.available; });
  return out;
}

bool SetEnabled(size_t id, bool on, std::string* why) {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  if (id >= s.live.size()) return false;
  Live& l = *s.live[id];
  if (!Apply(l, on, why)) return false;
  REXLOG_INFO("[patches] {} {}", l.patch->name, on ? "on" : "off");
  SaveChoices(l.module);
  return true;
}

std::vector<std::string> ModuleSummary() {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<std::string> out;
  for (const auto& m : s.modules) out.push_back(fmt::format("{} {:08X} {:016X}", m.name, m.title_id, m.hash));
  return out;
}

std::vector<BuildPatch> CodePatchesForBuild() {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<BuildPatch> out;
  for (const auto& l : s.live) {
    if (!l->matched || l->kind != Kind::kCode || l->patch->writes.empty()) continue;
    const Module& m = s.modules[l->module];
    BuildPatch b{l->patch->name, m.title_id, m.hash, {}};
    for (const auto& w : l->patch->writes) {
      for (size_t i = 0; i < w.bytes.size(); ++i) {
        const uint32_t a = w.address + uint32_t(i), word = a & ~3u;
        bool code = false;
        for (const auto& r : m.code) code |= word >= r.begin && word < r.end;
        if (!code) continue;  // the data part is written by Apply at run time
        auto it = b.words.find(word);
        if (it == b.words.end()) {
          const uint8_t* p = s.memory->TranslateVirtual(word);
          it = b.words.emplace(word, uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]).first;
        }
        const int shift = 8 * (3 - int(a & 3));
        it->second = (it->second & ~(0xFFu << shift)) | uint32_t(w.bytes[i]) << shift;
      }
    }
    out.push_back(std::move(b));
  }
  return out;
}

void RegisterBuiltIn(uint32_t title_id, uint64_t module_hash, std::string_view patch_name, std::atomic<bool>* flag) {
  std::lock_guard lock(BuiltInMutex());
  BuiltIns().push_back({title_id, module_hash, std::string(patch_name), flag});
}

}  // namespace rex::patches
