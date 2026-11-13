-- Authored pause/popup fixtures run the complete menu overrides without loading game data.
-- Run with Lua 5.1 from the repository root, optionally passing a different menus.lua path.
local source = arg[1] or "games/bo2/src/ui/menus.lua"
local function run(zombies, leader)
  local logs, effects = {}, {}
  local clients = {[leader] = 0, [leader == 0 and 1 or 0] = 1}
  local primaryClient = 0
  local function hit(name, event)
    effects[#effects + 1] = {name, event and event.controller}
    return name, "forwarded"
  end
  local methods = {}
  local function element(owner)
    return setmetatable({owner = owner, children = {}, m_eventHandlers = {}}, {__index = methods})
  end
  function methods:getOwner() return self.owner end
  function methods:addButton(label)
    local button = element()
    button.label = label
    self.children[#self.children + 1] = button
    return button
  end
  function methods:setActionEventName(name) self.action = name end
  function methods:registerEventHandler(name, handler) self.m_eventHandlers[name] = handler end
  function methods:processEvent(event)
    local handler = self.m_eventHandlers[event.name]
    if handler then return handler(self, event) end
  end
  function methods:openMenu(name, controller) return hit(name, {controller = controller}) end
  function methods:saveState() end
  function methods:close() self.closed = true end
  Engine = {ExecNow = function(_, command) logs[#logs + 1] = command end}
  UIExpression = {
    DvarInt = function() return 0 end,
    IsInGame = function() return 1 end,
    IsPrimaryLocalClient = function(controller)
      return clients[controller] ~= nil and clients[controller] == primaryClient and 1 or 0
    end,
  }
  CoD = {isSinglePlayer = false, isMultiplayer = true, isZombie = zombies, Class = {},
    EndGamePopup = {}, RestartGamePopup = {}, fonts = {Condensed = "font"},
    -- Both controllers see a running local server; this cannot distinguish the leader.
    isHost = function() return true end}
  LUI = {createMenu = {}}
  require = function() return "required" end
  function CoD.Class.AddButton(menu, label, event)
    local button = menu.buttonList:addButton(label)
    button:setActionEventName(event)
    return button
  end
  for _, name in ipairs({"EndGameButtonPressed", "RestartGameButtonPressed"}) do
    CoD.Class[name] = function(_, event) return hit(name, event) end
  end
  function CoD.EndGamePopup.FinishEndGame(_, event) return hit("end", event) end
  function CoD.EndGamePopup.YesButtonPressed(menu, event) return CoD.EndGamePopup.FinishEndGame(menu, event) end
  function CoD.RestartGamePopup.RestartLevel(_, event) return hit("restart", event) end
  function CoD.RestartGamePopup.YesButtonPressed(menu, event)
    return CoD.RestartGamePopup.RestartLevel(menu, event)
  end
  function CoD.RestartGamePopup.CancelUpload(menu, event) return hit("cancelUpload", event) end
  function CoD.RestartGamePopup.WaitForDemostream(menu, event)
    return CoD.RestartGamePopup.RestartLevel(menu, event)
  end
  local function popup(owner, handlers)
    local menu = element(owner)
    for event, handler in pairs(handlers) do menu:registerEventHandler(event, handler) end
    menu:registerEventHandler("button_prompt_back", function(_, event) return hit("back", event) end)
    return menu
  end
  -- Cache the original callbacks, as a menu constructor can do before the overrides are installed.
  local classEnd, classRestart = CoD.Class.EndGameButtonPressed, CoD.Class.RestartGameButtonPressed
  local endYes = CoD.EndGamePopup.YesButtonPressed
  local restartYes, wait, cancel = CoD.RestartGamePopup.YesButtonPressed,
    CoD.RestartGamePopup.WaitForDemostream, CoD.RestartGamePopup.CancelUpload
  LUI.createMenu.class = function(controller)
    local menu = popup(controller, {open_endGamePopup = classEnd, openRestartGamePopup = classRestart})
    menu.buttonList = element()
    for _, row in ipairs({{"RESUME", "soloResumeGame"}, {"CLASS", "open_chooseClass"},
        {"TEAM", "open_chooseTeam"}, {"OPTIONS", "open_options"},
        {"RESTART", "openRestartGamePopup"}, {"END GAME", "open_endGamePopup"}}) do
      CoD.Class.AddButton(menu, row[1], row[2])
    end
    for _, event in ipairs({"soloResumeGame", "open_chooseClass", "open_chooseTeam", "open_options"}) do
      menu:registerEventHandler(event, function(_, data) return hit(event, data) end)
    end
    return menu
  end
  LUI.createMenu.EndGamePopup = function(controller)
    return popup(controller, {endGamePopup_YesButtonPressed = endYes})
  end
  LUI.createMenu.RestartGamePopup = function(controller)
    return popup(controller, {restartGamePopup_YesButtonPressed = restartYes,
      waitForDemoStream = wait, cancelUpload = cancel})
  end
  local originalFactory = LUI.createMenu.class
  assert(loadfile(source))()
  assert(LUI.createMenu.class ~= originalFactory)
  local function row(menu, event)
    for _, button in ipairs(menu.buttonList.children) do if button.action == event then return button end end
  end
  local function emit(menu, name, controller) return menu:processEvent({name = name, controller = controller}) end
  local function blocked(fn)
    local before = #effects
    assert(fn() == nil)
    assert(#effects == before, "blocked action reached stock side effects")
  end
  local function allowed(fn, name)
    local before = #effects
    assert(fn() == name)
    assert(#effects == before + 1 and effects[#effects][1] == name)
  end
  local function check(controller, permitted)
    local menu = LUI.createMenu.class(controller)
    assert((row(menu, "open_endGamePopup") ~= nil) == permitted)
    assert((row(menu, "openRestartGamePopup") ~= nil) == permitted)
    for _, event in ipairs({"soloResumeGame", "open_chooseClass", "open_chooseTeam", "open_options"}) do
      assert(row(menu, event), "personal row removed: " .. event)
      allowed(function() return emit(menu, event, controller) end, event)
    end
    assert(row(menu, "bo2zm_open_extras"), "Extras removed")
    local beforeExtras = #effects
    emit(menu, "bo2zm_open_extras", controller)
    assert(#effects == beforeExtras + 1 and effects[#effects][1] == "Bo2zmExtras")
    for event, name in pairs({open_endGamePopup = "EndGameButtonPressed",
        openRestartGamePopup = "RestartGameButtonPressed"}) do
      local action = function() return emit(menu, event, controller) end
      if permitted then allowed(action, name) else blocked(action) end
      blocked(function() return emit(menu, event, controller == 0 and 1 or 0) end)
      blocked(function() return emit(menu, event) end)
    end
    local ending, restarting = LUI.createMenu.EndGamePopup(controller), LUI.createMenu.RestartGamePopup(controller)
    for _, action in ipairs({
      {function() return emit(ending, "endGamePopup_YesButtonPressed", controller) end, "end"},
      {function() return CoD.EndGamePopup.FinishEndGame(ending, {controller = controller}) end, "end"},
      {function() return emit(restarting, "restartGamePopup_YesButtonPressed", controller) end, "restart"},
      {function() return emit(restarting, "cancelUpload", controller) end, "cancelUpload"},
      {function() return emit(restarting, "waitForDemoStream") end, "restart"},
      {function() return CoD.RestartGamePopup.RestartLevel(restarting, {}) end, "restart"},
    }) do
      if permitted then allowed(action[1], action[2]) else blocked(action[1]) end
    end
    blocked(function() return emit(ending, "endGamePopup_YesButtonPressed", controller == 0 and 1 or 0) end)
    blocked(function() return emit(restarting, "waitForDemoStream", controller == 0 and 1 or 0) end)
    allowed(function() return emit(ending, "button_prompt_back", controller) end, "back")
    return menu, ending, restarting
  end
  check(leader, true)
  check(leader == 0 and 1 or 0, false)
  -- Identity is queried for each action, so an already-open popup loses host privileges when the leader changes.
  local _, ending, restarting = check(leader, true)
  local oldLeader = leader
  primaryClient = 1
  blocked(function() return emit(ending, "endGamePopup_YesButtonPressed", oldLeader) end)
  blocked(function() return emit(restarting, "waitForDemoStream") end)
  check(leader == 0 and 1 or 0, true)
  check(leader, false)
  primaryClient = 0
  -- Missing/throwing APIs and ambiguous returns all fail closed for both rows and callbacks.
  local primary = UIExpression.IsPrimaryLocalClient
  for _, api in ipairs({false, function() error("unavailable") end, function() return -1 end,
      function() return "1" end}) do
    UIExpression.IsPrimaryLocalClient = api or nil
    check(leader, false)
  end
  UIExpression.IsPrimaryLocalClient = function(controller) return controller == leader end
  check(leader, true)
  UIExpression.IsPrimaryLocalClient = primary
  local ownerMethod = methods.getOwner
  methods.getOwner = nil
  check(leader, false)
  methods.getOwner = function() error("no owner") end
  check(leader, false)
  methods.getOwner = ownerMethod
  check(-1, false)
  check(0.5, false)
  -- No event or invalid controller can fall through to stock's default controller.
  blocked(function() return CoD.Class.EndGameButtonPressed(element(leader), nil) end)
  blocked(function() return CoD.Class.EndGameButtonPressed(element(leader), {controller = tostring(leader)}) end)
  -- Installation is idempotent and handles a replaced constructor and callback after require.
  local wrapped = LUI.createMenu.class
  LUI.bo2zm.install()
  assert(LUI.createMenu.class == wrapped)
  LUI.createMenu.class = originalFactory
  CoD.Class.EndGameButtonPressed = classEnd
  assert(require("pause fixture reload") == "required")
  assert(LUI.createMenu.class ~= originalFactory and CoD.Class.EndGameButtonPressed ~= classEnd)
  check(leader, true)
  check(leader == 0 and 1 or 0, false)
  assert(#logs == 1 and string.find(logs[1], "menus.lua installed", 1, true), "unexpected Lua error")
end
for _, zombies in ipairs({false, true}) do for leader = 0, 1 do run(zombies, leader) end end
print("leader pause: MP/ZM, either leader port, rows/events/popups, owner mismatch, identity loss and reload passed")
