/**
 * @file        ui/overlay/guide_pages.cpp
 * @brief       Guide page registry and item builders. See rex/ui/guide.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/guide.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace rex::ui::guide {

namespace {

std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<Page>& Registry() {
  static std::vector<Page> pages;
  return pages;
}

int BladeRank(std::string_view blade) {
  if (blade == kBladeGame) return 0;
  if (blade == kBladeSystem) return 100;
  if (blade == kBladeControllers) return 200;
  if (blade == kBladeLeave) return 300;
  return 50;
}

bool CvarExists(const std::string& name) {
  return rex::cvar::GetFlagInfo(name) != nullptr;
}

std::string CvarLabel(const std::string& name, std::string label) {
  if (!label.empty()) return label;
  std::string out = name;
  std::replace(out.begin(), out.end(), '_', ' ');
  if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}

std::string CvarDescription(const std::string& name, std::string description) {
  if (!description.empty()) return description;
  const auto* info = rex::cvar::GetFlagInfo(name);
  if (!info) return {};
  std::string out = info->description;
  if (info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart) {
    out += " (takes effect after a restart)";
  }
  return out;
}

void SetCvar(const std::string& name, const std::string& label, const std::string& value) {
  if (rex::cvar::SetFlagByName(name, value)) {
    LogChange(label, value);
  } else {
    REXLOG_WARN("[guide] {} = {} refused by cvar {}", label, value, name);
  }
}

bool ParseNumber(const std::string& text, double& out) {
  char* end = nullptr;
  out = std::strtod(text.c_str(), &end);
  return end && end != text.c_str();
}

std::string FormatNumber(double value, bool integral) {
  if (integral) return std::to_string(static_cast<long long>(std::llround(value)));
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", value);
  return buf;
}

}  // namespace

void LogChange(std::string_view label, std::string_view value) {
  REXLOG_INFO("[guide] {} = {}", label, value);
}

Item Toggle(std::string label, std::function<bool()> get, std::function<void(bool)> set,
            std::string description) {
  Item item;
  item.type = ItemType::kToggle;
  item.label = std::move(label);
  item.description = std::move(description);
  item.get_bool = std::move(get);
  item.set_bool = std::move(set);
  return item;
}

Item Choice(std::string label, std::vector<std::string> choices, std::function<int()> get,
            std::function<void(int)> set, std::string description) {
  Item item;
  item.type = ItemType::kChoice;
  item.label = std::move(label);
  item.description = std::move(description);
  item.choices = std::move(choices);
  item.get_index = std::move(get);
  item.set_index = std::move(set);
  return item;
}

Item Slider(std::string label, double min, double max, double step, std::function<double()> get,
            std::function<void(double)> set, std::string description, std::string format) {
  Item item;
  item.type = ItemType::kSlider;
  item.label = std::move(label);
  item.description = std::move(description);
  item.min = min;
  item.max = max;
  item.step = step > 0 ? step : 1.0;
  item.format = std::move(format);
  item.get_value = std::move(get);
  item.set_value = std::move(set);
  return item;
}

Item Action(std::string label, std::function<void()> run, std::string description) {
  Item item;
  item.type = ItemType::kAction;
  item.label = std::move(label);
  item.description = std::move(description);
  item.action = std::move(run);
  return item;
}

Item Info(std::string label, std::function<std::string()> text, std::string description) {
  Item item;
  item.type = ItemType::kInfo;
  item.label = std::move(label);
  item.description = std::move(description);
  item.text = std::move(text);
  return item;
}

Item CvarToggle(std::string name, std::string label, std::string description) {
  label = CvarLabel(name, std::move(label));
  Item item = Toggle(
      label, [name] { return rex::cvar::GetFlagByName(name) == "true"; },
      [name, label](bool on) { SetCvar(name, label, on ? "true" : "false"); },
      CvarDescription(name, std::move(description)));
  item.cvar = name;
  item.visible = [name] { return CvarExists(name); };
  return item;
}

Item CvarChoice(std::string name, std::vector<std::string> values, std::vector<std::string> labels,
                std::string label, std::string description) {
  label = CvarLabel(name, std::move(label));
  if (labels.size() != values.size()) labels = values;
  Item item = Choice(
      label, labels,
      [name, values] {
        const std::string current = rex::cvar::GetFlagByName(name);
        for (size_t i = 0; i < values.size(); ++i) {
          if (values[i] == current) return static_cast<int>(i);
        }
        return -1;
      },
      [name, values, label](int index) {
        if (index >= 0 && static_cast<size_t>(index) < values.size()) {
          SetCvar(name, label, values[static_cast<size_t>(index)]);
        }
      },
      CvarDescription(name, std::move(description)));
  item.cvar = name;
  item.visible = [name] { return CvarExists(name); };
  return item;
}

Item CvarSlider(std::string name, double min, double max, double step, std::string label,
                std::string description, std::string format) {
  label = CvarLabel(name, std::move(label));
  const bool integral = std::floor(step) == step;
  Item item = Slider(
      label, min, max, step,
      [name] {
        double v = 0;
        ParseNumber(rex::cvar::GetFlagByName(name), v);
        return v;
      },
      [name, label, integral](double v) { SetCvar(name, label, FormatNumber(v, integral)); },
      CvarDescription(name, std::move(description)), std::move(format));
  item.cvar = name;
  item.visible = [name] { return CvarExists(name); };
  return item;
}

Item Cvar(std::string name, std::string label, std::string description) {
  const auto* info = rex::cvar::GetFlagInfo(name);
  if (!info) {
    // Hidden until (unless) the cvar shows up; Info keeps the type harmless.
    Item item = Info(CvarLabel(name, std::move(label)), [] { return std::string(); });
    item.cvar = name;
    item.visible = [name] { return CvarExists(name); };
    return item;
  }
  using rex::cvar::FlagType;
  if (info->type == FlagType::Boolean) {
    return CvarToggle(std::move(name), std::move(label), std::move(description));
  }
  if (info->constraints.HasAllowedValues()) {
    return CvarChoice(std::move(name), info->constraints.allowed_values, {}, std::move(label),
                      std::move(description));
  }
  const bool numeric = info->type != FlagType::String && info->type != FlagType::Command;
  if (numeric && info->constraints.min && info->constraints.max) {
    const bool integral = info->type != FlagType::Double;
    return CvarSlider(std::move(name), *info->constraints.min, *info->constraints.max,
                      integral ? 1.0 : (*info->constraints.max - *info->constraints.min) / 20.0,
                      std::move(label), std::move(description), integral ? "%.0f" : "%.2f");
  }
  label = CvarLabel(name, std::move(label));
  Item item = Info(label, [name] { return rex::cvar::GetFlagByName(name); },
                   CvarDescription(name, std::move(description)));
  item.cvar = name;
  item.visible = [name] { return CvarExists(name); };
  return item;
}

void RegisterPage(Page page) {
  std::lock_guard lock(RegistryMutex());
  auto& pages = Registry();
  auto it = std::find_if(pages.begin(), pages.end(), [&](const Page& p) { return p.id == page.id; });
  if (it != pages.end()) {
    *it = std::move(page);
  } else {
    pages.push_back(std::move(page));
  }
}

void UnregisterPage(std::string_view id) {
  std::lock_guard lock(RegistryMutex());
  auto& pages = Registry();
  pages.erase(std::remove_if(pages.begin(), pages.end(), [&](const Page& p) { return p.id == id; }),
              pages.end());
}

std::vector<Page> Pages() {
  std::vector<Page> out;
  {
    std::lock_guard lock(RegistryMutex());
    out = Registry();
  }
  std::stable_sort(out.begin(), out.end(), [](const Page& a, const Page& b) {
    const int ra = BladeRank(a.blade), rb = BladeRank(b.blade);
    if (ra != rb) return ra < rb;
    if (a.blade != b.blade) return a.blade < b.blade;
    return a.order < b.order;
  });
  return out;
}

}  // namespace rex::ui::guide
