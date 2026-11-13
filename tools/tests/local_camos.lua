-- Authored stock-menu semantics exercised against the actual overrides; no game data is loaded.
-- Run with Lua 5.1 from the repository root, optionally passing a different menus.lua path.
local source = arg[1] or "games/bo2/src/ui/menus.lua"
local ready, sessionMode, gameMode = 1, 0, 2
local owned, hidden, updates, purchases, writes, logs = {}, {}, 0, 0, {}, {}
local selections = 0
local methods = {}
local function element()
  return setmetatable({children = {}, m_eventHandlers = {}}, {__index = methods})
end
function methods:getFirstChild() return self.children[1] end
function methods:getNextSibling()
  for i, child in ipairs(self.parent.children) do
    if child == self then return self.parent.children[i + 1] end
  end
end
function methods:addElement(child)
  child.parent = self
  self.children[#self.children + 1] = child
end
function methods:registerEventHandler(name, handler) self.m_eventHandlers[name] = handler end
function methods:setInactive(value) self.inactive = value end
function methods:setHintText(text, image) self.hintText, self.hintImage = text, image end
function methods:setActionPromptString(text) self.actionPromptString = text end
function methods:setLocked(locked)
  if locked then
    self.lockImage, self.inactive, self.actionPromptString = {}, true, nil
  else
    -- Stock unlock removes the lock presentation, but leaves inactive for its caller to change.
    self.lockImage = nil
  end
end

Engine = {
  ExecNow = function(_, command) logs[#logs + 1] = command end,
  Localize = function(key) return key end,
  SessionModeIsMode = function(mode) return sessionMode == mode end,
  GameModeIsMode = function(mode) return gameMode == mode end,
  SkipMTXItem = function(name) return hidden[name] ~= false end,
  HasMTX = function(controller, name) return owned[controller .. ":" .. name] == true end,
  IsSignedInToLive = function() return false end,
  PurchaseMTX = function() purchases = purchases + 1 end,
  GetItemOptionLocked = function() return true end,
  SetClassItem = function(controller, class, slot, option)
    writes[#writes + 1] = {controller, class, slot, option}
  end,
}
UIExpression = {
  DvarInt = function(_, name) return name == "bo2mp_local_camos_ui" and ready or 0 end,
}
CoD = {
  isMultiplayer = true, isSinglePlayer = false, isZombie = false,
  SESSIONMODE_OFFLINE = 0, GAMEMODE_LOCAL_SPLITSCREEN = 2,
  fonts = {Condensed = "font"},
  ContentGridButton = {}, CACUtility = {}, CACCamo = {}, CamoGridButton = {}, perController = {},
}
LUI = {createMenu = {}, roots = {UIRoot0 = element()}}
require = function() return "required", nil, "third" end

-- This is the expected TU18 inventory, kept independent of the implementation's table.
local options = {
  {19, 45, 0}, {20, 46, 1}, {21, 47, 2}, {22, 48, 3}, {23, 49, 4},
  {24, 50, 5}, {25, 51, 6}, {26, 52, 7}, {27, 53, 8}, {28, 54, 9},
  {30, 66, 10}, {31, 67, 11}, {32, 68, 12}, {33, 69, 13},
  {34, 74, 14}, {35, 75, 15}, {36, 76, 16}, {37, 77, 17}, {38, 78, 18}, {39, 79, 19},
  {40, 91, 20}, {41, 92, 21}, {42, 93, 22}, {43, 94, 23}, {44, 96, 24},
}
local ordinalToOption, names, sortKeys = {[16] = 16}, {}, {}
for _, row in ipairs(options) do
  ordinalToOption[row[1]] = row[2]
  names[row[2]] = "mtx_weapon" .. row[3]
  -- Representative stock tab split: earlier rows in DLC, positive sort keys in DLC2.
  sortKeys[row[2]] = row[3] == 1 or row[3] >= 20
end
function UIExpression.GetWeaponOptionGroupIndex(_, ordinal) return ordinalToOption[ordinal] end
function CoD.CACUtility.ShouldDisplayWeaponOptionMTX(_, option)
  return not Engine.SkipMTXItem(names[option])
end
function CoD.CACCamo.IsCamoHidden(option, sentinel)
  assert(sentinel == nil or sentinel == "forwarded")
  return option ~= 0, "stock-hidden"
end
local stockHidden, stockDisplay = CoD.CACCamo.IsCamoHidden, CoD.CACUtility.ShouldDisplayWeaponOptionMTX
function CoD.ContentGridButton.SetMTXPurchased(button, purchased)
  if purchased then button.mtxImage = nil else button.mtxImage = button.mtxImage or {} end
end
function CoD.CamoGridButton.UpdateClass(button, event)
  updates = updates + 1
  if names[button.weaponOption] then
    local purchased = Engine.HasMTX(event.controller, names[button.weaponOption])
    CoD.ContentGridButton.SetMTXPurchased(button, purchased, event.controller)
    button:setHintText("stock purchase hint", "stock hint image")
    button:setActionPromptString(purchased and "MENU_SELECT" or "MENU_PURCHASE")
  else
    button:setLocked(Engine.GetItemOptionLocked(event.controller, button.weaponItemIndex, button.weaponOption))
    button:setHintText("stock prerequisite hint", "stock lock image")
    button.overrideLock, button.customEvent, button.showProgressBar = true, "stock_event", true
  end
  return "stock-update"
end
local stockUpdate = CoD.CamoGridButton.UpdateClass
local function newButton(ordinal, option)
  local button = element()
  button.camoNum, button.contentIndex = ordinal, ordinal
  button.weaponOption, button.weaponItemIndex = option or ordinalToOption[ordinal], 7
  button:registerEventHandler("update_class", CoD.CamoGridButton.UpdateClass)
  return button
end
local function refresh(button, controller, challengeMenu)
  return button.m_eventHandlers.update_class(button,
    {controller = controller or 0, contentIndex = 0, challengeMenu = challengeMenu})
end

-- Selection/equip follow the stock payload boundary: ordinal -> option ID -> chosenItem -> SetClassItem.
function CoD.CACCamo.CamoEquip(container, event)
  local player = CoD.perController[event.controller]
  local chosen = player.chosenItem
  player.chosenItem = nil
  if chosen then
    Engine.SetClassItem(event.controller, player.classNum,
      player.knifePersonalization and "knifecamo" or player.weaponSlot .. "camo", chosen)
  end
end
function CoD.CACCamo.CamoButtonSelected(container, event)
  selections = selections + 1
  if event.button.lockImage or event.button.inactive then return "locked" end
  if event.button.mtxImage then
    Engine.PurchaseMTX(event.controller, names[event.button.weaponOption])
    return "purchase"
  end
  CoD.perController[event.controller].chosenItem =
    UIExpression.GetWeaponOptionGroupIndex(nil, event.contentIndex)
  CoD.CACCamo.CamoEquip(container, event)
  return "equipped"
end
local stockSelect, stockEquip, stockSet = CoD.CACCamo.CamoButtonSelected, CoD.CACCamo.CamoEquip, Engine.SetClassItem
local engineFunctions = {}
for key, fn in pairs(Engine) do engineFunctions[key] = fn end

-- An open button captured stock before installation; a custom handler must remain untouched.
local existing, custom = newButton(16), newButton(16)
custom:registerEventHandler("update_class", function() return "custom" end)
local customHandler = custom.m_eventHandlers.update_class
LUI.roots.UIRoot0:addElement(existing)
LUI.roots.UIRoot0:addElement(custom)
LUI.roots.UIRoot0:registerEventHandler("content_button_selected", stockSelect)
assert(dofile(source))
assert(type(LUI.bo2zm.installLocalCamos) == "function")
assert(existing.m_eventHandlers.update_class == CoD.CamoGridButton.UpdateClass)
assert(existing.m_eventHandlers.update_class ~= stockUpdate)
assert(custom.m_eventHandlers.update_class == customHandler)
assert(LUI.roots.UIRoot0.m_eventHandlers.content_button_selected == CoD.CACCamo.CamoButtonSelected)
assert(refresh(existing) == "stock-update" and not existing.lockImage and not existing.inactive)

local function assertStock(button)
  if names[button.weaponOption] then
    assert(button.mtxImage and button.actionPromptString == "MENU_PURCHASE")
    assert(button.hintText == "stock purchase hint" and button.hintImage == "stock hint image")
  else
    assert(button.lockImage and button.inactive and button.overrideLock and button.customEvent == "stock_event")
    assert(button.showProgressBar and button.hintText == "stock prerequisite hint")
  end
  assert(button.bo2mpLocalCamo == nil)
end
local function assertLocal(button, controller)
  assert(refresh(button, controller) == "stock-update")
  assert(not button.lockImage and not button.mtxImage and not button.inactive)
  assert(not button.overrideLock and not button.customEvent and not button.showProgressBar)
  assert(button.hintText == "" and button.hintImage == nil and button.actionPromptString == "MENU_SELECT")
end
local function choose(button, controller, knife)
  CoD.perController[controller] = {classNum = 3, weaponSlot = "secondary", knifePersonalization = knife}
  assert(CoD.CACCamo.CamoButtonSelected({},
    {controller = controller, contentIndex = button.contentIndex, button = button}) == "equipped")
  local write = writes[#writes]
  assert(write[1] == controller and write[2] == 3 and write[3] == (knife and "knifecamo" or "secondarycamo"))
  assert(write[4] == button.weaponOption, "equip used a camo ordinal or MTX index instead of the option ID")
  assert(CoD.perController[controller].chosenItem == nil)
end

-- Exact allowlist, both controllers, repeated updates, both tab categories and the unmodified equip path.
local buttons = {newButton(16)}
for _, row in ipairs(options) do buttons[#buttons + 1] = newButton(row[1]) end
for _, button in ipairs(buttons) do
  assert(CoD.CACCamo.IsCamoHidden(button.weaponOption) == false)
  if names[button.weaponOption] then assert(CoD.CACUtility.ShouldDisplayWeaponOptionMTX(1, button.weaponOption)) end
  for controller = 0, 1 do
    assertLocal(button, controller)
    assertLocal(button, controller)
    choose(button, controller, false)
  end
end
assert(#writes == 52 and purchases == 0)
choose(buttons[1], 1, true)
local dlc, dlc2 = 0, 0
for _, row in ipairs(options) do
  if CoD.CACUtility.ShouldDisplayWeaponOptionMTX(0, row[2]) then
    if sortKeys[row[2]] then dlc2 = dlc2 + 1 else dlc = dlc + 1 end
  end
end
assert(dlc > 0 and dlc2 > 0 and dlc + dlc2 == 25, "DLC2 tab eligibility lost")
local expected = {[16] = true}
for _, row in ipairs(options) do expected[row[2]] = true end
for option = 1, 120 do
  local hiddenResult, sentinel = CoD.CACCamo.IsCamoHidden(option)
  assert(hiddenResult == not expected[option])
  assert((sentinel == nil) == (expected[option] == true), "allowlist widened")
end

-- Stock tab-building composition: lock candidates disappear before construction; MTX visibility creates DLC2.
local function tabCounts()
  local counts = {camo = 0, dlc = 0, dlc2 = 0}
  if not CoD.CACCamo.IsCamoHidden(16) or not Engine.GetItemOptionLocked(0, 7, 16) then
    counts.camo = counts.camo + 1
  end
  for _, row in ipairs(options) do
    if CoD.CACUtility.ShouldDisplayWeaponOptionMTX(0, row[2]) then
      local tab = sortKeys[row[2]] and "dlc2" or "dlc"
      counts[tab] = counts[tab] + 1
    end
  end
  return counts
end
assert(tabCounts().camo == 1 and tabCounts().dlc2 > 0)
ready = 0
assert(tabCounts().camo == 0 and tabCounts().dlc == 0 and tabCounts().dlc2 == 0)
ready = 1

-- A metadata-only/future camo and separate digital-unlock rows retain stock behavior.
for _, option in ipairs({0, 15, 17, 18, 19, 20, 28, 55, 65, 95, 97, 98, 999}) do
  local hiddenResult, sentinel = CoD.CACCamo.IsCamoHidden(option, "forwarded")
  assert(hiddenResult == (option ~= 0) and sentinel == "stock-hidden")
  assert(not CoD.CACUtility.ShouldDisplayWeaponOptionMTX(0, option))
  local button = newButton(0, option)
  refresh(button)
  assertStock(button)
end
assert(not CoD.CACUtility.ShouldDisplayWeaponOptionMTX(0, 16), "Diamond was treated as an MTX package")

-- Every guard is reevaluated on calls, and disabling restores buttons that were already changed.
local gates = {
  function(on) ready = on and 1 or 0 end,
  function(on) ready = on and 1 or 2 end,
  function(on) ready = on and 1 or nil end,
  function(on) sessionMode = on and 0 or 1 end,
  function(on) sessionMode = on and 0 or 2 end,
  function(on) gameMode = on and 2 or 0 end,
  function(on) CoD.isMultiplayer = on end,
  function(on) CoD.isSinglePlayer = not on end,
  function(on) CoD.isZombie = not on end,
  function(on) Engine.SessionModeIsMode = on and engineFunctions.SessionModeIsMode or nil end,
  function(on) Engine.GameModeIsMode = on and engineFunctions.GameModeIsMode or nil end,
  function(on) CoD.SESSIONMODE_OFFLINE = on and 0 or nil end,
  function(on) CoD.GAMEMODE_LOCAL_SPLITSCREEN = on and 2 or nil end,
}
for _, gate in ipairs(gates) do
  local diamond, mtx = buttons[1], buttons[2]
  assertLocal(diamond)
  assertLocal(mtx)
  gate(false)
  -- No refresh between closing the gate and pressing A: neither equip nor purchase may use stale presentation.
  local beforeWrites, beforePurchases, beforeSelections = #writes, purchases, selections
  for _, button in ipairs({diamond, mtx}) do
    assert(CoD.CACCamo.CamoButtonSelected({}, {controller = 0, contentIndex = button.contentIndex,
      button = button}) == nil)
    assertStock(button)
  end
  assert(#writes == beforeWrites and purchases == beforePurchases and selections == beforeSelections)
  assert(CoD.CACCamo.IsCamoHidden(16) and not CoD.CACUtility.ShouldDisplayWeaponOptionMTX(0, 45))
  refresh(diamond)
  refresh(mtx)
  assertStock(diamond)
  assertStock(mtx)
  gate(true)
  assertLocal(diamond)
  assertLocal(mtx)
end
refresh(buttons[1], 0, true)
assertStock(buttons[1])
assertLocal(buttons[1])
-- Owned stock rows also survive enable/disable without inventing an unowned icon.
owned["0:mtx_weapon0"] = true
assertLocal(buttons[2])
ready = 0
refresh(buttons[2])
assert(not buttons[2].mtxImage and buttons[2].actionPromptString == "MENU_SELECT")
ready = 1

-- Install is idempotent and wraps newly reloaded stock functions; require keeps its return values.
local wrapped = CoD.CamoGridButton.UpdateClass
LUI.bo2zm.install()
assert(CoD.CamoGridButton.UpdateClass == wrapped)
CoD.CamoGridButton.UpdateClass = stockUpdate
CoD.CACCamo.IsCamoHidden = stockHidden
CoD.CACUtility.ShouldDisplayWeaponOptionMTX = stockDisplay
local reloaded = newButton(16)
LUI.roots.UIRoot0:addElement(reloaded)
local first, second, third = require("camo fixture reload")
assert(first == "required" and second == nil and third == "third")
assert(CoD.CamoGridButton.UpdateClass ~= stockUpdate)
assert(reloaded.m_eventHandlers.update_class == CoD.CamoGridButton.UpdateClass)
assert(CoD.CACCamo.IsCamoHidden(16) == false and CoD.CACUtility.ShouldDisplayWeaponOptionMTX(0, 45))
assertLocal(newButton(16))
assert(CoD.CACCamo.CamoEquip == stockEquip and selections == #writes)
assert(Engine.SetClassItem == stockSet and not Engine.IsSignedInToLive())
for name, fn in pairs(engineFunctions) do assert(Engine[name] == fn, "Engine function replaced: " .. name) end
for name in pairs(Engine) do assert(engineFunctions[name], "Engine function added: " .. name) end
assert(Engine.GetItemOptionLocked() and not Engine.HasMTX(1, "mtx_weapon24"))
assert(#logs == 1 and string.find(logs[1], "menus.lua installed", 1, true), "unexpected command or Lua error")
assert(purchases == 0 and updates > #writes)
-- Outside the gate, fresh stock buttons still take their original purchase route.
ready = 0
local stockOnly, beforeWrites = newButton(44), #writes
refresh(stockOnly, 1)
assert(CoD.CACCamo.CamoButtonSelected({}, {controller = 1, contentIndex = 44, button = stockOnly}) == "purchase")
assert(purchases == 1 and #writes == beforeWrites)
print("local camos: exact inventory, gates/transitions, tabs, refresh/rebind and stock equip payloads passed")
