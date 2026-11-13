-- Exercise the actual bot-lobby patch with authored layout bounds; no game data or rendering.
local file = assert(io.open(arg[1] or "games/bo2/src/ui/menus.lua", "rb"))
local source = file:read("*a"):gsub("\r\n", "\n")
file:close()
local patch = assert(source:match("(local PatchBotsLobby\n;%(%s*function%(%)\n.-)\nlocal function WrapLobbyPopulate"))
local checks = 0

local function run(scale, origin, arrows, pending)
  local methods = {}
  local boundsReady = not pending
  local dvars = {ui_mapname = "zm_transit", ui_gametype = "zstandard", ui_zm_mapstartlocation = "town",
    bo2zm_bots_available_ui = 1, bo2zm_bots_count_ui = 0, bo2zm_bots_max_ui = 3}
  local function node(width)
    return setmetatable({width = width or 0, left = 0, children = {}, handlers = {}}, {__index = methods})
  end
  function methods:addElement(child)
    assert(not child.parent)
    child.parent = self
    table.insert(self.children, child)
  end
  function methods:removeElement(child)
    for i, current in ipairs(self.children) do
      if current == child then table.remove(self.children, i); child.parent = nil; return end
    end
    error("removing a non-child")
  end
  function methods:getFirstChild() return self.children[1] end
  function methods:getNextSibling()
    for i, child in ipairs(self.parent.children) do
      if child == self then return self.parent.children[i + 1] end
    end
  end
  function methods:rootLeft()
    if not self.parent then return origin + self.left * scale end
    local offset = self.left
    if self.parent.flow then
      offset = 0
      for _, child in ipairs(self.parent.children) do
        if child == self then break end
        offset = offset + child.width
      end
    end
    return self.parent:rootLeft() + offset * scale
  end
  function methods:getRect()
    if not boundsReady then return end
    local left = self:rootLeft()
    return left, 0, left + self.width * scale, 30 * scale
  end
  function methods:setLeftRight(leftAnchor, rightAnchor, left, right)
    assert(leftAnchor)
    self.left = left
    if not rightAnchor then self.width = right - left end
    self.layoutWrites = (self.layoutWrites or 0) + 1
  end
  function methods:setLayoutCached(value) self.layoutCached = value end
  function methods:registerEventHandler(event, handler) self.handlers[event] = handler end
  function methods:clearChoices() self.choices = {} end
  function methods:addChoice(controller, text, value) self.choices[#self.choices + 1] = {text, value} end
  function methods:refreshChoice()
    for _, choice in ipairs(self.choices) do
      if choice[2] == dvars.bo2zm_bots_count_ui or #self.choices == 1 then
        -- Tight choice text changes width when OFF/numbers/NOT INSTALLED changes.
        self.choiceText.width = #choice[1] * 13
      end
    end
  end
  function methods:updateChoice() self:refreshChoice() end
  function methods:disable() self.disabled = true end
  function methods:enable() self.disabled = false end

  local function selector(labelWidth, valueWidth)
    local row = node(500)
    row.horizontalList, row.label, row.choiceText = node(500), node(labelWidth), node(valueWidth)
    row.horizontalList.flow = true
    row:addElement(row.horizontalList)
    row.horizontalList:addElement(row.label)
    if arrows then row.leftArrow = node(25); row.horizontalList:addElement(row.leftArrow) end
    row.horizontalList:addElement(row.choiceText)
    if arrows then row.rightArrow = node(25); row.horizontalList:addElement(row.rightArrow) end
    return row
  end
  local function list()
    local result = node(500)
    result:addElement(node()) -- Non-selector rows must not become the reference.
    local stock = selector(185, 93)
    stock.m_settingName = "difficulty"
    stock.horizontalList.left = 35 -- A stock column is not just its label width or constructor gap.
    result.stock = stock
    result:addElement(stock)
    function result:addDvarLeftRightSelector()
      local row = selector(170, 1)
      self:addElement(row)
      return row
    end
    return result
  end
  local menu = node()
  menu.buttonPane = {body = {buttonList = list()}}
  local env = setmetatable({
    state = {zombies = function() return true end}, InGame = function() return false end,
    Engine = {ExecNow = function() end}, Protect = function(callback) return callback end,
    SteadyButton = function() end,
    ApplyStatus = function(row, status) row.disabled = status.status end,
    OnEvent = function(row, event, handler) row:registerEventHandler(event, handler) end,
    UIExpression = {
      DvarString = function(_, name) return dvars[name] or "" end,
      DvarInt = function(_, name) return dvars[name] or 0 end,
    },
    LUI = {UITimer = {new = function() return node() end}},
  }, {__index = _G})
  local chunk = assert(loadstring(patch .. "\nreturn PatchBotsLobby", "bot lobby placement"))
  setfenv(chunk, env)
  local apply = chunk()
  apply(menu, 0)
  boundsReady = true
  local function refresh() menu.handlers.bo2zm_refresh_bots(menu) end
  local function aligned()
    local body = menu.buttonPane.body
    local row, stock = body.bo2zmBotCount, body.buttonList.stock
    local first = row.leftArrow or row.choiceText
    local reference = stock.leftArrow or stock.choiceText
    assert(math.abs(first:rootLeft() - reference:rootLeft()) < 0.00001, "bot controls left the stock column")
    assert(row.label.parent == row and row.horizontalList.children[1] == first, "label still flows with choices")
    checks = checks + 1
    return row, stock
  end
  refresh()
  for _, glyphWidth in ipairs({1, 70, 350}) do
    for _, count in ipairs({0, 1, 2, 3, 0}) do
      local row = menu.buttonPane.body.bo2zmBotCount
      row.label.glyphWidth = glyphWidth -- Regular label text does not determine its allocated span.
      dvars.bo2zm_bots_count_ui = count
      refresh()
      aligned()
    end
  end
  dvars.bo2zm_bots_available_ui = 0
  refresh()
  assert(aligned().disabled, "missing-pack row was unlocked")
  dvars.bo2zm_bots_available_ui = 1
  refresh()
  local row, stock = aligned()
  assert(not row.disabled)
  local writes = row.horizontalList.layoutWrites
  refresh()
  aligned()
  assert(row.horizontalList.layoutWrites == writes, "unchanged layout was rewritten")
  stock.horizontalList.left = 63 -- Follow a changed stock column, rather than a hard-coded gap.
  refresh()
  aligned()
  menu.buttonPane.body = {buttonList = list()}
  apply(menu, 0)
  aligned()
end

for _, scale in ipairs({0.75, 1, 1.5, 2}) do
  for _, origin in ipairs({0, 208, -50}) do
    for _, arrows in ipairs({true, false}) do
      run(scale, origin, arrows, false)
      run(scale, origin, arrows, true)
    end
  end
end
print("Bot selector placement: " .. checks .. " layout checks passed; screenshots remain a game check")
