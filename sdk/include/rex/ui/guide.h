/**
 * @file        rex/ui/guide.h
 * @brief       In-game Guide (Xbox button menu): page registration API.
 *
 * @copyright   Copyright (c) 2026
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * The Guide is a Blades-style overlay that opens on the controller's Guide
 * button (or F1 / Home). It shows blades (tabs); each blade lists the pages
 * registered into it, and each page holds items. The core fills the System,
 * Controllers and Leave Game blades; a game adds its own pages, usually to the
 * "Game" blade, at startup:
 *
 *   namespace guide = rex::ui::guide;
 *   guide::Page page;
 *   page.id = "mygame.settings";
 *   page.title = "My Game";
 *   page.items = {
 *       guide::Toggle("Show FPS", [] { return g_show_fps; }, [](bool on) { g_show_fps = on; },
 *                     "Draws the frame rate in the corner."),
 *       guide::Choice("Quality", {"Low", "High"}, [] { return g_q; }, [](int i) { g_q = i; }),
 *       guide::Cvar("vsync"),                       // type taken from the cvar
 *       guide::Action("Max points", [] { GivePoints(); }).InGroup("Cheats"),
 *   };
 *   guide::RegisterPage(std::move(page));
 *
 * Threading: getters, setters and actions run on the UI thread, once per drawn
 * frame for getters. Hand anything that must touch guest state over to the
 * game's own thread (a queue drained each game frame works well).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::ui::guide {

enum class ItemType { kToggle, kChoice, kSlider, kAction, kInfo };

struct Item {
  ItemType type = ItemType::kInfo;
  std::string label;
  // Shown in the description pane when the item is highlighted.
  std::string description;
  // Items of one page with the same group are listed under that group's own
  // entry in the blade (e.g. "Cheats"). Empty: the page title.
  std::string group;

  // kToggle
  std::function<bool()> get_bool;
  std::function<void(bool)> set_bool;
  // kChoice: choices are the shown names; get/set use indices into it.
  std::vector<std::string> choices;
  std::function<int()> get_index;
  std::function<void(int)> set_index;
  // kSlider
  double min = 0.0;
  double max = 1.0;
  double step = 1.0;
  std::string format = "%.0f";  // printf format for the value
  std::function<double()> get_value;
  std::function<void(double)> set_value;
  // kAction: run on A. Text the action shows as its value ("Run" when empty).
  std::function<void()> action;
  // kInfo (and optional value text for kAction)
  std::function<std::string()> text;

  // Optional: greyed out and not selectable when this returns false.
  std::function<bool()> enabled;
  // Optional: the item is left out entirely when this returns false.
  std::function<bool()> visible;
  // Set by the cvar builders: the cvar the item is bound to.
  std::string cvar;

  Item&& InGroup(std::string name) && {
    group = std::move(name);
    return std::move(*this);
  }
  Item&& Describe(std::string text_) && {
    description = std::move(text_);
    return std::move(*this);
  }
  Item&& EnabledWhen(std::function<bool()> fn) && {
    enabled = std::move(fn);
    return std::move(*this);
  }
  Item&& VisibleWhen(std::function<bool()> fn) && {
    visible = std::move(fn);
    return std::move(*this);
  }
};

// ---- Item builders bound to getters/setters --------------------------------
Item Toggle(std::string label, std::function<bool()> get, std::function<void(bool)> set,
            std::string description = {});
Item Choice(std::string label, std::vector<std::string> choices, std::function<int()> get,
            std::function<void(int)> set, std::string description = {});
Item Slider(std::string label, double min, double max, double step, std::function<double()> get,
            std::function<void(double)> set, std::string description = {},
            std::string format = "%.0f");
Item Action(std::string label, std::function<void()> run, std::string description = {});
Item Info(std::string label, std::function<std::string()> text, std::string description = {});

// ---- Item builders bound to a cvar (REXCVAR_*) -----------------------------
// The item reads and writes the cvar by name. When the cvar does not exist the
// item is hidden (so one declarative list can name cvars only some games have).
// label/description default to the cvar's name/description.
//
// Cvar(): the type comes from the cvar: bool -> toggle, allowed values ->
// choice, numeric with a range -> slider, anything else -> info.
Item Cvar(std::string name, std::string label = {}, std::string description = {});
Item CvarToggle(std::string name, std::string label = {}, std::string description = {});
// values: what is written to the cvar; labels: what is shown (defaults to values).
Item CvarChoice(std::string name, std::vector<std::string> values,
                std::vector<std::string> labels = {}, std::string label = {},
                std::string description = {});
Item CvarSlider(std::string name, double min, double max, double step, std::string label = {},
                std::string description = {}, std::string format = "%.0f");

// ---- Pages -----------------------------------------------------------------
// Built-in blade names. Any other name adds a new blade (after Game).
inline constexpr std::string_view kBladeGame = "Game";
inline constexpr std::string_view kBladeSystem = "System";
inline constexpr std::string_view kBladeControllers = "Controllers";
inline constexpr std::string_view kBladeLeave = "Leave Game";

struct Page {
  std::string id;     // unique; registering the same id again replaces the page
  std::string title;  // entry name in the blade
  std::string blade = std::string(kBladeGame);
  int order = 0;      // lower first, within the blade
  std::string description;  // shown when the page entry is highlighted
  std::vector<Item> items;
  // Optional: called every frame the page is shown, instead of items (for
  // lists that change, like connected controllers).
  std::function<std::vector<Item>()> build;
};

void RegisterPage(Page page);
void UnregisterPage(std::string_view id);
// Snapshot of the registered pages, sorted by blade order then page order.
std::vector<Page> Pages();

// Logs "[guide] <text>" at INFO. Items the core changes log through this, so
// tests can see what the Guide did.
void LogChange(std::string_view label, std::string_view value);

}  // namespace rex::ui::guide
