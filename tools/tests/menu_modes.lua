-- Exercise the actual menu overrides with small LUI fixtures. Rendering remains a game/hardware check.
local source = arg[1] or "app/src/ui/menus.lua"
-- Optional negative-control selector: omit to run both retained review regressions.
local reviewCase = arg[2]
assert(reviewCase == nil or reviewCase == "bot_focus" or reviewCase == "hud_scope", "unknown review case")
-- installed: the program's list of installed modes (bo2zm_modes_ui, "cp mp zm"); nil leaves it unset, so the menus
-- fall back to Zombies and Multiplayer plus Campaign when campaignInstalled.
local function run(mode, inGame, testSpec, focusField, campaignInstalled, installed)
  local function has(m)
    if installed == nil then return m ~= "cp" or campaignInstalled == true end
    return string.find(" " .. installed .. " ", " " .. m .. " ", 1, true) ~= nil
  end
  local logs, methods, dvars = {}, {}, {
    bo2zm_scripts_count_ui = 2, bo2zm_script_0_name_ui = "Gr3Zz", bo2zm_script_1_name_ui = "Example",
    bo2zm_scripts_active_ui = "None", bo2zm_scripts_status_ui = "Disabled",
    bo2zm_scripts_route_ui = "INTERNAL (embedded paths)", bo2zm_scripts_base_ui = "Not used",
    bo2zm_scripts_preview_ui = "Not checked", bo2zm_scripts_target_count_ui = 0,
    bo2zm_scripts_issue_count_ui = 0,
    bo2zm_profile_on_ui = mode == "zombies" and 0 or 2,
    bo2zm_campaign_ui = campaignInstalled and 1 or 0,
    bo2zm_modes_ui = installed,
    ui_mapname = "zm_transit", ui_gametype = "zstandard",
    ui_zm_mapstartlocation = mode == "zombies" and "town" or nil,
    bo2zm_bots_count_ui = 0, bo2zm_bots_available_ui = 0, bo2zm_bots_max_ui = 3,
    bo2zm_bots_status_ui = "Unavailable: Missing bot data. Next match: OFF",
  }
  local starts, modeSwitches = 0, 0
  -- System Link fixture: the session mode, whether this console hosts the lobby, and its party's player count.
  local systemLink, hosting, partyPlayers = false, true, 1
  local queuedCommands = {}
  local botContext
  local targetCount, issueCount, checkResult = 2, 2, "Ready: 2 scripts"
  local scriptSources = {"flat.gsc", "nested/" .. string.rep("source_folder/", 12) .. "script.gsc"}
  local scriptTargets = {"maps/mp/zombies/flat.gsc", "maps/mp/" .. string.rep("target_folder/", 12) .. "script.gsc"}
  local setupIssues = {"MissingConfig: missing mod.ini", "InvalidConfig: invalid mod.ini: unknown path source"}
  local object
  object = function(label)
    return setmetatable({label = label, children = {}, m_eventHandlers = {}, m_choices = {}}, {__index = methods})
  end
  function methods:addElement(child)
    child.parent = self
    table.insert(self.children, child)
  end
  function methods:addElementToTop(child)
    child.parent = self
    table.insert(self.children, 1, child)
  end
  function methods:removeElement(child)
    for i, value in ipairs(self.children) do
      if value == child then table.remove(self.children, i); child.parent = nil; break end
    end
  end
  function methods:addButton(label, hint, priority)
    local button = object(label)
    button.hintText = hint
    self:addElement(button)
    button.priority = priority
    if priority then
      table.remove(self.children)
      local index = 1
      while self.children[index] and (self.children[index].priority or 0) <= priority do index = index + 1 end
      table.insert(self.children, index, button)
    end
    return button
  end
  function methods:addDvarLeftRightSelector(controller, label, dvar, hint)
    local selector = self:addButton(label, hint)
    selector.m_dvarName = dvar
    return selector
  end
  function methods:addText(label) return self:addButton(label) end
  function methods:addSpacer() return self:addButton("spacer") end
  function methods:addChoice(controller, label, value, unused, callback)
    table.insert(self.m_choices, {label = label, value = value, callback = callback})
  end
  function methods:clearChoices() self.m_choices = {} end
  function methods:getCurrentValue() return tostring(dvars[self.m_dvarName] or 0) end
  function methods:setOwner(controller) self.owner = controller end
  function methods:getOwner() return self.owner end
  function methods:getParent() return self.parent end
  function methods:getFirstChild() return self.children[1] end
  function methods:getNextSibling()
    if not self.parent then return nil end
    for i, child in ipairs(self.parent.children) do
      if child == self then return self.parent.children[i + 1] end
    end
  end
  function methods:registerEventHandler(name, handler) self.m_eventHandlers[name] = handler end
  function methods:setLabel(label) self.label = label end
  function methods:setActionEventName(event) self.action = event end
  function methods:isFocusable() return not self.unfocusable end
  function methods:isInFocus() return self.focused == true end
  function methods:makeNotFocusable() self.unfocusable = true end
  function methods:makeFocusable() self.unfocusable = nil end
  function methods:setPreviousMenu(name) self.previous = name end
  function methods:addLargePopupBackground() self.popupBackground = true end
  function methods:close()
    self.closed, self.focused = true, false
    if self.parent then self.parent:removeElement(self) end
  end
  function methods:processEvent(event)
    if event.name == "gain_focus" then
      assert(not self.closed and self:isFocusable(), "focus reached a closed or unfocusable button")
      self.focused = true
      self.focusLabel = self.label
    elseif event.name == "lose_focus" then
      self.focused = false
    end
    local handler = self.m_eventHandlers[event.name]
    if handler then return handler(self, event) end
  end
  function methods:openMenu(name, controller)
    self.openedMenuName = name
    self.openedMenu = LUI.createMenu[name](controller)
    return self.openedMenu
  end
  function methods:openPopup(name, controller, options)
    self.openedPopupName, self.openedPopupOptions = name, options
    self.openedPopup = name == "Error" and object(name) or LUI.createMenu[name](controller)
    self.openedPopup.occludedMenu = self
    return self.openedPopup
  end
  function methods:setMessage(text) self.message = text end
  local noop = function() end
  for _, name in ipairs({"setLeftRight", "setTopBottom", "setFont", "setRGB", "setUseStencil", "addTitle",
      "addSelectButton", "addBackButton", "animateToState", "disable", "enable",
      "disableCycling", "enableCycling", "saveState", "registerAnimationState", "setAlpha", "refreshChoice", "updateChoice",
      "setAlignment"}) do
    methods[name] = noop
  end
  function methods:setAlpha(value) self.alpha = value end
  function methods:setText(text) self.text = text end
  function methods:enable()
    self.disabled = nil
    self.stateChanges = (self.stateChanges or 0) + 1
  end
  function methods:disable()
    self.disabled = true
    self.stateChanges = (self.stateChanges or 0) + 1
  end
  Engine = {ExecNow = function(controller, text)
              table.insert(logs, text)
              local map, game, start, count = string.match(text,
                  "^bo2zm_bots_count ([a-z0-9_]+) ([a-z0-9_]+) ([a-z0-9_-]+) ([0-3])$")
              if map then
                botContext = map .. "." .. game .. "." .. start
                dvars.bo2zm_bots_count_ui = math.min(tonumber(count), dvars.bo2zm_bots_max_ui)
              elseif string.sub(text, 1, 19) == "bo2zm_bots_context " then
                local key = string.gsub(string.sub(text, 20), " ", ".")
                if key ~= botContext then dvars.bo2zm_bots_count_ui = 0 end
                botContext = key
              elseif text == "bo2zm_bots_open" then
                dvars.bo2zm_bots_count_ui = 0
                dvars.bo2zm_bots_max_ui = 3
              elseif string.match(text, "^bo2zm_bots_humans [1-4]$") then
                dvars.bo2zm_bots_max_ui = 4 - tonumber(string.sub(text, -1))
                dvars.bo2zm_bots_count_ui = math.min(dvars.bo2zm_bots_count_ui, dvars.bo2zm_bots_max_ui)
              elseif text == "bo2zm_scripts_check" then
                dvars.bo2zm_scripts_preview_ui = checkResult
                dvars.bo2zm_scripts_target_count_ui = targetCount
                for i = 0, 31 do
                  dvars["bo2zm_scripts_source_" .. i .. "_ui"] = scriptSources[i + 1] or "source_" .. i .. ".gsc"
                  dvars["bo2zm_scripts_target_" .. i .. "_ui"] = scriptTargets[i + 1] or "maps/mp/target_" .. i .. ".gsc"
                end
              elseif text == "bo2zm_scripts_refresh" then
                dvars.bo2zm_scripts_scan_ui = "2 matching mods, " .. issueCount .. " setup issues"
                dvars.bo2zm_scripts_issue_count_ui = issueCount
                for i = 0, 31 do
                  dvars["bo2zm_scripts_issue_" .. i .. "_ui"] = setupIssues[i + 1] or "Folder_" .. i .. ": missing mod.ini"
                end
              end
            end,
            Exec = function(controller, text) table.insert(queuedCommands, text) end,
            SetGametype = function(value) dvars.gametype = value end,
            SetDvar = function(name, value) dvars[name] = value end,
            PartyHostToggleStart = function() starts = starts + 1 end,
            PartyGetPlayerCount = function() return partyPlayers end,
            Localize = function(text) return text == "ZMUI_MAP_CAPS" and "MAP" or text end}
  UIExpression = {IsInGame = function() return inGame and 1 or 0 end,
                  SessionMode_IsSystemlinkGame = function() return systemLink and 1 or 0 end,
                  GetCurrentMapTableName = function() return "maps.csv" end,
                  ToUpper = function(controller, text) return string.upper(text) end,
                  TableLookup = function(controller, tableName, ...)
                    local columns = {...}
                    if tableName == "maps.csv" then
                      assert(columns[1] == 0 and columns[3] == 3, "the map name lookup changed")
                      return columns[2] == "zm_transit" and "GREEN RUN" or ""
                    end
                    if tableName == "gametypes.csv" then
                      assert(#columns == 5 and columns[1] == 0 and columns[2] == 5 and columns[3] == 3
                          and columns[5] == 4, "the start location lookup changed")
                      return string.upper(columns[4])
                    end
                    return ""
                  end,
                  DvarInt = function(controller, name) return tonumber(dvars[name]) or 0 end,
                  DvarString = function(controller, name)
                    assert(not string.match(name, "bo2zm_scripts_[a-z]+_32_ui"), "read past the manager's 32-row limit")
                    return name == "bo2zm_test_start_ui" and (testSpec or "") or tostring(dvars[name] or "")
                  end}
  require = noop
  CoD = {
    LeftRightSelector = {SetCurrentChoice = function(selector, value) selector.m_currentChoice = value end},
    isSinglePlayer = mode == "campaign", isZombie = mode == "zombies", fonts = {Condensed = "font"},
    gametypesTable = "gametypes.csv",
    textSize = {Default = 20}, DefaultTextColor = {r = 1, g = 1, b = 1},
    trueOrange = {r = 1, g = 0.5, b = 0}, CoD9Button = {Height = 30, TextHeight = 25},
    Options = {Width = 400}, HintText = {Height = 21}, ButtonPrompt = {Height = 30},
    ButtonList = {DefaultWidth = 400, new = function() return object() end},
    Menu = {Height = 720, TitleHeight = 60, New = function(name) return object(name) end},
    InGameMenu = {New = function(name, controller)
      local menu = object(name)
      menu:setOwner(controller)
      return menu
    end},
    Class = {AddButton = function(menu, label) return menu.buttonList:addButton(label) end},
    ObjectiveInfoMenu = {AddButton = function(list, label) return list:addButton(label) end},
  }
  CoD.Menu.NewFromState = function(name)
    local menu = object(name)
    menu.menuName = name
    return menu
  end
  LUI = {createMenu = {}, UITimer = {new = function() return object() end},
         UIText = {new = function() return object("text") end}, Alignment = {Left = 0},
         UIVerticalList = {UpdateNavigation = noop}}
  local originalHud = function(controller)
    local menu = CoD.Menu.NewFromState("HUD", {})
    menu:setOwner(controller)
    menu.controller = controller
    return menu
  end
  local originalLoading = function() return object("Loading") end
  LUI.createMenu.HUD, LUI.createMenu.Loading = originalHud, originalLoading
  LUI.createMenu.SplitscreenGameLobby = function()
    local menu = object()
    menu.buttonPane = object()
    CoD.PrivateLocalGameLobby.PopulateButtons(menu.buttonPane)
    menu.buttonPane.body.startMatchButton:processEvent({name = "gain_focus", controller = 0})
    return menu
  end
  LUI.createMenu.PrivateLocalGameLobby = function()
    local menu = object()
    menu.buttonPane = object()
    CoD.PrivateLocalGameLobby.PopulateButtons(menu.buttonPane)
    if hosting then menu.buttonPane.body.startMatchButton:processEvent({name = "gain_focus", controller = 0}) end
    return menu
  end
  LUI.createMenu.SelectMapZM = function() return object() end
  -- ui_mp/t6/mainmenu.lua's flyout handlers: the stock Back and its prompt come back when the flyout closes.
  CoD.MainMenu = {
    Back = function(self, event)
      modeSwitches = modeSwitches + 1
      local params = {{leaveText = "MENU_CAMPAIGN", leaveEvent = "start_sp"}}
      table.insert(params, mode == "zombies" and {leaveText = "MENU_MULTIPLAYER", leaveEvent = "start_mp"}
                                              or {leaveText = "MENU_ZOMBIE", leaveEvent = "start_zombies"})
      self:openPopup("ConfirmLeave", event.controller, {titleText = "MENU_MAIN_MENU_CAPS", params = params})
    end,
    CloseSystemLinkFlyout = function(self)
      self.flyoutClosed = (self.flyoutClosed or 0) + 1
      self.buttonList:animateToState("default")
      self.systemLinkFlyoutContainer:animateToState("default")
      if self.backButton then self.backButton:close() end
      self.backButton = object("BACK")
      self:addElement(self.backButton)
      self:registerEventHandler("button_prompt_back", CoD.MainMenu.Back)
    end,
    OpenSystemLinkFlyout = function(self)
      self.buttonList:animateToState("disabled")
      self.systemLinkFlyoutContainer:animateToState("show")
      if self.backButton then self.backButton:close() end
      self.backButton = object("BACK")
      self:addElement(self.backButton)
      self:registerEventHandler("button_prompt_back", CoD.MainMenu.CloseSystemLinkFlyout)
    end,
  }
  LUI.createMenu.MainMenu = function()
    local menu = object()
    menu.buttonList = object()
    menu:addElement(menu.buttonList)
    -- Animation states keep the place the element had when they were registered (the stock place).
    local function placed(element, top, bottom, state, alpha)
      element.top, element.bottom, element.states = top, bottom, {}
      function element:setTopBottom(_, _, newTop, newBottom) self.top, self.bottom = newTop, newBottom end
      function element:registerAnimationState(name, values)
        self.states[name] = {top = values.top or self.top, bottom = values.bottom or self.bottom}
      end
      function element:animateToState(name)
        local values = assert(self.states[name], "unknown animation state " .. name)
        self.top, self.bottom, self.state = values.top, values.bottom, name
      end
      element:registerAnimationState("default", {top = top, bottom = bottom})
      element:registerAnimationState(state, {alpha = alpha})
    end
    local row, prompts = CoD.CoD9Button.Height, CoD.ButtonPrompt.Height
    local stockTop = -(row * (8 + 1)) - prompts
    placed(menu.buttonList, stockTop, -prompts, "disabled", 0.5)
    menu.systemLinkFlyoutContainer = object("flyout")
    menu:addElement(menu.systemLinkFlyoutContainer)
    placed(menu.systemLinkFlyoutContainer, stockTop + row + 2, stockTop + row + 2 + row * 2 + 2, "show", 1)
    menu.mainLobbyButton = menu.buttonList:addButton("Xbox LIVE", nil, 1)
    menu.mainLobbyButton:setActionEventName("open_main_lobby_requested")
    menu.systemLinkButton = menu.buttonList:addButton("SYSTEM LINK", nil, 2)
    menu.systemLinkButton:setActionEventName("open_system_link_flyout")
    -- Stock localButton uses MENU_LOCAL_CAPS / ZMUI_LOCAL_CAPS, not a separate split-screen field.
    menu.localButton = menu.buttonList:addButton(mode == "zombies" and "SPLIT SCREEN" or "LOCAL", "Local lobby hint", 3)
    menu.localButton:setActionEventName("open_local_match_lobby")
    menu.optionsButton = menu.buttonList:addButton("OPTIONS", nil, 4)
    menu.optionsButton:setActionEventName("open_options_menu")
    menu.spButton = menu.buttonList:addButton("CAMPAIGN", nil, 6)
    menu.spButton:setActionEventName("start_sp")
    local modeField = mode == "zombies" and "mpButton" or "zombieButton"
    menu[modeField] = menu.buttonList:addButton(mode == "zombies" and "MULTIPLAYER" or "ZOMBIES", nil, 7)
    menu[modeField]:setActionEventName(mode == "zombies" and "start_mp" or "start_zombies")
    menu[focusField or "mainLobbyButton"]:processEvent({name = "gain_focus", controller = 0})
    menu.backButton = object("BACK")
    menu:addElement(menu.backButton)
    menu.m_eventHandlers.button_prompt_back = CoD.MainMenu.Back
    if focusField == "systemLinkButton" then
      -- Rebuilt with SYSTEM LINK focused: the game opens the flyout while it builds the menu.
      menu.m_eventHandlers.button_prompt_back = CoD.MainMenu.CloseSystemLinkFlyout
      menu.buttonList:animateToState("disabled")
      menu.systemLinkFlyoutContainer:animateToState("show")
    end
    local function resetLiveButton(self)
      self.mainLobbyButton:setLabel("Xbox LIVE")
      self.mainLobbyButton:setActionEventName("open_main_lobby_requested")
      self.mainLobbyButton.disabled = true
      return "stock sign-in handler"
    end
    menu.m_eventHandlers.first_signed_in = resetLiveButton
    menu.m_eventHandlers.last_signed_out = resetLiveButton
    -- SYSTEM LINK's flyout buttons (CoD.MainMenu.OpenSystemLinkLobby / OpenServerBrowser).
    menu.m_eventHandlers.open_system_link_lobby = function(self, event)
      self.systemLinkLobbies = (self.systemLinkLobbies or 0) + 1
    end
    menu.m_eventHandlers.open_server_browser = function(self, event)
      self.serverBrowsers = (self.serverBrowsers or 0) + 1
    end
    menu.m_eventHandlers.open_local_match_lobby = function(self, event)
      self:openMenu(mode == "zombies" and "SelectMapZM" or "SplitscreenGameLobby", event.controller)
    end
    return menu
  end
  LUI.createMenu.GraphicsSettings = function()
    local menu = object()
    menu.buttonList = object()
    menu.anaglyphButton = menu.buttonList:addButton("3D")
    return menu
  end
  LUI.createMenu.Error = function() error("an Extras page failed to build") end
  LUI.createMenu.ConfirmLeave = function() return object("ConfirmLeave") end
  local function populateLobby(pane)
    if pane.body then
      while pane.body.buttonList.children[1] do pane.body.buttonList.children[1]:close() end
    end
    pane.body = {buttonList = object()}
    pane.body.buttonList.hintText = object("Begin the game")
    pane.body.statusText = object("Game begins in 5")
    for _, label in ipairs({"START MATCH", "SETUP GAME", "SETUP BOTS", "CREATE A CLASS"}) do
      local button = pane.body.buttonList:addButton(label)
      button:registerEventHandler("start_game", button.disable)
      button:registerEventHandler("cancel_start_game", button.enable)
      if label == "START MATCH" then
        button.hintText = Engine.Localize("MPUI_START_MATCH_DESC")
        pane.body.startMatchButton = button
      end
    end
    if mode == "zombies" then
      pane.body.changeMapButton = pane.body.buttonList:addButton("MAP")
      pane.body.changeMapButton:registerEventHandler("start_game", pane.body.changeMapButton.disable)
      pane.body.changeMapButton:registerEventHandler("cancel_start_game", pane.body.changeMapButton.enable)
    end
    -- A stock rebuild may have established fallback focus before the override restores its previous row.
    if pane.fixtureFocusStart then pane.body.startMatchButton:processEvent({name = "gain_focus", controller = 0}) end
  end
  CoD.PrivateGameLobby = {PopulateButtons = populateLobby, IsHost = function() return hosting end}
  CoD.PrivateLocalGameLobby = {PopulateButtons = populateLobby}
  local function newPlaylistButton(layout, action, gain, lose, itemInfo, index, category)
    local button = object(itemInfo.name)
    button.itemInfo, button.listIndex, button.category = itemInfo, index, category
    local function refresh(self, event)
      if self.itemInfo.index ~= nil and event.locked then self:disable() else self:enable() end
    end
    button:registerEventHandler("partylobby_update", refresh)
    button:registerEventHandler("gamelobby_update", refresh)
    return button
  end
  CoD.PlaylistButton = {new = newPlaylistButton}
  local camoTouches = 0
  if mode == "campaign" then
    CoD.CACCamo = setmetatable({}, {__index = function()
      camoTouches = camoTouches + 1
    end})
  end
  assert(loadfile(source))()
  assert((type(LUI.bo2zm.installLocalCamos) == "function") == (mode ~= "campaign"),
    "Campaign constructed the unused camo implementation")
  if mode == "campaign" then
    LUI.bo2zm.install()
    require("campaign camo boundary")
    assert(camoTouches == 0, "Campaign install touched MP camo APIs")
  end
  do
    local before = #logs
    local lobby = LUI.createMenu.SplitscreenGameLobby(0)
    local function rows()
      local result = {}
      for _, button in ipairs(lobby.buttonPane.body.buttonList.children) do
        assert(not result[button.label], "duplicate lobby row: " .. button.label)
        result[button.label] = button
      end
      return result
    end
    if mode ~= "campaign" then
      assert(lobby.buttonPane.body.startMatchButton.hintText == nil, "redundant start hint remains")
      local start = lobby.buttonPane.body.startMatchButton
      start.hintText = Engine.Localize("MPUI_START_MATCH_DESC")
      start:processEvent({name = "gamelobby_update"})
      assert(start.hintText == nil, "stock lobby update restored redundant help")
      start.hintText = "Not enough players"
      start:processEvent({name = "gamelobby_update"})
      assert(start.hintText == "Not enough players", "start validation warning was removed")
      start.hintText = nil
      local countdown = lobby.buttonPane.body.statusText
      lobby:processEvent({name = "start_game", controller = 0})
      assert(countdown.alpha == nil, "countdown was hidden with the help text")
      CoD.PrivateLocalGameLobby.PopulateButtons(lobby.buttonPane)
      assert(lobby.buttonPane.body.startMatchButton.hintText == nil, "pane rebuild restored start hint")
      lobby:processEvent({name = "cancel_start_game", controller = 0})
      assert(countdown.alpha == nil, "cancellation changed the countdown widget")
      lobby.buttonPane.body.startMatchButton:processEvent({name = "gain_focus", controller = 0})
    end
    local current = rows()
    if mode == "zombies" and not inGame then
      local selector = assert(current["BOT COUNT"], "Zombies LOCAL lobby has no bot selector")
      assert(not current["BOT STATUS"], "separate bot status row remains")
      assert(#selector.m_choices == 1 and selector.m_choices[1].label == "NOT INSTALLED")
      assert(not selector:isFocusable() and selector.disabled, "missing bots selector is editable")
      assert(current["START MATCH"].focused and not selector.focused, "bots stole lobby focus")
      assert(lobby.buttonPane.body.changeMapButton.label == "MAP: GREEN RUN / TOWN",
          "Zombies lobby did not show its selected map and location")
      local function refresh() lobby:processEvent({name = "bo2zm_refresh_bots", controller = 0}) end
      dvars.ui_zm_mapstartlocation = "farm"
      refresh()
      assert(lobby.buttonPane.body.changeMapButton.label == "MAP: GREEN RUN / FARM",
          "lobby map text did not follow the selected location")
      dvars.ui_zm_mapstartlocation = "town"
      refresh()
      dvars.ui_zm_mapstartlocation = ""
      refresh()
      assert(lobby.buttonPane.body.changeMapButton.label == "MAP: GREEN RUN",
          "lobby map text invented a start location")
      dvars.ui_mapname = "zm_frontend"
      refresh()
      assert(lobby.buttonPane.body.changeMapButton.label == "MAP", "front-end lobby kept a stale map")
      dvars.ui_mapname, dvars.ui_zm_mapstartlocation = "zm_transit", "town"
      refresh()
      assert(lobby.buttonPane.body.changeMapButton.label == "MAP: GREEN RUN / TOWN",
          "lobby map text did not return to the selected map")
      dvars.bo2zm_bots_available_ui = 1
      refresh()
      assert(selector:isFocusable() and not selector.disabled, "installed bots remained locked")
      assert(#selector.m_choices == 4 and selector.m_choices[1].label == "OFF")
      for i = 2, 4 do assert(selector.m_choices[i].label == tostring(i - 1)) end
      local initialChanges = selector.stateChanges
      local function choose(count)
        assert(selector.m_choices[count + 1].callback(selector, true), "bot count change was rejected")
      end
      for _ = 1, 10 do refresh() end
      assert(selector.stateChanges == initialChanges and not selector.focused, "bot refresh reset highlight or focus")
      assert(dvars.bo2zm_bots_count_ui == 0, "fresh map did not default OFF")
      local writes = #logs
      selector.m_choices[4].callback(selector, false)
      assert(#logs == writes, "constructing or refreshing choices wrote a preference")
      choose(3)
      assert(logs[#logs] == "bo2zm_bots_count zm_transit zstandard town 3")
      dvars.ui_zm_mapstartlocation = "farm"
      refresh()
      assert(dvars.bo2zm_bots_count_ui == 0, "Farm inherited Town's count")
      choose(1)
      dvars.ui_gametype = "zgrief"
      refresh()
      assert(dvars.bo2zm_bots_count_ui == 0, "game types share a bot preference")
      choose(2)
      dvars.ui_gametype, dvars.ui_zm_mapstartlocation = "zstandard", "town"
      refresh()
      assert(dvars.bo2zm_bots_count_ui == 0, "returning to a map restored stale bots")
      for _, map in ipairs({"zm_transit", "zm_transit_dr", "zm_nuked", "zm_highrise", "zm_prison", "zm_buried", "zm_tomb"}) do
        dvars.ui_mapname, dvars.ui_zm_mapstartlocation = map, ""
        refresh()
        choose(2)
        assert(logs[#logs] == "bo2zm_bots_count " .. map .. " zstandard - 2", "map without a start token failed")
        local reopened = LUI.createMenu.SplitscreenGameLobby(0)
        assert(reopened.buttonPane.body.bo2zmBotCount, "reopened lobby lost bots")
        assert(dvars.bo2zm_bots_count_ui == 0, "reopened lobby did not reset bots OFF")
      end
      dvars.ui_mapname, dvars.ui_zm_mapstartlocation = "zm_transit", "town"
      refresh()
      choose(3)
      lobby.buttonPane.body.startMatchButton:processEvent({name = "lose_focus", controller = 0})
      selector:processEvent({name = "gain_focus", controller = 0})
      CoD.PrivateLocalGameLobby.PopulateButtons(lobby.buttonPane)
      current = rows()
      assert(current["BOT COUNT"] and not current["BOT STATUS"], "rebuilt button pane changed bot row layout")
      selector = current["BOT COUNT"]
      assert(selector.focused and not current["START MATCH"].focused, "pane rebuild lost the bot selector's focus")
      assert(dvars.bo2zm_bots_count_ui == 3, "pane rebuild reset the current lobby choice")
      if reviewCase ~= "hud_scope" then
        -- A fourth human joins while BOT COUNT has focus, before the periodic selector refresh.
        dvars.bo2zm_bots_max_ui, dvars.bo2zm_bots_count_ui = 0, 0
        lobby.buttonPane.fixtureFocusStart = true
        CoD.PrivateLocalGameLobby.PopulateButtons(lobby.buttonPane)
        lobby.buttonPane.fixtureFocusStart = nil
        current = rows()
        selector = current["BOT COUNT"]
        assert(selector.bo2zmInstalled and selector.bo2zmMaximum == 0 and selector.disabled,
            "four humans did not rebuild a locked OFF-only selector")
        assert(#selector.m_choices == 1 and selector.m_choices[1].label == "OFF" and not selector.focused,
            "pane rebuild restored focus onto locked BOT COUNT")
        assert(current["START MATCH"].focused, "locked bot focus restoration cleared fallback focus")
        assert(dvars.bo2zm_bots_count_ui == 0, "full-lobby rebuild enabled bots")
        dvars.bo2zm_bots_max_ui = 3
        refresh()
        choose(3)
      end
      dvars.bo2zm_bots_status_ui = "Bots unavailable: script conflict: maps/mp/other.gsc"
      refresh()
      assert(selector.hintText == "Unavailable with this mod.", "conflict was hidden")
      local commands = #logs
      dvars.ui_zm_mapstartlocation = "../farm;quit"
      refresh()
      assert(selector.m_choices[4].callback(selector, true) == false, "unsafe context accepted")
      assert(#logs == commands, "unsafe context reached the command bridge")
      dvars.ui_zm_mapstartlocation = "town"
      dvars.bo2zm_bots_available_ui = 0
      refresh()
      assert(#selector.m_choices == 1 and selector.m_choices[1].label == "NOT INSTALLED")
      assert(selector.disabled, "removing bot files left cycling enabled")
      dvars.bo2zm_bots_available_ui = 1
      refresh()
      assert(#selector.m_choices == 4 and not selector.disabled, "reinstall did not restore choices")
      local stale = selector.m_choices[4].callback
      for maximum = 2, 0, -1 do
        dvars.bo2zm_bots_max_ui = maximum
        dvars.bo2zm_bots_count_ui = math.min(dvars.bo2zm_bots_count_ui, maximum)
        refresh()
        assert(#selector.m_choices == maximum + 1, "joining humans left excess bot choices")
        assert((selector.disabled == true) == (maximum == 0), "full lobby did not lock bots OFF")
        local beforeStale = #logs
        assert(stale(selector, true) == false and #logs == beforeStale, "stale callback bypassed new human cap")
      end
      dvars.bo2zm_bots_max_ui = 3
      refresh()
      assert(dvars.bo2zm_bots_count_ui == 0 and #selector.m_choices == 4, "leaving human re-enabled bots")
      local wrapped = LUI.createMenu.SplitscreenGameLobby
      LUI.bo2zm.install()
      assert(wrapped == LUI.createMenu.SplitscreenGameLobby, "local lobby wrapped twice")
    else
      assert(not current["BOT COUNT"] and not current["BOT STATUS"], "bots visible outside Zombies front end")
      for i = before + 1, #logs do
        assert(not string.find(logs[i], "bo2zm_bots_", 1, true), "non-Zombies UI sent a bot command")
      end
    end
    -- Keep the existing automatic-start and Script Loader checks independent.
    LUI.bo2zm.testStarted = nil
    dvars.ui_mapname, dvars.ui_gametype = "zm_transit", "zstandard"
    dvars.ui_zm_mapstartlocation = mode == "zombies" and "town" or nil
  end
  if mode ~= "campaign" and not inGame then
    -- System Link: PrivateLocalGameLobby announces itself like the Local lobby (its local players join the same way,
    -- host and joiner alike); a Zombies host gets BOT COUNT and reports its party's players; a joiner and Multiplayer
    -- get no row and send no bot command.
    local function rowsOf(lobby)
      local result = {}
      for _, button in ipairs(lobby.buttonPane.body.buttonList.children) do result[button.label] = button end
      return result
    end
    local function sent(from, prefix)
      local found = {}
      for i = from + 1, #logs do
        if string.sub(logs[i], 1, #prefix) == prefix then table.insert(found, logs[i]) end
      end
      return found
    end
    local savedMap, savedType, savedStart = dvars.ui_mapname, dvars.ui_gametype, dvars.ui_zm_mapstartlocation
    dvars.ui_mapname, dvars.ui_gametype = mode == "zombies" and "zm_transit" or "mp_hijacked", mode == "zombies" and "zclassic" or "dm"
    dvars.ui_zm_mapstartlocation = mode == "zombies" and "transit" or nil
    dvars.bo2zm_bots_available_ui = 1
    systemLink, hosting, partyPlayers = true, true, 2
    -- The game's System Link rule (two per console), and the Local lobby's (four).
    dvars.party_maxlocalplayers, dvars.party_maxlocalplayers_systemlink = 2, 2
    dvars.party_maxlocalplayers_local_splitscreen = 4
    local before = #logs
    local lobby = LUI.createMenu.PrivateLocalGameLobby(0)
    assert(#sent(before, "bo2zm_local_lobby 1 0") == 1, "the System Link lobby did not let its local players join")
    assert(dvars.party_maxlocalplayers == 4 and dvars.party_maxlocalplayers_systemlink == 4,
        "the System Link lobby kept two players per console")
    -- The game sets its own count again after the lobby opens (the Zombies start-location menu); the lobby's timer
    -- raises it and the join list's count again.
    dvars.party_maxlocalplayers_systemlink = 2
    local joinList, teamList = {addSplitscreenRow = function() end, maxLocalPlayers = 2}, {maxLocalPlayers = 7}
    lobby.lobbyPane = {body = {lobbyList = {playerLists = {teamList, joinList}}}}
    lobby:processEvent({name = "bo2zm_local_players", controller = 0})
    assert(dvars.party_maxlocalplayers_systemlink == 4 and joinList.maxLocalPlayers == 4 and teamList.maxLocalPlayers == 7,
        "the System Link lobby's join list kept the game's count")
    local current = rowsOf(lobby)
    if mode == "zombies" then
      local selector = assert(current["BOT COUNT"], "System Link host has no bot selector")
      local humans = sent(before, "bo2zm_bots_humans")
      assert(#humans == 1 and humans[1] == "bo2zm_bots_humans 2", "host did not report its two players")
      assert(#selector.m_choices == 3 and selector.m_choices[3].label == "2", "two players did not leave two bots")
      assert(current["START MATCH"].focused and not selector.focused, "bots stole the System Link lobby's focus")
      local function refresh() lobby:processEvent({name = "bo2zm_refresh_bots", controller = 0}) end
      for _ = 1, 5 do refresh() end
      assert(#sent(before, "bo2zm_bots_humans") == 1, "unchanged player count was sent again")
      assert(selector.m_choices[3].callback(selector, true), "System Link host could not choose two bots")
      assert(logs[#logs] == "bo2zm_bots_count zm_transit zclassic transit 2")
      partyPlayers = 4
      refresh()
      assert(logs[#logs] == "bo2zm_bots_humans 4" or sent(before, "bo2zm_bots_humans")[2] == "bo2zm_bots_humans 4",
          "a joining player was not reported")
      assert(#selector.m_choices == 1 and selector.disabled and dvars.bo2zm_bots_count_ui == 0,
          "a full System Link lobby kept bots")
      partyPlayers = 9
      refresh()
      assert(sent(before, "bo2zm_bots_humans")[3] == nil, "an out-of-range count reached the program")
      partyPlayers = 1
      refresh()
      assert(#selector.m_choices == 4, "a leaving player did not free a slot")
      CoD.PrivateLocalGameLobby.PopulateButtons(lobby.buttonPane)
      assert(rowsOf(lobby)["BOT COUNT"], "rebuilt System Link pane lost bots")
      -- A console that joined: no row, nothing sent.
      hosting, partyPlayers = false, 2
      before = #logs
      local joined = LUI.createMenu.PrivateLocalGameLobby(0)
      assert(#sent(before, "bo2zm_local_lobby 1 0") == 1, "a joining console's lobby did not let its local players join")
      assert(not rowsOf(joined)["BOT COUNT"], "a joining console got BOT COUNT")
      joined:processEvent({name = "bo2zm_refresh_bots", controller = 0})
      assert(#sent(before, "bo2zm_bots_humans") == 0 and #sent(before, "bo2zm_bots_count") == 0,
          "a joining console sent bot commands")
      -- The Local lobby keeps counting local controllers only.
      systemLink, hosting = false, true
      before = #logs
      LUI.createMenu.SplitscreenGameLobby(0)
      assert(#sent(before, "bo2zm_local_lobby 1") == 1 and #sent(before, "bo2zm_bots_humans") == 0,
          "the Local lobby changed")
    else
      assert(not current["BOT COUNT"], "Multiplayer System Link lobby got bots")
      assert(#sent(before, "bo2zm_bots_") == 0, "Multiplayer System Link lobby sent a bot command")
      systemLink = false
      before = #logs
      LUI.createMenu.SplitscreenGameLobby(0)
      assert(#sent(before, "bo2zm_local_lobby 1") == 1, "the Multiplayer Local lobby stopped announcing itself")
    end
    local closing = #logs
    lobby:close()
    assert(#sent(closing, "bo2zm_local_lobby 0") == 1, "a closed System Link lobby kept its local players' join")
    -- An unexpected PrivateLocalGameLobby outside System Link keeps the earlier Local behavior.
    systemLink, hosting, partyPlayers = false, true, 1
    before = #logs
    LUI.createMenu.PrivateLocalGameLobby(0)
    assert(#sent(before, "bo2zm_local_lobby 1") == 1, "non-System Link PrivateLocalGameLobby lost the Local lobby")
    dvars.ui_mapname, dvars.ui_gametype, dvars.ui_zm_mapstartlocation = savedMap, savedType, savedStart
    dvars.bo2zm_bots_available_ui, dvars.bo2zm_bots_count_ui, dvars.bo2zm_bots_max_ui = 0, 0, 3
    LUI.bo2zm.testStarted = nil  -- the Local lobbies above armed the one automatic start
  end
  if mode ~= "campaign" then
    for _, category in ipairs({"STANDARD", "DLC"}) do
      local button = CoD.PlaylistButton.new({}, "pick", "focus", "blur", {name = category}, 1, category)
      assert(button.listIndex == 1 and button.category == category, "playlist constructor arguments changed")
      for _ = 1, 10 do
        button.m_eventHandlers.partylobby_update(button, {})
        button.m_eventHandlers.gamelobby_update(button, {})
      end
      assert(button.stateChanges == nil, "map list refresh reset an enabled highlight")
    end
    local locked = CoD.PlaylistButton.new({}, "pick", "focus", "blur", {name = "playlist", index = 7})
    for _ = 1, 10 do locked.m_eventHandlers.partylobby_update(locked, {locked = true}) end
    assert(locked.disabled and locked.stateChanges == 1, "playlist lock transition was lost or repeated")
    for _ = 1, 10 do locked.m_eventHandlers.gamelobby_update(locked, {locked = false}) end
    assert(not locked.disabled and locked.stateChanges == 2, "playlist unlock transition was lost or repeated")
    local guardedNew = CoD.PlaylistButton.new
    LUI.bo2zm.install()
    assert(CoD.PlaylistButton.new == guardedNew, "periodic install wrapped playlist creation twice")
    CoD.PlaylistButton.new = newPlaylistButton
    require("T6.PlaylistButton")
    assert(CoD.PlaylistButton.new ~= newPlaylistButton, "reloaded playlist constructor was not guarded")
    local reloaded = CoD.PlaylistButton.new({}, "pick", "focus", "blur", {name = "reloaded"})
    reloaded.m_eventHandlers.gamelobby_update(reloaded, {})
    assert(reloaded.stateChanges == nil, "reloaded map list refresh reset the highlight")
  else
    assert(CoD.PlaylistButton.new == newPlaylistButton, "campaign playlist constructor was patched")
  end
  if mode ~= "campaign" then
    for _, lobby in ipairs({CoD.PrivateGameLobby, CoD.PrivateLocalGameLobby}) do
      local pane = object()
      lobby.PopulateButtons(pane)
      for _, button in ipairs(pane.body.buttonList.children) do
        button:enable()
        assert(button.stateChanges == nil, "unchanged enabled state reset the highlight")
        for _ = 1, 10 do button.m_eventHandlers.start_game(button, {}) end
        assert(button.disabled and button.stateChanges == 1, "repeated disable reset the highlight")
        for _ = 1, 10 do button.m_eventHandlers.cancel_start_game(button, {}) end
        assert(not button.disabled and button.stateChanges == 2, "repeated enable reset the highlight")
      end
    end
  end
  local main = LUI.createMenu.MainMenu(0)
  if mode ~= "campaign" then
    local function checkHome(menu)
      local labels, localActions, focusCount = {}, 0, 0
      local expected = {"LOCAL", "SYSTEM LINK", mode == "zombies" and "ZOMBIES MULTIPLAYER" or "MULTIPLAYER",
                        "OPTIONS", "EXTRAS", "CHECK FOR UPDATES"}
      if has("cp") then table.insert(expected, "CAMPAIGN") end
      if has(mode == "zombies" and "mp" or "zm") then
        table.insert(expected, mode == "zombies" and "MULTIPLAYER" or "ZOMBIES")
      end
      for _, button in ipairs(menu.buttonList.children) do
        assert(not button.closed and button.parent == menu.buttonList, "dangling home button")
        assert(button.label ~= "SPLITSCREEN" and button.label ~= "SPLIT SCREEN", "split-screen home entry remains")
        assert(not labels[button.label], "duplicate home label: " .. button.label)
        labels[button.label] = true
        if button.action == "open_local_match_lobby" then localActions = localActions + 1 end
        if button:isInFocus() then focusCount = focusCount + 1 end
      end
      assert(#menu.buttonList.children == #expected, "unexpected home button count")
      assert(menu.mainLobbyButton.label == "LOCAL" and localActions == 1, "LOCAL must be the only local-lobby action")
      for i, label in ipairs(expected) do assert(menu.buttonList.children[i].label == label, "home button order changed") end
      assert(menu.bo2zmSplitscreen == nil, "split-screen field remains")
      assert(menu.mainLobbyButton.hintText == "Local lobby hint", "local lobby hint was lost")
      assert(menu.mainLobbyButton.action == "open_local_match_lobby" and not menu.mainLobbyButton.disabled)
      assert(labels.OPTIONS and labels.EXTRAS and labels["CHECK FOR UPDATES"], "home tools were lost")
      assert(menu.optionsButton.action == "open_options_menu")
      assert((menu.spButton ~= nil) == has("cp") and (not menu.spButton or menu.spButton.action == "start_sp"),
          "CAMPAIGN does not follow the installed modes")
      local other = mode == "zombies" and menu.mpButton or menu.zombieButton
      assert((other ~= nil) == has(mode == "zombies" and "mp" or "zm"), "the other mode's button does not follow the installed modes")
      assert(not other or other.action == (mode == "zombies" and "start_mp" or "start_zombies"), "mode switch was changed")
      assert(menu.systemLinkButton.action == "open_system_link_flyout", "SYSTEM LINK lost the game's flyout")
      -- The flyout sits on SYSTEM LINK's row (the second) of the moved list, open or closed.
      local list, flyout, row = menu.buttonList, menu.systemLinkFlyoutContainer, CoD.CoD9Button.Height
      assert(list.top == -(row * (8 + 2)) - CoD.ButtonPrompt.Height, "home list did not gain its extra row")
      for _, step in ipairs({{"disabled", "show"}, {"default", "default"}, {"disabled", "show"}, {"default", "default"}}) do
        list:animateToState(step[1])
        flyout:animateToState(step[2])
        assert(list.top == -(row * (8 + 2)) - CoD.ButtonPrompt.Height and list.bottom == -CoD.ButtonPrompt.Height,
            "home list moved back to the stock place in state " .. step[1])
        assert(flyout.top == list.top + row + 2 and flyout.bottom == flyout.top + row * 2 + 2,
            "System Link flyout is off SYSTEM LINK's row in state " .. step[2])
      end
      local focused = menu[focusField or "mainLobbyButton"]
      assert(focusCount == 1 and focused:isInFocus(), "home focus was lost or duplicated")
      if focused == menu.mainLobbyButton then assert(focused.focusLabel == "LOCAL", "LOCAL focus label is stale") end
    end
    checkHome(main)
    for _, event in ipairs({"first_signed_in", "last_signed_out"}) do
      assert(main:processEvent({name = event, controller = 0}) == "stock sign-in handler", "stock sign-in handler was lost")
      checkHome(main)
    end
    assert(main.localButton.label == (mode == "zombies" and "ZOMBIES MULTIPLAYER" or "MULTIPLAYER"))
    assert(main.localButton.action == "bo2zm_open_online")
    -- Internet play: the popup's buttons follow the program's state; its lobby and browser are SYSTEM LINK's own,
    -- which, pressed from the flyout, end online play first.
    local function pressed(text)
      for _, line in ipairs(logs) do if line == text then return true end end
    end
    local function labels(popup)
      local names = {}
      for _, button in ipairs(popup.bo2zmOnlineButtons().children) do table.insert(names, button.label) end
      return table.concat(names, ",")
    end
    dvars.bo2zm_online_ui = 0
    main:processEvent({name = "bo2zm_open_online", controller = 0})
    local online = main.openedPopup
    assert(main.openedPopupName == "Bo2zmOnline" and online.bo2zmHome == main, "online popup did not open")
    assert(labels(online) == "HOST ONLINE GAME,JOIN WITH CODE", "online popup buttons: " .. labels(online))
    online:processEvent({name = "bo2zm_online_button", controller = 0,
      button = online.bo2zmOnlineButtons().children[1]})
    assert(pressed("bo2zm_online host"), "HOST ONLINE GAME sent nothing")
    dvars.bo2zm_online_ui = 1
    online:processEvent({name = "bo2zm_online_refresh"})
    assert(online.bo2zmOnlineButtons() == nil, "buttons while preparing")
    online:processEvent({name = "button_prompt_back", controller = 0})
    assert(pressed("bo2zm_online cancel"), "B while preparing did not cancel")
    dvars.bo2zm_online_ui, dvars.bo2zm_online_code_ui = 2, "AXQ3C-24E0G"
    online:processEvent({name = "bo2zm_online_refresh"})
    assert(labels(online) == "CONTINUE TO LOBBY,JOIN WITH CODE", "host-ready buttons: " .. labels(online))
    local offs = 0
    for _, line in ipairs(logs) do if line == "bo2zm_online off" then offs = offs + 1 end end
    online:processEvent({name = "bo2zm_online_button", controller = 0,
      button = online.bo2zmOnlineButtons().children[1]})
    assert(main.systemLinkLobbies == 1 and online.closed, "CONTINUE TO LOBBY did not open the System Link lobby")
    dvars.bo2zm_online_ui = 3
    main:processEvent({name = "bo2zm_open_online", controller = 0})
    online = main.openedPopup
    assert(labels(online) == "FIND GAME,JOIN WITH CODE,HOST ONLINE GAME", "join-ready buttons: " .. labels(online))
    online:processEvent({name = "bo2zm_online_button", controller = 0,
      button = online.bo2zmOnlineButtons().children[1]})
    assert(main.serverBrowsers == 1, "FIND GAME did not open the server browser")
    for _, line in ipairs(logs) do if line == "bo2zm_online off" then offs = offs - 1 end end
    assert(offs == 0, "the online popup's own lobby or browser ended online play")
    main:processEvent({name = "open_system_link_lobby", controller = 0})
    main:processEvent({name = "open_server_browser", controller = 0})
    assert(main.systemLinkLobbies == 2 and main.serverBrowsers == 2 and pressed("bo2zm_online off"),
        "SYSTEM LINK's own buttons did not end online play")
    dvars.bo2zm_online_ui = 0
    -- B closes the System Link flyout, then the home menu's Back is the port's again (prompt included).
    if focusField == "systemLinkButton" then
      main:processEvent({name = "button_prompt_back", controller = 0})
      assert(main.flyoutClosed == 1 and modeSwitches == 0, "B did not close the flyout the home menu opened with")
    end
    CoD.MainMenu.OpenSystemLinkFlyout(main)
    assert(main.backButton, "the flyout lost its Back prompt")
    main:processEvent({name = "button_prompt_back", controller = 0})
    assert(main.flyoutClosed == (focusField == "systemLinkButton" and 2 or 1) and modeSwitches == 0,
        "B did not close the System Link flyout")
    checkHome(main)
    main:processEvent({name = "button_prompt_back", controller = 0})
    local otherMode = mode == "zombies" and "mp" or "zm"
    local switchable = has("cp") or has(otherMode)
    assert(modeSwitches == (switchable and 1 or 0), "mode-picker Back behavior changed")
    assert((main.backButton ~= nil) == switchable, "dangling Back prompt")
    if switchable then
      -- The game's popup offers the installed modes only, and the home menu's own openPopup is back afterwards.
      assert(main.openedPopupName == "ConfirmLeave", "Back did not open the game's mode popup")
      local offered = {}
      for _, entry in ipairs(main.openedPopupOptions.params) do table.insert(offered, entry.leaveEvent) end
      local wanted = {}
      if has("cp") then table.insert(wanted, "start_sp") end
      if has(otherMode) then table.insert(wanted, mode == "zombies" and "start_mp" or "start_zombies") end
      assert(table.concat(offered, ",") == table.concat(wanted, ","), "mode popup offered " .. table.concat(offered, ","))
      assert(rawget(main, "openPopup") == nil, "the home menu kept the popup filter")
    end
    local wrappedMain = LUI.createMenu.MainMenu
    LUI.bo2zm.install()
    assert(LUI.createMenu.MainMenu == wrappedMain, "home menu wrapped twice")
    checkHome(LUI.createMenu.MainMenu(0))
    if testSpec and mode == "multiplayer" and testSpec ~= "mp_hijacked dm" then
      assert(not main.m_eventHandlers.bo2zm_testSolo, "Zombies specification drove Multiplayer")
      assert(not LUI.createMenu.SplitscreenGameLobby(0).m_eventHandlers.bo2zm_testStarted)
    else
      if testSpec then
        assert(main.m_eventHandlers.bo2zm_testSolo, "test did not enter LOCAL")
        main:processEvent({name = "bo2zm_testSolo", controller = 0})
      else
        assert(not main.m_eventHandlers.bo2zm_testSolo)
        main:processEvent({name = main.mainLobbyButton.action, controller = 0})
      end
      assert(main.openedMenuName == (mode == "zombies" and "SelectMapZM" or "SplitscreenGameLobby"), "LOCAL route changed")
      local lobby = main.openedMenu
      if testSpec and mode == "zombies" then
        assert(lobby.m_eventHandlers.bo2zm_testPicked, "Zombies map selection was not automated")
        lobby:processEvent({name = "bo2zm_testPicked", controller = 0})
        assert(lobby.closed and lobby.openedMenuName == "SplitscreenGameLobby", "Zombies local lobby was lost")
        lobby = lobby.openedMenu
        assert(lobby.previous == "MainMenu")
      end
      if testSpec then
        assert(lobby.m_eventHandlers.bo2zm_testStarted, "internal local lobby auto-start was lost")
        lobby:processEvent({name = "bo2zm_testStarted", controller = 0})
        lobby:processEvent({name = "bo2zm_testStarted", controller = 0})
        assert(starts == 1, "test match did not start exactly once")
        if mode == "multiplayer" then
          assert(dvars.gametype == "dm" and dvars.ui_mapname == "mp_hijacked")
          assert(dvars.ui_zm_gamemodegroup == nil and dvars.ui_zm_mapstartlocation == nil)
        else
          assert(dvars.gametype == "zstandard" and dvars.ui_mapname == "zm_transit")
          assert(dvars.ui_zm_gamemodegroup == "zsurvival" and dvars.ui_zm_mapstartlocation == "town")
        end
      else
        assert(not lobby.m_eventHandlers.bo2zm_testStarted and starts == 0, "manual LOCAL started a test")
      end
    end
  else
    assert(main.mainLobbyButton.label == "Xbox LIVE", "campaign mode picker was patched")
    if not inGame then
      -- The campaign's own mode menu (ui/t6/mainmenu.lua in its patch.ff): a carousel of MULTIPLAYER, SINGLEPLAYER
      -- and ZOMBIES nav buttons that starts on item 2.
      CoD.CarouselCommon = {
        SetStartItem = function(self, index) self.m_currentItem = math.min(index, #self.items) end,
        GetCurrentItemIndex = function(self) return self.m_currentItem end}
      CoD.VerticalCarousel = {AddItem = function(self, item) table.insert(self.items, item) end}
      local addItem, setStart = CoD.VerticalCarousel.AddItem, CoD.CarouselCommon.SetStartItem
      LUI.createMenu.MainMenu = function()
        local menu = object("MainMenu")
        local carousel = object("carousel")
        carousel.items = {}
        carousel.addItem = CoD.VerticalCarousel.AddItem
        carousel.setStartItem = CoD.CarouselCommon.SetStartItem
        carousel.getCurrentItemIndex = CoD.CarouselCommon.GetCurrentItemIndex
        menu:addElement(carousel)
        menu.carousel = carousel
        for _, spec in ipairs({{"MULTIPLAYER", "open_multiplayer"}, {"SINGLEPLAYER", "open_campaign"},
                               {"ZOMBIES", "open_zombies"}}) do
          local button = object(spec[1])
          button.actionEventName = spec[2]
          carousel:addItem(button)
        end
        carousel:setStartItem(2)
        return menu
      end
      LUI.bo2zm.install()
      local home = LUI.createMenu.MainMenu(0)
      local labels, wanted = {}, {}
      for _, item in ipairs(home.carousel.items) do table.insert(labels, item.label) end
      if has("mp") then table.insert(wanted, "MULTIPLAYER") end
      table.insert(wanted, "SINGLEPLAYER")
      if has("zm") then table.insert(wanted, "ZOMBIES") end
      assert(table.concat(labels, ",") == table.concat(wanted, ","), "campaign mode menu: " .. table.concat(labels, ","))
      assert(home.carousel.items[home.carousel.m_currentItem].label == "SINGLEPLAYER", "campaign mode menu start moved")
      assert(home.carousel:getCurrentItemIndex() == 2, "the game's numbering of SINGLEPLAYER changed")
      assert(CoD.VerticalCarousel.AddItem == addItem and CoD.CarouselCommon.SetStartItem == setStart,
          "the carousel's class functions were not restored")
    end
  end
  local extras = LUI.createMenu.Bo2zmExtras(0)
  local names = {}
  for _, row in ipairs(extras.buttonList.children) do names[row.label] = true end
  if not inGame then
    local positions = {}
    for i, row in ipairs(extras.buttonList.children) do positions[row.label] = i end
    assert(positions["EDIT USERNAME"] < positions.EXPERIMENTAL, "username below Experimental")
    if mode == "zombies" then
      assert(positions["LOADING SCREEN VIDEO"] < positions["EDIT USERNAME"], "username above loading video")
    end
  end
  assert((names["EDIT USERNAME"] == true) == not inGame)
  assert(names.EXPERIMENTAL and names.DEBUG)
  assert(extras.buttonList.children[1].label == "SCRIPT LOADER", "Script Loader is not first")
  local installedCount = (has("cp") and 1 or 0) + (has("mp") and 1 or 0) + (has("zm") and 1 or 0)
  assert((names["DEFAULT GAMEMODE"] == true) == (not inGame and installedCount > 1),
      "default mode must appear on every home Extras page when more than one mode is installed")
  if names["DEFAULT GAMEMODE"] then
    local row
    for _, child in ipairs(extras.buttonList.children) do
      if child.label == "DEFAULT GAMEMODE" then row = child end
    end
    local choices, wanted = {}, {}
    for _, choice in ipairs(row.m_choices) do table.insert(choices, choice.label .. "=" .. tostring(choice.value)) end
    for i, spec in ipairs({{"cp", "CAMPAIGN"}, {"mp", "MULTIPLAYER"}, {"zm", "ZOMBIES"}}) do
      if has(spec[1]) then table.insert(wanted, spec[2] .. "=" .. (i - 1)) end
    end
    assert(table.concat(choices, ",") == table.concat(wanted, ","), "default mode choices: " .. table.concat(choices, ","))
    local last = row.m_choices[#row.m_choices]
    last.callback(row, true)
    assert(logs[#logs] == "bo2zm_default_mode " .. tostring(last.value), "default mode pick sent " .. tostring(logs[#logs]))
  end
  local scripts = LUI.createMenu.Bo2zmExtras_scripts(0)
  assert(scripts and scripts.buttonList, "Script Loader did not open in " .. mode)
  for _, command in ipairs(logs) do
    assert(command ~= "bo2zm_scripts_load" and command ~= "bo2zm_scripts_unload", "opening the loader changed active scripts")
  end
  local scriptNames = {}
  for _, row in ipairs(scripts.buttonList.children) do if row.label then scriptNames[row.label] = true end end
  assert(scriptNames["SELECT MOD"] and scriptNames["ACTIVE MOD"] and scriptNames.STATUS)
  assert(scriptNames[mode == "campaign" and "ENABLE FOR ZOMBIES" or "LOAD SELECTED + RESTART"])
  assert(scriptNames[mode == "campaign" and "DISABLE SCRIPTS" or "UNLOAD ALL + RESTART"])
  local scriptRows = {}
  for _, row in ipairs(scripts.buttonList.children) do if row.label then scriptRows[row.label] = row end end
  assert(#scriptRows["SELECT MOD"].m_choices == 3)
  assert(scriptRows["SELECT MOD"].m_choices[2].label == "Gr3Zz")
  assert(not scriptRows["ACTIVE MOD"]:isFocusable() and not scriptRows.STATUS:isFocusable())
  for _, spec in ipairs({{"PATH SOURCE", "INTERNAL (embedded paths)"}, {"BASE PATH", "Not used"},
      {"CHECK RESULT", "Not checked"}, {"SCAN RESULT", "2 matching mods, 2 setup issues"}}) do
    local row = assert(scriptRows[spec[1]], "missing status row: " .. spec[1])
    assert(not row:isFocusable() and row.disabled and #row.m_choices == 1, "status row is editable")
    assert(row.m_choices[1].label == spec[2], "wrong status text: " .. spec[1])
    assert(row.m_choices[1].callback(row, true) ~= true, "status choice accepted input")
  end
  assert(scriptRows["CHECK SCRIPTS"]:isFocusable() and scriptRows["SETUP ISSUES"]:isFocusable())
  if mode == "campaign" then
    assert(string.find(scriptRows["SELECT MOD"].hintText, "next Zombies launch", 1, true), "campaign manages an unsupported mode")
  else
    assert(string.find(scriptRows["SELECT MOD"].hintText, "Flat and nested", 1, true), "layout support is unexplained")
  end
  assert(string.find(scriptRows["CHECK SCRIPTS"].hintText, "INTERNAL uses stored names", 1, true))
  assert(string.find(scriptRows["CHECK SCRIPTS"].hintText, "FILES requires base", 1, true))
  assert(string.find(scriptRows[mode == "campaign" and "ENABLE FOR ZOMBIES" or "LOAD SELECTED + RESTART"].hintText,
      "ends the current match", 1, true), "load hint hides the match restart")
  scriptRows["SELECT MOD"].m_choices[2].callback(scriptRows["SELECT MOD"], true)
  assert(logs[#logs] == "bo2zm_script_pick 1")
  scripts.m_eventHandlers.bo2zm_page_button(scripts,
      {button = scriptRows[mode == "campaign" and "ENABLE FOR ZOMBIES" or "LOAD SELECTED + RESTART"], controller = 0})
  assert(logs[#logs] == "bo2zm_scripts_load")
  scripts.m_eventHandlers.bo2zm_page_button(scripts,
      {button = scriptRows[mode == "campaign" and "DISABLE SCRIPTS" or "UNLOAD ALL + RESTART"], controller = 0})
  assert(logs[#logs] == "bo2zm_scripts_unload")
  do
    local commandsBefore = #logs
    local queuedBefore = #queuedCommands
    local home = object("home")
    local function rows(menu)
      local result = {}
      for _, row in ipairs(menu.buttonList.children) do
        if row.label then
          assert(#row.label <= 127, "button label overflows the menu's text limit")
          result[row.label] = row
        end
      end
      return result
    end
    local function press(menu, label)
      local button = assert(rows(menu)[label], "missing button: " .. label)
      assert(button:isFocusable(), "detail or navigation button is unfocusable: " .. label)
      menu:processEvent({name = button.action, button = button, controller = 0})
    end
    local function navigate(menu, label, page)
      menu.occludedMenu = home
      press(menu, label)
      local result
      if mode == "zombies" and not inGame then
        assert(home.openedPopupName == "Bo2zmExtras_" .. page, "wrong popup page")
        result = home.openedPopup
        result:processEvent({name = "button_prompt_back", controller = 0})
        assert(home.openedPopupName == "Bo2zmExtras_scripts", "Back did not return to Script Loader")
        -- Reopen after checking Back so subsequent interactions use a live page.
        result = home:openPopup("Bo2zmExtras_" .. page, 0)
      else
        assert(menu.openedMenuName == "Bo2zmExtras_" .. page, "wrong menu page")
        result = menu.openedMenu
        assert(result.previous == "Bo2zmExtras_scripts", "wrong parent page")
      end
      assert(menu.closed, "navigation left the old page open")
      return result
    end
    assert(LUI.createMenu.Bo2zmExtras_script_targets and LUI.createMenu.Bo2zmExtras_script_issues,
        "script subpages were not registered in " .. mode)
    local targets = navigate(LUI.createMenu.Bo2zmExtras_scripts(0), "CHECK SCRIPTS", "script_targets")
    assert(logs[#logs] == "bo2zm_scripts_check", "target preview did not run an immediate check")
    local targetRows = rows(targets)
    assert(targetRows["CHECK RESULT"].m_choices[1].label == checkResult, "preview read stale check results")
    assert(not targetRows["CHECK RESULT"]:isFocusable())
    assert(targetRows["SCRIPT 1"] and targetRows["SCRIPT 2"] and not targetRows["SCRIPT 3"])
    assert(targetRows["CHECK SCRIPTS"].focused, "target page did not focus the check button")
    assert(#scriptSources[2] > 127 and #scriptTargets[2] > 127, "long-path fixture is too short")
    for i = 1, 2 do
      press(targets, "SCRIPT " .. i)
      assert(targets.openedPopupName == "Error" and targets.openedPopup.anyControllerAllowed)
      assert(targets.openedPopup.message == "SOURCE\n" .. scriptSources[i] .. "\n=> REPLACEMENT TARGET\n" .. scriptTargets[i],
          "source or exact replacement target was truncated or changed")
      targets.openedPopup:close()
      assert(not targets.closed, "closing a detail popup closed the target list")
    end
    local issues = navigate(LUI.createMenu.Bo2zmExtras_scripts(0), "SETUP ISSUES", "script_issues")
    assert(logs[#logs] == "bo2zm_scripts_refresh", "setup page did not scan immediately")
    local issueRows = rows(issues)
    assert(issueRows["SCAN RESULT"].m_choices[1].label == "2 matching mods, 2 setup issues")
    assert(not issueRows["SCAN RESULT"]:isFocusable() and issueRows["REFRESH LIST"].focused)
    assert(issueRows["ISSUE 1"] and issueRows["ISSUE 2"] and not issueRows["ISSUE 3"], "issues differ from the scan")
    for i = 1, 2 do
      press(issues, "ISSUE " .. i)
      assert(issues.openedPopup.message == setupIssues[i], "setup issue lost the folder or reason")
      issues.openedPopup:close()
    end
    assert(string.find(issueRows["REFRESH LIST"].hintText, "another gamemode are not setup issues", 1, true))
    setupIssues[1] = string.rep("folder/", 24) .. ": invalid mod.ini: missing base path"
    issues = navigate(issues, "REFRESH LIST", "script_issues")
    press(issues, "ISSUE 1")
    assert(issues.openedPopup.message == setupIssues[1], "long setup issue was truncated")
    issues.openedPopup:close()
    for _, route in ipairs({{"FILES (folder paths)", "maps/mp/zombies"}, {"FILES (folder paths)", "Not used"},
        {"INTERNAL (embedded paths)", "Not used"}, {"No mod selected", "Not used"}}) do
      dvars.bo2zm_scripts_route_ui, dvars.bo2zm_scripts_base_ui = route[1], route[2]
      local loader = LUI.createMenu.Bo2zmExtras_scripts(0)
      local loaderRows = rows(loader)
      assert(loaderRows["PATH SOURCE"].m_choices[1].label == route[1])
      assert(loaderRows["BASE PATH"].m_choices[1].label == route[2])
      dvars.bo2zm_scripts_base_ui = "maps/mp/normalized_base"
      loader:processEvent({name = "bo2zm_refresh_selectors", controller = 0})
      assert(loaderRows["BASE PATH"].m_choices[1].label == "maps/mp/normalized_base", "base path status did not refresh")
    end
    for _, count in ipairs({0, -1, 99}) do
      targetCount, issueCount = count, count
      checkResult = count == 99 and "Ready: many scripts" or "Check failed: no valid scripts"
      targets = navigate(targets, "CHECK SCRIPTS", "script_targets")
      targetRows = rows(targets)
      assert(targetRows["CHECK RESULT"].m_choices[1].label == checkResult, "check error was hidden")
      assert((targetRows["SCRIPT 1"] ~= nil) == (count > 0), "stale or missing targets")
      assert((targetRows["SCRIPT 32"] ~= nil) == (count > 0) and not targetRows["SCRIPT 33"], "target count was not clamped")
      issues = navigate(issues, "REFRESH LIST", "script_issues")
      issueRows = rows(issues)
      assert((issueRows["ISSUE 1"] ~= nil) == (count > 0), "stale or invented issues")
      assert((issueRows["ISSUE 32"] ~= nil) == (count > 0) and not issueRows["ISSUE 33"], "issue count was not clamped")
    end
    for i = commandsBefore + 1, #logs do
      assert(logs[i] == "bo2zm_scripts_check" or logs[i] == "bo2zm_scripts_refresh",
          "checking or viewing issues changed scripts: " .. logs[i])
    end
    assert(#queuedCommands == queuedBefore, "script checks queued a command instead of using ExecNow")
  end
  assert((names.CHEATS == true) == (mode == "zombies"), "wrong cheat visibility in " .. mode)
  assert((names["LOADING SCREEN VIDEO"] == true) == (mode == "zombies"))
  assert((LUI.createMenu.Bo2zmExtras_weapons ~= nil) == (mode == "zombies"))
  assert((LUI.createMenu.Bo2zmExtras_zombies ~= nil) == (mode == "zombies"))
  if mode ~= "zombies" then
    assert(not extras.popupBackground, "a Zombies popup material was used")
    assert(extras.previous == (inGame and (mode == "campaign" and "ObjectiveInfoMenu" or "class")
                              or (mode == "campaign" and "CampaignMenu" or "MainMenu")))
    assert(LUI.createMenu.HUD == originalHud and LUI.createMenu.Loading == originalLoading)
  end
  local graphics = LUI.createMenu.GraphicsSettings(0)
  local profileRows = 0
  for _, row in ipairs(graphics.buttonList.children) do
    if row.m_dvarName == "bo2zm_profile_ui" or row.m_dvarName == "bo2zm_profile_on_ui" then
      profileRows = profileRows + 1
    end
  end
  assert(profileRows == 2, "wrong graphics profile rows in " .. mode)
  if mode ~= "zombies" then
    for _, row in ipairs(graphics.buttonList.children) do
      if row.m_dvarName == "bo2zm_profile_ui" then
        assert(#row.m_choices == 1 and row.m_choices[1].label == "GLOBAL" and not row:isFocusable())
      elseif row.m_dvarName == "bo2zm_profile_on_ui" then
        assert(not row:isFocusable(), "non-Zombies profile was editable")
      end
    end
  end
  if inGame then
    local pause = object()
    pause.buttonList = object()
    if mode == "campaign" then
      CoD.ObjectiveInfoMenu.AddButton(pause.buttonList, "OPTIONS", "open_options")
    else
      CoD.Class.AddButton(pause, "OPTIONS", "open_options")
    end
    assert(pause.buttonList.children[2].label == "EXTRAS", "pause Extras was not added")
  end
  if mode == "zombies" and inGame then
    assert(loadfile("tools/tests/player_cheat_ui.lua"))()({dvars = dvars, commands = logs, object = object,
      methods = methods, reviewCase = reviewCase})
  elseif mode ~= "zombies" then
    for _, command in ipairs(logs) do
      assert(not string.find(command, "bo2zm_player ", 1, true), "player cheat transport used outside Zombies")
    end
  end
  for _, line in ipairs(logs) do assert(not string.find(line, "bo2zm_log error:", 1, true), line) end
  print(mode .. (inGame and " in match" or " front end") .. ": home focus, actions, Extras, profiles and wrapper scope passed")
end
-- Every combination of installed modes, in each installed mode's front end.
for _, installed in ipairs({"cp mp zm", "mp zm", "cp zm", "cp mp", "zm", "mp", "cp"}) do
  for _, spec in ipairs({{"zombies", "zm"}, {"multiplayer", "mp"}, {"campaign", "cp"}}) do
    if string.find(" " .. installed .. " ", " " .. spec[2] .. " ", 1, true) then
      run(spec[1], false, nil, nil, nil, installed)
    end
  end
end
for _, mode in ipairs({"zombies", "multiplayer", "campaign"}) do
  run(mode, false)
  run(mode, true)
end
run("multiplayer", false, "mp_hijacked dm")
run("multiplayer", false, "zm_transit town zstandard zsurvival")
run("zombies", false, "zm_transit town zstandard zsurvival")
for _, mode in ipairs({"zombies", "multiplayer"}) do
  for _, focusField in ipairs({"localButton", "systemLinkButton", "optionsButton", "spButton",
      mode == "zombies" and "mpButton" or "zombieButton"}) do
    run(mode, false, nil, focusField, true)
  end
end
