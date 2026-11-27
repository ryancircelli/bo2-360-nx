/**
 * @file        system/live_control.cpp
 * @brief       Commands for a running test game (see rex/system/live_control.h)
 */

#include <rex/system/live_control.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/input_record.h>
#include <rex/input/script/script_input_driver.h>
#include <rex/system/patch.h>
#include <rex/system/savepoint.h>

namespace rex::system {
namespace {

std::string Trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::pair<std::string, std::string> Split(const std::string& s) {
  const std::string t = Trim(s);
  const size_t sp = t.find_first_of(" \t");
  if (sp == std::string::npos) return {t, {}};
  return {t.substr(0, sp), Trim(t.substr(sp))};
}

std::string Cvar(const std::string& args) {
  auto [name, value] = Split(args);
  if (name.empty()) return "error usage: cvar NAME [VALUE]";
  const auto* info = cvar::GetFlagInfo(name);
  if (!info) return "error unknown cvar " + name;
  if (info->type == cvar::FlagType::Command) return "error " + name + " is a command (use: cmd " + name + ")";
  if (value.empty()) return "ok " + name + " = " + cvar::GetFlagByName(name);
  if (info->lifecycle == cvar::Lifecycle::kInitOnly) return "error " + name + " is init-only (needs a restart)";
  if (!cvar::SetFlagByName(name, value)) return "error cannot set " + name + " to " + value;
  std::string note = info->lifecycle == cvar::Lifecycle::kRequiresRestart ? " (takes effect after a restart)" : "";
  return "ok " + name + " = " + cvar::GetFlagByName(name) + note;
}

std::string Cmd(const std::string& args) {
  auto [name, rest] = Split(args);
  if (name.empty()) return "error usage: cmd NAME [ARGS]";
  const auto* info = cvar::GetFlagInfo(name);
  if (!info || info->type != cvar::FlagType::Command) return "error unknown command " + name;
  return cvar::InvokeCommand(name, rest) ? "ok ran " + name : "error " + name + " failed";
}

std::string PatchStatus() {
  auto& table = patch::SlotTable::Get();
  return "ok version " + std::to_string(patch::CommittedPatchVersion()) + " slots " + std::to_string(table.size()) +
         " patched " + std::to_string(table.PatchedCount());
}

std::string Patch(const std::string& args) {
  const std::string arg = Trim(args);
  if (arg.empty() || arg == "status") return PatchStatus();
  if (arg == "off") {
    patch::SlotTable::Get().ResetAll();
    return "ok all slots original; " + PatchStatus().substr(3);
  }
  const auto started = std::chrono::steady_clock::now();
  patch::LoadResult r = patch::PreparePatchLibrary(arg);
  if (!r.ok) return "error " + r.error + " (old code kept)";
  // Bound at the next frame boundary (graphics swap); a paused or hung game never commits.
  while (patch::CommittedPatchVersion() < r.version) {
    if (std::chrono::steady_clock::now() - started > std::chrono::seconds(10)) {
      return "error prepared version " + std::to_string(r.version) + " but no frame committed it within 10 s";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  std::ostringstream out;
  out << "ok version " << r.version << " applied " << r.applied << " unmatched " << r.unmatched.size();
  for (size_t i = 0; i < r.unmatched.size() && i < 8; ++i) out << (i ? "," : " (") << r.unmatched[i];
  if (!r.unmatched.empty()) out << (r.unmatched.size() > 8 ? ",...)" : ")");
  out << " bound_ms " << static_cast<int>(ms) << " slots " << patch::SlotTable::Get().size();
  return out.str();
}

std::string Slots(const std::string& args) {
  const auto list = patch::SlotTable::Get().List(Trim(args), 200);
  std::ostringstream out;
  out << "ok " << patch::SlotTable::Get().size() << " slots;";
  for (const auto* s : list) {
    out << ' ' << s->name << (s->current.load(std::memory_order_relaxed) != s->original ? "*" : "");
  }
  return out.str();
}

std::string Timescale(const std::string& args) {
  const std::string arg = Trim(args);
  if (!arg.empty()) {
    char* end = nullptr;
    const double v = std::strtod(arg.c_str(), &end);
    if (!end || *end || v < 0.1 || v > 16.0) return "error usage: timescale X (0.1..16)";
    chrono::Clock::set_guest_time_scalar(static_cast<double>(static_cast<int>(v * 10.0 + 0.5)) / 10.0);
  }
  std::ostringstream out;
  out << "ok timescale " << chrono::Clock::guest_time_scalar();
  return out.str();
}

std::string Savepoint(const std::string& args) {
  // Same entry as the Metal test control: save | load | save PATH | file PATH.
  return ControlInProcessSavepoint(args, 100000);
}

// record PATH: every pad input since start (routes and live presses alike) in the hid_replay_input format.
std::string Record(const std::string& args) {
  const std::string path = Trim(args);
  if (path.empty()) return "error usage: record PATH";
  const auto samples = input::GlobalInputRing().Window(input::ProcessMs(), input::ProcessMs());
  std::ofstream out(path, std::ios::trunc);
  out << input::SamplesToText(samples);
  if (!out) return "error cannot write " + path;
  // The file's times start at its first sample: first_ms is when that was (hid_replay_delay for the replay).
  return "ok recorded " + std::to_string(samples.size()) + " samples to " + path + " first_ms " +
         std::to_string(samples.empty() ? 0 : samples.front().ms);
}

// record PATH: every pad input since start (routes and live presses alike) in the hid_replay_input format.
std::string Record(const std::string& args) {
  const std::string path = Trim(args);
  if (path.empty()) return "error usage: record PATH";
  const auto samples = input::GlobalInputRing().Window(input::ProcessMs(), input::ProcessMs());
  std::ofstream out(path, std::ios::trunc);
  out << input::SamplesToText(samples);
  if (!out) return "error cannot write " + path;
  // The file's times start at its first sample: first_ms is when that was (hid_replay_delay for the replay).
  return "ok recorded " + std::to_string(samples.size()) + " samples to " + path + " first_ms " +
         std::to_string(samples.empty() ? 0 : samples.front().ms);
}

}  // namespace

std::string LiveControlCommand(const std::string& command, const std::string& args) {
  if (command == "cvar") return Cvar(args);
  if (command == "cmd") return Cmd(args);
  if (command == "patch") return Patch(args);
  if (command == "slots") return Slots(args);
  if (command == "timescale") return Timescale(args);
  if (command == "savepoint") return Savepoint(args);
  if (command == "record") return Record(args);
#if !REX_PLATFORM_SWITCH
  if (std::string pad = input::script::LivePadCommand(command, args); !pad.empty()) return pad;
#endif
  return {};
}

}  // namespace rex::system
