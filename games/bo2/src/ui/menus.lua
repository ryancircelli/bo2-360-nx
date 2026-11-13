-- bo2zm: the port's changes to the game's menus.
--
-- The game builds its menus in Lua (HavokScript) from scripts stored in its fastfiles. This file is the port's own:
-- CMake embeds it in the executable as text (app/CMakeLists.txt, bo2zm_ui_lua.h), and bo2zm_ui.cpp runs it inside the
-- menu system's Lua state as soon as that state exists. None of the game's files are modified. The file must not
-- contain the at sign, which the CMake template uses as its delimiter.
--
-- Keep the number of top-level locals small: app/CMakeLists.txt fails the build above 80 "local" lines at the left
-- margin. The game's Lua compiler does not enforce a limit on a function's active local variables; past about 95 it
-- silently overwrites the menu heap, and the game crashed right after this file's second install once the main chunk
-- reached 97 (92 was fine). The check was probably compiled out of the shipped game, which contains none of Lua 5.1's
-- limit messages. Helpers that only one section needs therefore live in a do ... end block that exposes only what the
-- rest of the file uses. `python -I tools/check_menus_lua.py` checks the syntax, the globals read and written (a local
-- used above its declaration reads a global instead) and the local count with the Lua 5.1 compiler.
--
-- How it works: each menu is built by a function in LUI.createMenu (MainMenu for the Zombies home menu,
-- GraphicsSettings for Options > Graphics, class for the pause menu in a match). We wrap those functions: the game
-- builds its menu as usual, then the wrapper edits the result. The game reloads its menu scripts (a match loads its
-- own copies), which replaces the wrappers, so Install runs after every require and once a second from the program
-- (bo2zm_ui.cpp) and wraps whatever is not wrapped yet. The menu system refuses new global variables ("Tried to create
-- global variable"), so our state lives in LUI.bo2zm and require is replaced through rawset.
--
-- The game's menu code this relies on was read with tools/inspect/hks_dis.py: ui/t6/buttonlist.lua,
-- leftrightselector.lua, dvarleftrightselector.lua, codmenu.lua, graphicssettingsoptions.lua, ui_mp/t6/mainmenu.lua,
-- hud/class.lua and menus/privategamelobby.lua in patch_zm.ff and patch_ui_zm.ff.
--
-- Talking to the program: Engine.ExecNow(controller, "bo2zm_<command> ...") runs one of the port's console commands
-- (bo2zm_commands.cpp), and the program reports its settings back through game dvars that the menus read.
-- Engine.Exec would only queue the text, where that hook never sees it.

-- Returns true once installed; false while the menu system is not ready yet (the program tries again later).
local LUI = rawget(_G, "LUI")
if not (LUI and LUI.createMenu and rawget(_G, "require") and rawget(_G, "Engine") and rawget(_G, "UIExpression")) then
  return false
end
if rawget(LUI, "bo2zm") then
  return true
end
local state = {wrappers = {}}
rawset(LUI, "bo2zm", state)

-- Each mode keeps its own front end. Zombies-only options require the game's explicit Zombies flag.
state.campaign = function()
  local CoD = rawget(_G, "CoD")
  return CoD ~= nil and CoD.isSinglePlayer == true
end
state.zombies = function()
  local CoD = rawget(_G, "CoD")
  return CoD ~= nil and CoD.isZombie == true and not state.campaign()
end

-- The installed modes, "cp", "mp" and "zm": the program lists them in bo2zm_modes_ui (bo2zm_extras.cpp; a player may
-- install any of the three). Every place that picks a mode offers these only. Until the program has set the list,
-- Zombies and Multiplayer count as installed and Campaign as bo2zm_campaign_ui says.
state.modeInstalled = function(mode)
  local list = UIExpression.DvarString(nil, "bo2zm_modes_ui")
  if list == nil or list == "" then
    return mode ~= "cp" or UIExpression.DvarInt(nil, "bo2zm_campaign_ui") == 1
  end
  return string.find(" " .. list .. " ", " " .. mode .. " ", 1, true) ~= nil
end
-- Whether another installed mode can be switched to from this one.
state.otherModeInstalled = function()
  local current = state.campaign() and "cp" or (state.zombies() and "zm" or "mp")
  for _, mode in ipairs({"cp", "mp", "zm"}) do
    if mode ~= current and state.modeInstalled(mode) then return true end
  end
  return false
end
-- An item with modes = true picks a mode (its choices are Campaign, Multiplayer, Zombies, values 0-2): the copy the
-- page shows keeps the installed ones, and has no label (so the page leaves it out) when only one is.
state.modeItem = function(item)
  local choices, values = {}, {}
  for i, mode in ipairs({"cp", "mp", "zm"}) do
    if state.modeInstalled(mode) then
      table.insert(choices, item.choices[i])
      table.insert(values, i - 1)
    end
  end
  if #choices < 2 then return {where = item.where} end
  return {label = item.label, hint = item.hint, setting = item.setting, where = item.where, choices = choices,
          values = values}
end

-- Declared first: a local is only visible below its declaration, and a function that calls one declared further
-- down reads an undefined global instead.

-- A line in the program's log, as "[lua] text".
local function Log(text)
  local clean = string.gsub(tostring(text), "[;\"\n]", " ")
  Engine.ExecNow(0, "bo2zm_log " .. clean)
end

-- Runs f(...) and logs the first few errors instead of breaking the game's menu. Returns f's first result.
local errors = 0
local function Safely(f, ...)
  local ok, result = pcall(f, ...)
  if ok then
    return result
  end
  if errors < 10 then
    errors = errors + 1
    Log("error: " .. tostring(result))
  end
end

---------------------------------------------------------------------------------------------------------------------
-- Extras pages (EXTRAS on the Zombies home menu and in the pause menu). Each page is a list of items, in order:
--   {section = "TITLE"}
--       a heading.
--   {label, hint, page = "name"}
--       a button that opens another page (they open on top; Back returns).
--   {label, hint, action = "name"}
--       a button that runs Actions[name] (with the item, so arg can say more). With labelDvar = "dvar", its label is
--       that game dvar's text, read again four times a second (the program's, e.g. PURGE DEBUG INFO's counts).
--   A page may have parent = "page" (Back returns there, not to the main page) and opened = "command" (run when it
--   opens).
--   A setting item with apply = "command" only takes the pick with left and right; A runs the command.
--   A setting item with status = true is a status row: it only shows the program's value (see the rule below).
--   A setting item with statusChoice = n has a choice that only reports a state (N/A): left and right skip it, and
--   while the program shows it the row is a status row.
-- An item with where = "home" shows only on the home menu's page, where = "game" only in a match's.
-- An item with zm = true is shown only in Zombies: the cheats and the loading video.
-- An item with modes = true picks a mode: it offers the installed modes only, and is left out when one is installed
-- (state.modeItem).
-- An item with cheat = true works only while CHEATS is on (every setting and button of the cheat pages is one, except
-- those with pref = true). Otherwise a pick or a press shows a message and changes nothing. The DEBUG page has no
-- such lock: DEBUG is a master switch in the program (bo2zm_debug_options.cpp), so its options can be set while it is
-- off and only apply once it is on.
--   {label, hint, setting = "name", choices = {...}}
--       one of the port's settings (bo2zm_extras.cpp): the menu shows the game dvar bo2zm_<name>_ui, which the
--       program keeps up to date (0, 1, ... = the choice), and a pick runs "bo2zm_<name> <index>". With values, the
--       choices are those values instead of 0, 1, ... (DEFAULT GAMEMODE without the modes that are not installed).
--       Cheat settings use bo2zm_p<controller>_<name>_ui and send bo2zm_player <controller> before the command.
--   {label, hint, dvar = "name", choices = {...}, values = {...}, normal = n}
--       one of the game's dvars (the built-in cheats: tools/inspect/dvar_list.py --cheat lists them). values gives
--       each choice's value; choice n (1 if not given) is the game's own and gives the dvar back to the game, any
--       other asks the program to hold the dvar at that value (bo2zm_cheats.cpp: it puts it back after map loads and
--       scripts, whatever sv_cheats is). These engine dvars are shared; bo2zm_hold_<dvar> keeps each pick, which the
--       selector shows (the home menu and the pause menu alike); before the first pick it shows choice n.
--
-- The menu script never runs console commands other than the program's own (bo2zm_*), and every function it hands to
-- the menu system is protected: an error there would reach the game's handler, which resets the menus with
-- "UI Error <number>" (bo2zm_ui.cpp logs the message).
--
-- The rule for every row of these pages and of Options > Graphics: a row the player cannot change is never a selector
-- that moves and snaps back. It is a status row (StatusRow below): left and right do nothing, the cursor skips it as
-- it skips the headings, and it is drawn in the game's disabled color. A row that needs a hint to be understood is not
-- a status row; put the explanation in the hint of the row it depends on, since a status row's own hint never shows.
-- A new row that shows a value the player cannot pick gets status = true (or statusChoice when only one choice is
-- such), so no page has rows that seem to take input and then do nothing.
---------------------------------------------------------------------------------------------------------------------
local OnOff = {"OFF", "ON"}
local Off1On = {0, 1}

-- The round picker's choices: every round to 30, then every 5th to 100, every 10th to 250, and 255 (the same list as
-- kRounds in bo2zm_extras.cpp).
local RoundChoices = {}
for round = 1, 30 do
  table.insert(RoundChoices, tostring(round))
end
for round = 35, 100, 5 do
  table.insert(RoundChoices, tostring(round))
end
for round = 110, 250, 10 do
  table.insert(RoundChoices, tostring(round))
end
table.insert(RoundChoices, "255")

local ExtrasPages = {
  main = {
    title = "EXTRAS",
    {label = "SCRIPT LOADER", hint = "Choose or unload a script mod.",
     page = "scripts"},
    {label = "DEFAULT GAMEMODE", hint = "Choose the mode opened at launch.",
     setting = "default_mode", choices = {"CAMPAIGN", "MULTIPLAYER", "ZOMBIES"}, where = "home", modes = true},
    {zm = true, label = "LOADING SCREEN VIDEO", hint = "Show a video instead of the loading image.", setting = "loading_video",
     choices = {"DISABLED", "ENABLED"}},
    {label = "EDIT USERNAME", hint = "Change your player name.", action = "edit_name",
     where = "home", nameHint = true},
    {label = "KEYBOARD AND MOUSE", hint = "ON: the keyboard and mouse play as player 1: WASD move, mouse looks, left click fires, right click "
       .. "aims, Space jump, C crouch, R reload/use, 2 weapon swap, G grenade, 1 special grenade, V melee, F sprint, "
       .. "Return pause, Tab scores. Controllers keep working.", setting = "keyboard", choices = OnOff, platform = "pc"},
    {label = "EXPERIMENTAL", hint = "Features still being tested.", page = "experimental", platform = "switch"},
    {zm = true, section = "CHEATS"},
    {zm = true, label = "CHEATS", hint = "ON: the cheats work. OFF: normal play; your cheat picks are kept and come back when "
       .. "you turn CHEATS on.", setting = "cheats", choices = OnOff},
    {zm = true, label = "GOD MODE", hint = "You take no damage.", setting = "god", choices = OnOff, cheat = true},
    {zm = true, label = "INFINITE AMMO", hint = "Your clip stays full.", setting = "ammo", choices = OnOff, cheat = true},
    {zm = true, label = "EXPLOSIVE BULLETS", hint = "Your bullets explode where they hit. The blasts never hurt you.",
     setting = "explosive", choices = OnOff, cheat = true},
    {zm = true, label = "NO RECOIL", hint = "Shooting does not kick your aim.", setting = "norecoil", choices = OnOff,
     cheat = true},
    {zm = true, label = "GIVE MAX POINTS", hint = "Sets your points to 1,000,000.", action = "max_points", where = "game",
     cheat = true},
    {zm = true, label = "GAME SPEED", hint = "Slow motion or fast forward, for everything in the match.",
     dvar = "timescale", choices = {"VERY SLOW", "SLOW", "NORMAL", "FAST", "VERY FAST"},
     values = {0.25, 0.5, 1, 1.5, 2}, normal = 3, cheat = true},
    {zm = true, label = "MOVEMENT", hint = "Speed, jumping, gravity, noclip and UFO.", page = "movement"},
    {zm = true, label = "CAMERA", hint = "Third person, field of view, the gun and the HUD.", page = "camera"},
    {zm = true, label = "ZOMBIES", hint = "Make zombies ignore you, freeze them, stop spawns or kill them all.", page = "zombies"},
    {zm = true, label = "POWER UPS", hint = "Spawn power-ups in front of you. Only the ones this map has appear.",
     page = "powerups", where = "game"},
    {zm = true, label = "PERKS", hint = "Get any perk, and keep your perks when you go down.", page = "perks"},
    {zm = true, label = "WEAPONS", hint = "Get any gun this map has in place of the one in your hands, Pack-a-Punched or not.",
     page = "weapons", where = "game"},
    {zm = true, label = "DOORS + POWER", hint = "Open or close every door, turn the power on or off.", page = "doors_power",
     where = "game"},
    {zm = true, label = "RESET CHEATS", hint = "Every cheat back to normal play, kept picks too. The combo's settings stay.",
     action = "reset_cheats"},
    {section = "BUGS"},
    {label = "DEBUG", hint = "Help find a bug: debug presets, mark the log, take screenshots.", page = "debug"},
  },
  scripts = {},
  script_targets = {},
  script_issues = {},
  experimental = {
    title = "EXPERIMENTAL",
    {label = "CPU CORES", hint = "4: the game also uses the Switch's 4th CPU core (faster in busy matches). Works only "
       .. "when the game is started from a forwarder made in sphaira with CPU Cores: 4. Applies at once. IN USE "
       .. "shows the cores in use; SWITCHED OFF: the 4th core was overloaded, so the game went back to 3 (pick 4 to "
       .. "try again).", setting = "cpu_cores", choices = {"3", "4"}},
    -- A status row: CPU CORES's hint explains it.
    {label = "IN USE", setting = "cpu_cores_in_use",
     choices = {"3 CORES", "4 CORES", "3 CORES (NO 4-CORE LAUNCH)", "3 CORES (SWITCHED OFF)"}, status = true},
  },
  movement = {
    title = "MOVEMENT",
    {label = "MOVE SPEED", hint = "How fast you walk and run (each player picks their own).",
     setting = "speed", choices = {"NORMAL", "FAST", "FASTER", "FASTEST"}},
    {label = "UNLIMITED SPRINT", hint = "Sprint without running out of breath. For everyone in the match.",
     dvar = "player_sprintUnlimited", choices = OnOff, values = Off1On},
    {label = "JUMP HEIGHT", hint = "How high you jump. Use god mode for the high ones: landings hurt.",
     setting = "jump", choices = {"NORMAL", "HIGH", "HIGHER", "HIGHEST", "INSANE", "EXTREME"}},
    {label = "FEATHER FALL", hint = "Fall slowly and steer while falling, to glide from a high jump.",
     setting = "feather", choices = OnOff},
    {label = "GRAVITY", hint = "Lower gravity makes jumps and falls float. For everyone in the match.",
     dvar = "bg_gravity", choices = {"MOON", "LOW", "NORMAL", "HIGH"}, values = {150, 400, 800, 1200}, normal = 3},
    {section = "FLY"},
    {label = "NOCLIP", hint = "Fly with the stick, triggers for up and down. One fly mode at a time.",
     setting = "noclip", choices = OnOff},
    {label = "UFO", hint = "Fly where the camera looks and float to a stop. One fly mode at a time.",
     setting = "ufo", choices = OnOff},
    {label = "COLLISION", hint = "Whether walls stop noclip and UFO.",
     setting = "collision", choices = {"DISABLED", "ENABLED"}, pref = true},
    {label = "COMBO", hint = "Aim + D-pad Down turns the combo's fly mode on and off (while CHEATS is on).",
     setting = "combo", choices = {"DISABLED", "ENABLED"}, pref = true},
    {label = "COMBO SETS", hint = "The fly mode the combo turns on.", setting = "combo_mode", choices = {"NOCLIP", "UFO"},
     pref = true},
  },
  camera = {
    title = "CAMERA",
    {label = "THIRD PERSON", hint = "The camera follows you from behind (the game's own third person).",
     setting = "third", choices = OnOff},
    {label = "TPP CROSSHAIR", hint = "A crosshair in third person. Aiming moves the camera over your shoulder.",
     setting = "third_crosshair", choices = {"DISABLED", "WHEN AIMING", "ALWAYS"}, pref = true},
    {label = "TPP CROSSHAIR TYPE", hint = "The third person crosshair's shape.", setting = "tpp_type",
     choices = {"DOT", "SMALL CROSS"}, pref = true},
    {label = "TPP CROSSHAIR COLOR", hint = "The third person crosshair's color.", setting = "tpp_color",
     choices = {"WHITE", "RED", "GREEN", "CYAN", "YELLOW", "MAGENTA", "ORANGE"}, pref = true},
    {label = "FIELD OF VIEW", hint = "How wide the camera sees, in degrees. The game's is 65. RESET CHEATS puts it back.",
     setting = "fov", choices = {"55", "60", "65", "70", "75", "80", "85", "90", "95", "100", "105", "110"}},
    {label = "GUN", hint = "Show or hide your gun and hands.",
     dvar = "cg_drawGun", choices = {"SHOWN", "HIDDEN"}, values = {1, 0}},
    {label = "HUD", hint = "Show or hide the HUD (points, ammo, round).", setting = "hud", choices = {"SHOWN", "HIDDEN"}},
  },
  zombies = {
    title = "ZOMBIES",
    {label = "ZOMBIES IGNORE YOU", hint = "Zombies leave you alone (each player picks their own).", setting = "zm_ignore",
     choices = {"DISABLED", "ENABLED"}},
    {label = "ZOMBIES FREEZE", hint = "Every zombie stops where it is. For everyone in the match.",
     dvar = "g_ai", choices = {"DISABLED", "ENABLED"}, values = {1, 0}},
    {label = "STOP SPAWNS", hint = "No new zombies appear. The round waits until spawns are back. For everyone in the match.",
     setting = "zm_nospawn", choices = {"DISABLED", "ENABLED"}},
    {label = "KILL ALL ZOMBIES", hint = "Kills every zombie now. The kills are yours.", action = "kill_zombies",
     where = "game"},
    {label = "ROUND", hint = "Pick a round and press A: this round ends and the next one is the round you picked.",
     setting = "round", choices = RoundChoices, apply = "bo2zm_goto_round", where = "game"},
  },
  perks = {
    title = "PERKS",
    {label = "KEEP PERKS", hint = "Keep your perks when you go down and get revived (or respawn).",
     setting = "keep_perks", choices = {"DISABLED", "ENABLED"}},
    {label = "REMOVE PERKS", hint = "Press A to lose every perk you have.", action = "remove_perks", where = "game"},
    {label = "CROSS-MAP PERKS", hint = "ENABLED also lists the perks of other maps' machines, marked OTHER MAP: each one "
       .. "says how much of it works here. Open this page again to see the list change.", setting = "cross_perks",
     choices = {"DISABLED", "ENABLED"}, pref = true},
    -- A perk shows when this map has its machine (bo2zm_map_perks_ui), or with CROSS-MAP PERKS; `other` says how
    -- much of it works on a map without the machine.
    {label = "JUGGERNOG", hint = "Press A to get it.", action = "perk", arg = "specialty_armorvest", where = "game",
     other = "Works here (more health), without its HUD icon."},
    {label = "QUICK REVIVE", hint = "Press A to get it.", action = "perk", arg = "specialty_quickrevive", where = "game",
     other = "Works here (faster revives; solo: one self-revive), without its HUD icon."},
    {label = "SPEED COLA", hint = "Press A to get it.", action = "perk", arg = "specialty_fastreload", where = "game",
     other = "Works here (faster reloads), without its HUD icon."},
    {label = "DOUBLE TAP", hint = "Press A to get it.", action = "perk", arg = "specialty_rof", where = "game",
     other = "Works here (faster fire), without its HUD icon."},
    {label = "STAMIN-UP", hint = "Press A to get it.", action = "perk", arg = "specialty_longersprint", where = "game",
     other = "Works here (longer sprint), without its HUD icon."},
    {label = "PHD FLOPPER", hint = "Press A to get it.", action = "perk", arg = "specialty_flakjacket", where = "game",
     other = "In part: explosions and falls stop hurting you, but the dive-to-prone blast is only in its own map's files."},
    {label = "DEADSHOT DAIQUIRI", hint = "Press A to get it.", action = "perk", arg = "specialty_deadshot", where = "game",
     other = "Works here (steadier aim), without its HUD icon."},
    {label = "MULE KICK", hint = "Press A to get it.", action = "perk", arg = "specialty_additionalprimaryweapon",
     where = "game", other = "Works here (a third gun), without its HUD icon."},
    {label = "TOMBSTONE", hint = "Press A to get it.", action = "perk", arg = "specialty_scavenger", where = "game",
     other = "Does nothing here: its script only runs on a map that has the machine."},
    {label = "WHO'S WHO", hint = "Press A to get it.", action = "perk", arg = "specialty_finalstand", where = "game",
     other = "Does nothing here: its script only runs on a map that has the machine."},
    {label = "ELECTRIC CHERRY", hint = "Press A to get it.", action = "perk", arg = "specialty_grenadepulldeath",
     where = "game", other = "Does nothing here: its script only runs on a map that has the machine."},
    {label = "VULTURE AID", hint = "Press A to get it.", action = "perk", arg = "specialty_nomotionsensor", where = "game",
     other = "Does nothing here: its script is only in Buried's files."},
  },
  doors_power = {
    title = "DOORS + POWER",
    {label = "OPEN ALL DOORS", hint = "Opens every door and clears every debris pile that is bought, for free "
       .. "(Mob of the Dead's Afterlife doors too).", action = "doors", arg = "open", where = "game"},
    {label = "CLOSE ALL DOORS", hint = "Closes every open door the game's way (zombies cannot pass); you can buy them "
       .. "again. Cleared debris stays cleared (the game removes it), and doors the power opens stay open.",
     action = "doors", arg = "close", where = "game"},
    {label = "POWER ON", hint = "Turns the power on: perk machines, Pack-a-Punch and electric doors.", action = "power",
     arg = "on", where = "game", mapHints = {
       zm_tomb = "Captures every generator: each zone's perk machines, random perk machine and box come on.",
       zm_prison = "Mob of the Dead has no power to turn on: its machines always work."}},
    {label = "POWER OFF", hint = "Turns the power off: perk machines and Pack-a-Punch stop working until POWER ON. "
       .. "Doors stay as they are.", action = "power", arg = "off", where = "game", mapHints = {
       zm_tomb = "Every generator is lost to the zombies: each zone's machines and box go off until POWER ON.",
       zm_prison = "Mob of the Dead has no power to turn off: its machines always work."}},
    {label = "REMOVE FREE DEBRIS", hint = "Clears what opens without buying it: Buried's barricades that Leroy breaks. "
       .. "The areas behind them open. Only Buried has them.", action = "debris", arg = "remove", where = "game"},
    {label = "PUT DEBRIS BACK", hint = "Puts the barricades REMOVE FREE DEBRIS cleared back, solid again (not the ones "
       .. "Leroy broke: the game deletes those).", action = "debris", arg = "restore", where = "game"},
  },
  -- The guns (bo2zm_cheats.cpp's catalog); CreateWeaponsPage builds the page.
  weapons = {
    title = "WEAPONS",
    custom = true,
  },
  -- DEBUG (bo2zm_debug_options.cpp): what a player can be asked to turn on to help find a bug. DEBUG is the master
  -- switch: every setting below can be changed while it is off and applies only while it is on.
  debug = {
    title = "DEBUG",
    {label = "DEBUG", hint = "OFF: the game plays normally, whatever is picked below (you can still set it up). ON: "
       .. "the debug options below apply.", setting = "dbg", choices = OnOff},
    {label = "DEBUG PRESET", hint = "BASIC: nothing extra. VERBOSE: detailed log. GRAPHICS: + safe renderer. "
       .. "PERFORMANCE: stack profile (next start on). CRASH (or freeze): log and profile. FULL: all. CUSTOM: yours.",
     setting = "dbg_preset", choices = {"CUSTOM", "BASIC", "VERBOSE", "GRAPHICS", "PERFORMANCE", "CRASH", "FULL"}},
    {section = "REPORTING A BUG"},
    {label = "MARK THE LOG", hint = "Press A when you see a bug: a marked line in the log and a screenshot show us "
       .. "the moment.", action = "mark"},
    {label = "TAKE SCREENSHOT", hint = "Press A to save the screen in sdmc:/switch/bo2zm/captures.",
     action = "screenshot"},
    {label = "MARK LOCATION", hint = "ON (with DEBUG on) puts MARK LOCATION at the top of the pause menu in a match: "
       .. "it saves where you stand and look, the map and round, and four screenshots, so we can go back to that "
       .. "exact spot.", setting = "mark_location", choices = OnOff},
    {label = "BUILD AND LOG FILE", hint = "", infoHint = true, action = "info"},
    {label = "MAX LOG FILES", hint = "How many logs the game keeps, one per start: each start deletes the older ones. "
       .. "OFF keeps every log. From the next start, whatever DEBUG is.", setting = "max_logs",
     choices = {"5", "10", "20", "OFF"}},
    {label = "PURGE DEBUG INFO", hint = "Deletes debug files (logs, screenshots, traces) from the SD card: pick what, "
       .. "see how much, then confirm. Your settings, saves and the game stay.", page = "purge"},
    {label = "LOG DETAIL", hint = "DETAILED adds the game's debug lines to the log (a bigger file).",
     setting = "dbg_log", choices = {"NORMAL", "DETAILED"}},
    {label = "STACK PROFILE", hint = "From the next start, logs/rex/rex_profile.log records where each thread spends "
       .. "its time. It costs frame rate: turn it off afterwards.", setting = "dbg_profile", choices = OnOff},
    {section = "GRAPHICS BUGS"},
    {label = "TEXTURE CHECKS", hint = "FULL rechecks every texture's bytes instead of skipping unchanged ones. Try it "
       .. "for flickering or out of date textures.", setting = "dbg_textures", choices = {"FAST", "FULL"}},
    {label = "SHADER CONSTANTS", hint = "FULL copies every shader constant, not only the ones a shader reads. Try it "
       .. "for wrong colors or lighting.", setting = "dbg_constants", choices = {"FAST", "FULL"}},
    {label = "GEOMETRY CACHES", hint = "OFF uploads models' vertices and indices again every time. Try it for broken "
       .. "or stretched models.", setting = "dbg_caches", choices = {"ON", "OFF"}},
    {label = "SAFE RENDERER", hint = "ON turns the three shortcuts above off at once (slower). If a graphics bug goes "
       .. "away, tell us.", setting = "dbg_safe", choices = OnOff},
    {label = "RESET DEBUG", hint = "Every debug option back to normal (preset BASIC). CUSTOM keeps your own picks.",
     action = "debug_reset"},
  },
  -- PURGE DEBUG INFO (bo2zm_debug_options.cpp, which lists the files of each tier). The DELETE button's label is the
  -- program's (labelDvar): how many files and MB the tier holds, then what it deleted; B backs out.
  purge = {
    title = "PURGE DEBUG INFO",
    parent = "debug",
    opened = "bo2zm_purge_scan",
    {label = "WHAT TO DELETE", hint = "LOGS: old logs (not this run's), crash and profiler logs, marks. SCREENSHOTS: "
       .. "captures. TRACES AND TEST FILES: D3D traces, PGO training files, launch captures. ALL: the three. "
       .. "Settings, saves, caches and the game always stay.", setting = "purge_tier",
     choices = {"LOGS", "SCREENSHOTS", "TRACES AND TEST FILES", "ALL"}},
    {label = "COUNTING...", labelDvar = "bo2zm_purge_ui", hint = "A deletes the files counted on this line (it cannot "
       .. "be undone). B goes back without deleting.", action = "purge"},
  },
  -- A power-up shows when this map includes it (bo2zm_map_powerups_ui). Bonfire Sale is left out because no map
  -- includes it.
  powerups = {
    title = "POWER UPS",
    {label = "NUKE", hint = "Press A to spawn.", action = "powerup", arg = "nuke"},
    {label = "MAX AMMO", hint = "Press A to spawn.", action = "powerup", arg = "full_ammo"},
    {label = "INSTA-KILL", hint = "Press A to spawn.", action = "powerup", arg = "insta_kill"},
    {label = "DOUBLE POINTS", hint = "Press A to spawn.", action = "powerup", arg = "double_points"},
    {label = "CARPENTER", hint = "Press A to spawn.", action = "powerup", arg = "carpenter"},
    {label = "FIRE SALE", hint = "Press A to spawn.", action = "powerup", arg = "fire_sale"},
    {label = "DEATH MACHINE", hint = "Press A to spawn.", action = "powerup", arg = "minigun"},
    {label = "FREE PERK", hint = "Press A to spawn.", action = "powerup", arg = "free_perk"},
    {label = "ZOMBIE BLOOD", hint = "Press A to spawn.", action = "powerup", arg = "zombie_blood"},
    {label = "BONUS POINTS", hint = "Press A to spawn.", action = "powerup", arg = "bonus_points_player"},
    {label = "RANDOM WEAPON", hint = "Press A to spawn.", action = "powerup", arg = "random_weapon"},
  },
}

local ExtrasHint = "Your username, the loading screen video, cheats and debug options."

-- The pages, in a list: the game's HavokScript pairs() saw no keys in ExtrasPages (observed: a pairs loop over it never
-- ran), so tables are only walked with ipairs over lists.
-- Every page of ExtrasPages must be here: only these become menus (state.pageBuilders), so a page left out never opens.
local PageNames = {"main", "scripts", "script_targets", "script_issues", "experimental", "movement", "camera", "zombies",
                   "powerups", "perks", "debug", "weapons", "doors_power", "purge"}

-- Every setting and button of these pages is a cheat, except preferences (pref = true).
local CheatPages = {movement = true, camera = true, zombies = true, powerups = true, perks = true,
                    doors_power = true}

-- Whether the space-separated list in a game dvar has name (the program's lists of this map's perks and power-ups,
-- bo2zm_cheats.cpp); an empty list (not known yet) has everything.
local function ListHas(dvar, name)
  local list = UIExpression.DvarString(nil, dvar)
  if list == nil or list == "" then
    return true
  end
  return string.find(" " .. list .. " ", " " .. name .. " ", 1, true) ~= nil
end

-- What an item looks like on this map: false to leave it out, else its label and hint. Perks show when the map has
-- their machine or with CROSS-MAP PERKS (marked OTHER MAP, with what works here); power-ups when the map has them.
local function OnThisMap(item, controller)
  if state.worldItem(item) and not state.primaryController(controller) then return false end
  if item.action == "perk" then
    if ListHas("bo2zm_map_perks_ui", item.arg) then
      return item.label, item.hint
    end
    if UIExpression.DvarInt(nil, state.playerDvar("cross_perks", controller)) == 1 then
      return item.label .. " (OTHER MAP)", "Press A to get it. " .. item.other
    end
    return false
  end
  if item.action == "powerup" and not ListHas("bo2zm_map_powerups_ui", item.arg) then
    return false
  end
  -- A map's own note on an item (mapHints, by ui_mapname).
  local mapHint = item.mapHints and item.mapHints[UIExpression.DvarString(nil, "ui_mapname")]
  if mapHint then
    return item.label, mapHint
  end
  return item.label or item.section, item.hint
end
for _, page in ipairs(PageNames) do
  if CheatPages[page] then
    for _, item in ipairs(ExtrasPages[page]) do
      if not item.section then item.player = true end
      if not (item.section or item.page or item.pref) then
        item.cheat = true
      end
    end
  end
end

-- Page closures keep their controller; shared graphics/debug settings never enter this transport.
(function()
  for _, item in ipairs(ExtrasPages.main) do
    if item.cheat or item.setting == "cheats" or item.action == "reset_cheats" then item.player = true end
    if item.page == "doors_power" then item.world = true end
  end
  function state.validController(controller)
    return type(controller) == "number" and controller >= 0 and controller <= 3 and controller % 1 == 0
  end
  function state.playerItem(item) return item.player == true or item.cheat == true or item.world == true end
  function state.worldItem(item)
    return item.world == true or item.dvar ~= nil or item.setting == "round" or item.setting == "zm_nospawn"
      or item.action == "kill_zombies" or item.action == "doors" or item.action == "power" or item.action == "debris"
  end
  function state.playerDvar(option, controller)
    if not state.validController(controller) then return "bo2zm_player_unassigned_ui" end
    return "bo2zm_p" .. controller .. "_" .. option .. "_ui"
  end
  function state.pageController(menu, expected, event)
    local controller = expected
    if type(menu.getOwner) == "function" then
      local ok, owner = pcall(menu.getOwner, menu)
      if not ok then return end
      controller = owner
    end
    if not state.validController(controller) or (expected ~= nil and controller ~= expected)
        or (event and event.controller ~= nil and event.controller ~= controller) then return end
    return controller
  end
  function state.execPlayer(controller, command, world)
    if not state.zombies() or not state.validController(controller)
        or (world and not state.primaryController(controller)) then return false end
    Engine.ExecNow(controller, "bo2zm_player " .. controller .. " " .. command)
    return true
  end
end)()

-- Every cheat back to normal play: the game's dvars back to the game ("bo2zm_reset_cheats" also releases them in the
-- program, and turns god mode and the fly modes off). The FPS counter, the preset and the combo's settings stay.
local function ResetCheats(controller)
  for _, page in ipairs(PageNames) do
    for _, item in ipairs(ExtrasPages[page]) do
      if item.dvar and state.primaryController(controller) then
        state.execPlayer(controller, "bo2zm_release " .. item.dvar .. " " .. tostring(item.values[item.normal or 1]), true)
      end
    end
  end
  state.execPlayer(controller, "bo2zm_reset_cheats")
  if state.applyHud then
    Safely(state.applyHud, controller, false)
  end
end

local Actions = {
  scripts_load = function(controller) Engine.ExecNow(controller, "bo2zm_scripts_load") end,
  scripts_unload = function(controller) Engine.ExecNow(controller, "bo2zm_scripts_unload") end,
  scripts_refresh = function(controller) Engine.ExecNow(controller, "bo2zm_scripts_refresh") end,
  reset_cheats = ResetCheats,
  max_points = function(controller)
    state.execPlayer(controller, "bo2zm_max_points")
  end,
  kill_zombies = function(controller)
    state.execPlayer(controller, "bo2zm_kill_zombies", true)
  end,
  powerup = function(controller, item)
    state.execPlayer(controller, "bo2zm_powerup " .. item.arg)
  end,
  perk = function(controller, item)
    state.execPlayer(controller, "bo2zm_perk " .. item.arg)
  end,
  remove_perks = function(controller)
    state.execPlayer(controller, "bo2zm_remove_perks")
  end,
  give_weapon = function(controller, item)
    state.execPlayer(controller, "bo2zm_give_weapon " .. item.arg)
  end,
  pap_weapon = function(controller)
    state.execPlayer(controller, "bo2zm_pap_weapon")
  end,
  doors = function(controller, item)
    state.execPlayer(controller, "bo2zm_doors " .. item.arg, true)
  end,
  debris = function(controller, item)
    state.execPlayer(controller, "bo2zm_debris " .. item.arg, true)
  end,
  power = function(controller, item)
    state.execPlayer(controller, "bo2zm_power " .. item.arg, true)
  end,
  edit_name = function(controller)
    Engine.ExecNow(controller, "bo2zm_edit_name")
  end,
  mark = function(controller)
    Engine.ExecNow(controller, "bo2zm_mark")
  end,
  screenshot = function(controller)
    Engine.ExecNow(controller, "bo2zm_screenshot")
  end,
  debug_reset = function(controller)
    Engine.ExecNow(controller, "bo2zm_debug_reset")
  end,
  purge = function(controller)
    Engine.ExecNow(controller, "bo2zm_purge")
  end,
}

-- The build and this run's log file (the program sets both dvars at start).
local function InfoText()
  return "Build " .. UIExpression.DvarString(nil, "bo2zm_build_ui") .. ". This run's log: "
    .. "sdmc:/switch/bo2zm/logs/" .. UIExpression.DvarString(nil, "bo2zm_log_ui") .. "."
end

-- Options > Graphics: the Anaglyph 3D selector becomes this.
local PresetItem = {
  label = "GRAPHICS PRESET",
  hint = "Lower presets trade detail for frame rate. Changing any setting below makes it CUSTOM; picking a preset "
    .. "sets them all.",
  setting = "preset", choices = {"LOWEST", "VERY LOW", "LOW", "MEDIUM", "HIGH", "CUSTOM"},
}

-- Options > Graphics: port-wide runtime controls, then each option of the presets (bo2zm_preset.cpp), in its order.
local GraphicsItems = {
  {label = "SPLIT-SCREEN QUALITY", hint = "The highest graphics level used with more than one local player. SAME "
     .. "uses the preset above; lower choices reduce detail for every split-screen view.",
   setting = "split_quality", choices = {"SAME", "LOWEST", "VERY LOW", "LOW", "MEDIUM", "HIGH"}},
  {label = "TEXTURE STREAMING", hint = "Maximum in-match .ipak read rate. 4 MB/s matches the Switch limit; "
     .. "UNLIMITED lets the game stream at full speed.",
   setting = "ipak_rate", choices = {"4 MB/S", "16 MB/S", "UNLIMITED"}},
  {label = "SPLIT-SCREEN MODEL SCALE", hint = "How much sooner distant models simplify with 3 or 4 players. 1 keeps "
     .. "the same screen-size detail as one player.",
   setting = "split_model", choices = {"1X", "1.5X", "2X", "3X", "4X"}},
  {label = "TEXTURE MEMORY", hint = "The texture cache's soft and hard limits. Larger caches keep more textures "
     .. "ready instead of loading them again. Changes apply right away.",
   setting = "texture_memory", choices = {"384 / 768 MB", "768 / 1536 MB", "1536 / 3072 MB", "3072 / 6144 MB"}},
  {label = "TEXTURE DETAIL", hint = "How sharp textures are. Lower uses smaller copies of them.",
   setting = "gfx_texture", choices = {"LOWEST", "LOW", "MEDIUM", "HIGH"}},
  {label = "MODEL DETAIL", hint = "How soon models switch to simpler versions with distance.",
   setting = "gfx_model", choices = {"LOW", "MEDIUM", "HIGH", "ULTRA"}},
  {label = "DRAW DISTANCE", hint = "How far the world is drawn. Town's fog hides most of what lies past MEDIUM; "
     .. "Buried's caves have no fog: with FAR OBJECTS on SIMPLE the sky stays hidden at any distance (or FAR or "
     .. "FULL; a map profile can keep it there).",
   setting = "gfx_distance", choices = {"SHORTEST", "SHORT", "MEDIUM", "FAR", "FULL"}},
  {label = "FAR OBJECTS", hint = "What lies past the draw distance. HIDE: not drawn, so the sky can show through "
     .. "cave walls and far buildings. SIMPLE: big shapes stay, in their simplest form, for a little frame rate.",
   setting = "gfx_far", choices = {"HIDE", "SIMPLE"}},
  {label = "SHADOWS", hint = "Sun and spotlight shadows.",
   setting = "gfx_shadows", choices = {"OFF", "LOW", "MEDIUM", "HIGH"}},
  {label = "DYNAMIC LIGHTS", hint = "Lights from muzzle flashes, explosions and effects.",
   setting = "gfx_lights", choices = {"OFF", "LOW", "MEDIUM", "HIGH"}},
  {label = "SCREEN EFFECTS", hint = "Depth of field, flames, water on the screen and clouds.",
   setting = "gfx_effects", choices = OnOff},
  {label = "DECALS", hint = "Blood, scorch and bullet marks, and how far they show.",
   setting = "gfx_decals", choices = {"OFF", "NEAR", "MEDIUM", "FAR"}},
  {label = "CORPSES AND DEBRIS", hint = "How many bodies and pieces of debris stay around.",
   setting = "gfx_bodies", choices = {"FEWEST", "FEW", "SOME", "ALL"}},
}

-- Options > Graphics, at the top: map profiles (bo2zm_preset.cpp). A map with its profile on loads settings of its
-- own; the rest use GLOBAL. In the menus MAP PROFILE picks which one the settings below show and change (GLOBAL, or a
-- map: its own when its profile is on, else GLOBAL, which it uses); in a match PROFILE IN USE shows the one the map is
-- using, and ENABLE PROFILE switches it there at once. The names are the program's kLocations, in its order.
local ProfileNames = {"GLOBAL", "TOWN", "FARM", "BUS DEPOT", "TRANZIT", "DINER", "NUKETOWN", "DIE RISE",
                      "MOB OF THE DEAD", "BURIED", "ORIGINS"}
local MapProfileItem = {
  label = "MAP PROFILE",
  hint = "The profile the settings below show and change. GLOBAL: every map without a profile of its own.",
  setting = "profile", choices = ProfileNames,
}
-- In a match only the program picks the profile: a status row (the rule above the Extras pages).
local ProfileInUseItem = {
  label = "PROFILE IN USE",
  setting = "profile", choices = ProfileNames, status = true,
}
-- N/A (GLOBAL picked, or a map with no profile) is a status: left and right never reach it, and while it shows the
-- row is a status row.
local EnableProfileItem = {
  label = "ENABLE PROFILE",
  hint = "ON: the map above loads its own settings, the ones below while it is picked (the first time, a copy of "
    .. "GLOBAL). OFF: it uses GLOBAL.",
  setting = "profile_on", choices = {"OFF", "ON", "N/A"}, statusChoice = 3,
}
local EnableProfileGameItem = {
  label = "ENABLE PROFILE",
  hint = "PROFILE IN USE: the settings this map uses. ON: this map gets its own settings from now on, saved for it "
    .. "(the first time, a copy of GLOBAL). OFF: it goes back to GLOBAL.",
  setting = "profile_on", choices = {"OFF", "ON", "N/A"}, statusChoice = 3,
}

-- Options > Graphics: a frame rate cap, added to the same list.
local FpsCapItem = {
  label = "FPS CAP",
  hint = "The highest frame rate in a match (menus keep the game's own). 60 is the game's own.",
  setting = "fps_cap", choices = {"30", "60"},
}

-- Options > Graphics: vertical sync (bo2zm_extras.cpp).
local VsyncItem = {
  label = "VSYNC",
  hint = "Each frame waits for the screen to refresh. With FPS CAP 30 the screen paces a steady 30: the smoothest "
    .. "choice when the game cannot hold 60.",
  setting = "vsync", choices = OnOff,
}

-- Options > Graphics (Mac/PC): full screen or a window (bo2zm_extras.cpp).
local WindowItem = {
  label = "DISPLAY",
  hint = "FULL SCREEN fills the screen. WINDOW plays in a window you can move and resize. Applies at once.",
  setting = "window_mode", choices = {"FULL SCREEN", "WINDOW"},
}

-- Options > Graphics: whether the preset stays in the top-right corner, added after the cap.
local ShowPresetItem = {
  label = "SHOW PRESET",
  hint = "Shows the graphics preset in the top-right corner, and on the Switch the CPU cores in use (3 or 4). "
    .. "Disabled, the preset only shows briefly when the button combination changes it.",
  setting = "show_preset", choices = {"DISABLED", "ENABLED"},
}

-- Options > Graphics: the frame counter (it was in Extras), added after the preset's.
local FpsCounterItem = {
  label = "FPS COUNTER",
  hint = "Shows the frames per second in the top-left corner.",
  setting = "fps", choices = OnOff,
}

---------------------------------------------------------------------------------------------------------------------
-- Helpers
---------------------------------------------------------------------------------------------------------------------
-- f, safe to hand to the menu system (event handlers, timers, choice functions): an error is logged instead.
local function Protect(f)
  return function(...)
    return Safely(f, ...)
  end
end

-- Replaces a menu's handler for an event, calling the game's handler first when keep is true.
local function OnEvent(menu, eventName, handler, keep)
  local original = menu.m_eventHandlers and menu.m_eventHandlers[eventName]
  menu:registerEventHandler(eventName, function(self, event)
    local result
    if keep and original then
      result = original(self, event)
    end
    Safely(handler, self, event)
    return result
  end)
end

-- A popup with just a message, using the game's own error popup.
local function MessagePopup(menu, controller, text)
  local popup = menu:openPopup("Error", controller)
  if popup then
    popup.anyControllerAllowed = true
    popup:setMessage(text)
  end
end

-- Why an item cannot be used now (CHEATS is off), or nil.
local function BlockedText(item, controller)
  if (item.zm or state.playerItem(item)) and not state.zombies() then return "Cheats are available in Zombies." end
  if state.worldItem(item) and not state.primaryController(controller) then
    return "Only the local lobby leader can change this."
  end
  if item.cheat and UIExpression.DvarInt(nil, state.playerDvar("cheats", controller)) ~= 1 then
    return "Turn CHEATS on first (the first option under CHEATS in Extras)."
  end
end

-- Gives focus to a button again, so its highlight is measured for its current label.
local function Refocus(button, controller)
  if button and button:isInFocus() then
    button:processEvent({name = "lose_focus", controller = controller})
    button:processEvent({name = "gain_focus", controller = controller})
  end
end

-- Closes a menu's button by field name (the menu keeps it as menu[name]).
local function CloseField(menu, name)
  if menu[name] then
    menu[name]:close()
    menu[name] = nil
  end
end

-- Whether menus are being built inside a match rather than in the front end.
local function InGame()
  return UIExpression.IsInGame() == 1
end

---------------------------------------------------------------------------------------------------------------------
-- Selectors (the game's DvarLeftRightSelector). A choice's function gets userRequested = true only when the player
-- changes it (LeftRightSelector.Cycle from HandleGamepadButton); setting up and refreshing call it without, so
-- nothing is sent then. While the menu is open, refreshChoice re-reads every selector's dvar four times a second (it
-- only redraws when the value changed), so a change from elsewhere shows up: the preset's button combination, the fly
-- combo, noclip turning UFO off.
---------------------------------------------------------------------------------------------------------------------
local function Track(menu, selector)
  local tracked = menu.bo2zmTracked
  if not tracked then
    tracked = {}
    menu.bo2zmTracked = tracked
    menu:addElement(LUI.UITimer.new(250, "bo2zm_refresh_selectors", false))
    menu:registerEventHandler("bo2zm_refresh_selectors", Protect(function(self, event)
      for _, s in ipairs(tracked) do
        if s.bo2zmPicked then
          s.bo2zmPicked = nil  -- the program shows the pick from its next frame on
        else
          s:refreshChoice()
          if s.bo2zmRefreshed then
            s.bo2zmRefreshed()
          end
        end
      end
    end))
  end
  table.insert(tracked, selector)
end

-- Status rows (the rule above the Extras pages). Only ApplyStatus is used further down (the top level's locals are
-- limited: see the top of the file).
local ApplyStatus
do
  -- A selector's arrows (where the game draws them): none on a status row; on a normal row LeftRightSelector.new's
  -- looks, which EnableCycling does not restore (its focused look keeps the arrow's last color, so a row that had
  -- been a status row showed a grey left arrow). DisableCycling hides them, but its "disabled" look, the one
  -- UIButton.disable shows, draws them grey.
  local function SetArrows(selector, shown)
    for _, arrow in ipairs({selector.leftArrow or false, selector.rightArrow or false}) do
      if arrow then
        if shown then
          local text, over = CoD.ButtonTextColor, CoD.BOIIOrange
          arrow:registerAnimationState("default", {red = text.r, green = text.g, blue = text.b, alpha = 1})
          arrow:registerAnimationState("button_over", {red = over.r, green = over.g, blue = over.b, alpha = 1})
          arrow:animateToState(selector:isInFocus() and "button_over" or "default")
        else
          arrow:registerAnimationState("disabled", {alpha = 0})
          arrow:animateToState("disabled")
        end
      end
    end
  end

  -- A status row (the rule above the Extras pages), or a normal row again with on = false. The game's own parts:
  -- LeftRightSelector.DisableCycling (its HandleGamepadButton then ignores left and right: no change, no sound),
  -- UIButton.disable (the disabled color), and the list's navigation, which links only focusable rows
  -- (LUI.UIVerticalList.UpdateNavigation; the headings are skipped the same way). A row with the cursor on it becomes
  -- unreachable only once the cursor leaves (the next call).
  local function StatusRow(selector, on)
    if (selector.bo2zmStatus == true) ~= on then
      selector.bo2zmStatus = on
      if on then
        selector:disableCycling()
        selector:disable()
      else
        selector:enableCycling()
        selector:enable()
      end
      SetArrows(selector, not on)
    end
    if (selector:isFocusable() == true) == on and not (on and selector:isInFocus()) then
      if on then
        selector:makeNotFocusable()
      else
        selector:makeFocusable()
      end
      local list = selector:getParent()
      if list then
        LUI.UIVerticalList.UpdateNavigation(list)
      end
    end
  end

  -- Left and right never stop on choice number skip (statusChoice): the game's LeftRightSelector.Cycle
  -- (ui/t6/leftrightselector.lua), which steps to the next choice either way round, with that choice left out.
  local function SkipChoice(selector, skip)
    selector.cycle = function(self, forward, userRequested)
      local count = self.m_choices and #self.m_choices or 0
      local index = self.m_currentChoice or 0
      for _ = 1, count do
        if forward then
          index = index % count + 1
        else
          index = (index - 2) % count + 1
        end
        if index ~= skip then
          break
        end
      end
      if count == 0 or index == skip or index == self.m_currentChoice then
        return
      end
      if self.m_sfx then
        Engine.PlaySound(self.m_sfx)
      end
      CoD.LeftRightSelector.SetCurrentChoice(self, index)
      self:updateChoice(userRequested)
    end
  end

  -- A setting row's status rule (status, statusChoice), once its choices are in.
  function ApplyStatus(selector, item)
    if item.status ~= nil then
      StatusRow(selector, item.status)
    elseif item.statusChoice then
      SkipChoice(selector, item.statusChoice)
      selector.bo2zmRefreshed = Protect(function()
        StatusRow(selector, selector.m_currentChoice == item.statusChoice)
      end)
      selector.bo2zmRefreshed()
    end
  end
end

-- The dvar a selector shows for an item, and the console text a pick of value runs.
local function ItemDvar(item, controller)
  if state.playerItem(item) and not state.worldItem(item) then return state.playerDvar(item.setting, controller) end
  if item.setting then
    return "bo2zm_" .. item.setting .. "_ui"
  end
  return "bo2zm_hold_" .. item.dvar
end

local function PickCommand(item, index, value)
  if item.setting then
    return "bo2zm_" .. item.setting .. " " .. tostring(item.values and value or (index - 1))
  end
  if index == (item.normal or 1) then
    return "bo2zm_release " .. item.dvar .. " " .. tostring(value)
  end
  return "bo2zm_hold " .. item.dvar .. " " .. tostring(value)
end

-- The value of each choice: values[i] for game dvars, the index for the port's settings.
local function ChoiceValue(item, index)
  return item.values and item.values[index] or (index - 1)
end

-- A section heading in a button list.
local function AddHeading(list, text)
  list:addSpacer(CoD.CoD9Button.Height / 2)
  local heading = list:addText(text)
  heading:setTopBottom(true, false, 0, CoD.CoD9Button.TextHeight)  -- the option labels' size
  heading:setFont(CoD.fonts.Condensed)
  heading:setRGB(CoD.trueOrange.r, CoD.trueOrange.g, CoD.trueOrange.b)
  return heading
end

-- Long lists (Options > Graphics, the long Extras pages). The game gives a ButtonList a box 720 tall from the title,
-- past the screen's bottom, and keeps the hint as the list's last child: a long list ran into the Select / Back
-- prompts and pushed the hint off the screen (no hint on Options > Graphics was ever visible). FitList ends
-- the list's box above a hint of HintLines lines and the prompts (the menu's prompt bars are CoD.ButtonPrompt.Height
-- tall at its bottom, codmenu.lua), moves the hint under that box, and clips the list with a stencil as the game's
-- ScrollingVerticalList does: the list then scrolls by itself to keep the focused row in view.
-- Only FitList and TooTall are used further down (the top level's locals are limited: see the top of the file).
local FitList, TooTall
do
  local HintLines = 4

  -- A hint's lines are CoD.HintText.Height + 4 apart (25 at a height of 21); DRAW DISTANCE's hint has 4 lines.
  local function HintRoom()
    return HintLines * (CoD.HintText.Height + 4) + 10
  end

  function FitList(menu, list, left, right)
    local bottom = CoD.Menu.Height - CoD.ButtonPrompt.Height - HintRoom()
    list:setTopBottom(true, false, CoD.Menu.TitleHeight, bottom)
    list:setUseStencil(true)
    local hint = list.hintText
    if hint and hint:getParent() == list then
      list:removeElement(hint)
      hint:setLeftRight(true, false, left, right)
      hint:setTopBottom(true, false, bottom + 10, bottom + 10 + CoD.HintText.Height)
      menu:addElement(hint)
    end
  end

  -- Whether rows and headings (as the ButtonList and AddHeading make them) leave no room for the hint above the
  -- prompts.
  function TooTall(rows, headings)
    local spacing = CoD.ButtonList.ButtonSpacing or 0
    local height = rows * (CoD.CoD9Button.Height + spacing)
      + headings * (CoD.CoD9Button.Height / 2 + CoD.CoD9Button.TextHeight + 2 * spacing)
    return CoD.Menu.TitleHeight + height + HintRoom() > CoD.Menu.Height - CoD.ButtonPrompt.Height
  end
end

-- A choice's function returning false makes the game's LeftRightSelector.UpdateChoice put the previous choice back at
-- once (it never shows the refused one).
local function AddChoices(menu, selector, controller, item)
  for i, label in ipairs(item.choices) do
    local value = ChoiceValue(item, i)
    selector:addChoice(controller, label, value, nil, Protect(function(params, userRequested)
      if userRequested then
        if item.status or i == item.statusChoice then
          return false
        end
        if state.playerItem(item) and not state.pageController(menu, controller, params) then return false end
        local blocked = BlockedText(item, controller)
        if blocked then
          MessagePopup(menu, controller, blocked)
          return false
        end
        selector.bo2zmPicked = true
        local command = PickCommand(item, i, value)
        if state.playerItem(item) then
          if not state.execPlayer(controller, command, state.worldItem(item)) then return false end
        else
          Engine.ExecNow(controller, command)
        end
        if item.setting == "hud" and state.applyHud then
          state.applyHud(controller, i == 2 and UIExpression.DvarInt(nil, state.playerDvar("cheats", controller)) == 1)
        elseif item.setting == "cheats" and state.applyHud then
          state.applyHud(controller, i == 2 and UIExpression.DvarInt(nil, state.playerDvar("hud", controller)) == 1)
        end
      end
    end))
  end
  Track(menu, selector)
end

-- A held dvar's menu dvar exists from the first pick on (the program creates it); until then the selector shows the
-- game's own choice.
local function DefaultToNormal(selector, item)
  local normal = tostring(ChoiceValue(item, item.normal or 1))
  local read = selector.getCurrentValue
  selector.getCurrentValue = function(self)
    local value = read(self)
    if value == nil or value == "" then
      return normal
    end
    return value
  end
  selector.currentValue = selector:getCurrentValue()
end

---------------------------------------------------------------------------------------------------------------------
-- Extras: the main page opens as a popup over the home menu; in a match (and in the campaign's front end) it takes the
-- menu's place like Options does. Each other page takes the main page's place (a popup over the home menu, or the main page's place in a match),
-- so every page has the same background, and Back returns to the main page; Back on the main page returns to the home
-- menu or the pause menu.
---------------------------------------------------------------------------------------------------------------------
local function PageMenuName(page)
  return page == "main" and "Bo2zmExtras" or ("Bo2zmExtras_" .. page)
end

local function OpenPage(menu, controller, page)
  local name = PageMenuName(page)
  if InGame() or not state.zombies() then
    menu:saveState()
    menu:openMenu(name, controller)
    menu:close()
  elseif page == "main" then
    menu:openPopup(name, controller)
  else
    local home = menu.occludedMenu  -- the home menu, under the main page
    menu:close()
    home:openPopup(name, controller)
  end
end

local function CreatePage(controller, page)
  local items = ExtrasPages[page]
  local name = PageMenuName(page)
  local menu
  if InGame() then
    menu = CoD.InGameMenu.New(name, controller, items.title, CoD.isSinglePlayer)
    menu:setPreviousMenu(page == "main" and (state.campaign() and "ObjectiveInfoMenu" or "class")
                         or PageMenuName(items.parent or "main"))
  elseif not state.zombies() then
    -- Use each mode's normal menu frame rather than a popup material supplied by Zombies zones.
    menu = CoD.Menu.New(name)
    menu:setOwner(controller)
    menu:addTitle(items.title)
    menu:setPreviousMenu(page == "main" and (state.campaign() and "CampaignMenu" or "MainMenu")
                         or PageMenuName(items.parent or "main"))
  else
    menu = CoD.Menu.New(name)
    menu:setOwner(controller)
    menu:addTitle(items.title)
    menu:addLargePopupBackground()
    if page ~= "main" then
      menu:registerEventHandler("button_prompt_back", Protect(function(self, event)
        local home = self.occludedMenu
        self:close()
        home:openPopup(PageMenuName(items.parent or "main"), event.controller or controller)
      end))
    end
  end
  -- On the home screen no controller is assigned to a player yet, and CoD.Menu.GamepadButton renames such a
  -- controller's presses to "unused_gamepad_button" for menus without anyControllerAllowed: the options would never
  -- see Up, Down, Left or Right. The home menu and its popups allow any controller the same way.
  menu.anyControllerAllowed = not InGame()
  menu:addSelectButton()
  menu:addBackButton()

  local list = CoD.ButtonList.new()
  -- The width of the game's option pages (CoD.Options, loaded with them; the plain list width before that).
  list:setLeftRight(true, false, 0, CoD.Options and CoD.Options.Width or CoD.ButtonList.DefaultWidth)
  list:setTopBottom(true, false, CoD.Menu.TitleHeight, CoD.Menu.TitleHeight + 720)
  menu:addElement(list)
  menu.buttonList = list

  local first
  local inGame = InGame()
  local rows, headings = 0, 0
  for _, item in ipairs(items) do
    if item.modes then item = state.modeItem(item) end
    local element
    local label, hint = OnThisMap(item, controller)
    if item.where and (item.where == "game") ~= inGame then
      -- not on this menu
    elseif item.zm and not state.zombies() then
      -- Zombies only
    elseif item.platform and item.platform ~= UIExpression.DvarString(nil, "bo2zm_platform_ui") then
      -- Switch-only or Mac/PC-only
    elseif not label then
      -- not on this map
    elseif item.section then
      AddHeading(list, item.section)
      headings = headings + 1
    elseif item.textDvar then
      element = list:addDvarLeftRightSelector(controller, item.label, "bo2zm_scripts_text_ui", item.hint)
      AddChoices(menu, element, controller, {setting = "scripts_text", choices = {""}, status = true})
      element.refreshChoice = function(self)
        local text = UIExpression.DvarString(nil, item.textDvar) or ""
        if text ~= self.bo2zmText then
          self.bo2zmText = text
          self:clearChoices()
          self:addChoice(controller, text, 0, nil, function() end)
          CoD.LeftRightSelector.SetCurrentChoice(self, 1)
          self:updateChoice(false)
        end
      end
      element:refreshChoice()
      ApplyStatus(element, {status = true})
    elseif item.page or item.action then
      if item.nameHint then
        hint = "Now: " .. UIExpression.DvarString(nil, "bo2zm_name_ui") .. ". " .. hint
      elseif item.infoHint then
        hint = InfoText()
      end
      element = list:addButton(label, hint)
      element:setActionEventName("bo2zm_page_button")
      element.bo2zmItem = item
      if item.labelDvar then
        -- The program's label, read again four times a second like a selector's dvar (Track).
        element.refreshChoice = function(self)
          local text = UIExpression.DvarString(nil, item.labelDvar)
          if text ~= nil and text ~= "" and text ~= self.bo2zmText then
            self.bo2zmText = text
            self:setLabel(text)
            Refocus(self, controller)
          end
        end
        element:refreshChoice()
        Track(menu, element)
      end
    else
      element = list:addDvarLeftRightSelector(controller, item.label, ItemDvar(item, controller), item.hint)
      if item.dvar then
        DefaultToNormal(element, item)
      end
      AddChoices(menu, element, controller, item)
      ApplyStatus(element, item)
      if item.apply then
        -- A applies the pick (left and right only choose).
        element:registerEventHandler("button_action", Protect(function(self, event)
          if not state.pageController(menu, controller, event) then return end
          local blocked = BlockedText(item, controller)
          if blocked then
            MessagePopup(menu, controller, blocked)
          else
            state.execPlayer(controller, item.apply, state.worldItem(item))
          end
        end))
      end
    end
    if element then
      rows = rows + 1
      if element:isFocusable() then
        first = first or element
      end
    end
  end
  if TooTall(rows, headings) then
    FitList(menu, list, 0, CoD.Options and CoD.Options.Width or CoD.ButtonList.DefaultWidth)
  end
  menu:registerEventHandler("bo2zm_page_button", Protect(function(self, event)
    if not state.pageController(self, controller, event) then return end
    local item = event.button.bo2zmItem
    local blocked = BlockedText(item, controller)
    if blocked then
      MessagePopup(self, event.controller or controller, blocked)
    elseif item.page then
      OpenPage(self, event.controller or controller, item.page)
    elseif item.action == "info" then
      MessagePopup(self, event.controller or controller, InfoText())
    elseif item.action == "message" then
      MessagePopup(self, event.controller or controller, item.message)
    else
      Actions[item.action](event.controller or controller, item)
      if item.refreshPage then OpenPage(self, event.controller or controller, item.refreshPage) end
    end
  end))
  if items.opened then
    Engine.ExecNow(controller, items.opened)  -- a page's own command when it opens (PURGE DEBUG INFO counts)
  end
  if first then
    first:processEvent({name = "gain_focus", controller = controller})
  end
  return menu
end

---------------------------------------------------------------------------------------------------------------------
-- WEAPONS (in a match): the guns this map has (the program's catalog, bo2zm_cheats.cpp: bo2zm_guns_ui_count and
-- bo2zm_guns_ui_<n>, ten "name,key,category,upgradable" each), in a grid of GunColumns x GunRows buttons per page,
-- by category. A gives the gun in place of the one in your hands. Above the grid: PACK-A-PUNCH (one pick for every
-- gun, so the list is not doubled: ON gives the upgraded version; Y switches it too) and PACK-A-PUNCH CURRENT
-- WEAPON. LB and RB change the page. The grid's buttons are linked left and right by the game's own navigation
-- (LUI.UIElement.GamepadButton follows element.navigation[button]; each column's ButtonList links up and down).
---------------------------------------------------------------------------------------------------------------------
-- Only CreateWeaponsPage is used further down (the top level's locals are limited: see the top of the file).
local CreateWeaponsPage
do
  local GunColumns, GunRows = 3, 8
  local GunsPerPage = GunColumns * GunRows
  local GunColumnWidth = 300
  local GunCategoryHints = {pistol = "Pistol", smg = "Submachine gun", rifle = "Assault rifle", shotgun = "Shotgun",
                            lmg = "Light machine gun", sniper = "Sniper rifle", launcher = "Launcher",
                            special = "Wonder weapon"}
  local PapItem = {label = "PACK-A-PUNCH",
                   hint = "ON: the guns below come Pack-a-Punched (when the gun has an upgrade).",
                   setting = "give_pap", choices = {"OFF", "ON"}, pref = true, player = true}
  local PapCurrentItem = {label = "PACK-A-PUNCH CURRENT WEAPON", hint = "Upgrades the gun in your hands.",
                          action = "pap_weapon", cheat = true}

  local function MapGuns()
    local guns = {}
    local count = UIExpression.DvarInt(nil, "bo2zm_guns_ui_count") or 0
    local part = 0
    while #guns < count and part < 40 do
      local text = UIExpression.DvarString(nil, "bo2zm_guns_ui_" .. part) or ""
      for name, key, category, upgradable in string.gmatch(text, "([%w_]+),([%w_]+),(%a+),(%d)") do
        table.insert(guns, {name = name, key = key, category = category, upgradable = upgradable == "1"})
      end
      part = part + 1
    end
    return guns
  end

  -- The game's name for a gun (its display name's localization), else one made from its internal name. The two
  -- ballistic knives share one display name: the Bowie one is marked.
  local function GunLabel(gun)
    local ok, text = pcall(Engine.Localize, gun.key)
    if ok and type(text) == "string" and text ~= "" and text ~= gun.key and not string.find(text, "_", 1, true) then
      if string.find(gun.name, "_bowie", 1, true) then
        text = text .. " (BOWIE)"
      end
      return string.upper(text)
    end
    local plain = string.gsub(gun.name, "_zm$", "")
    return string.upper(plain)
  end

  function CreateWeaponsPage(controller)
    local name = PageMenuName("weapons")
    local menu = CoD.InGameMenu.New(name, controller, ExtrasPages.weapons.title, CoD.isSinglePlayer)
    menu:setPreviousMenu(PageMenuName("main"))
    menu.anyControllerAllowed = not InGame()
    menu:addSelectButton()
    menu:addBackButton()

    local guns = MapGuns()
    local pages = math.max(1, math.ceil(#guns / GunsPerPage))
    local rowHeight = CoD.CoD9Button.Height + (CoD.ButtonList.ButtonSpacing or 0)
    local hintTop = CoD.Menu.TitleHeight + 3 * rowHeight + 20 + GunRows * rowHeight + 10
    local hint = CoD.HintText.new({leftAnchor = true, rightAnchor = false, left = 0,
                                   right = GunColumns * GunColumnWidth,
                                   topAnchor = true, bottomAnchor = false, top = hintTop,
                                   bottom = hintTop + CoD.HintText.Height})
    menu:addElement(hint)

    local head = CoD.ButtonList.new(nil, nil, nil, hint)
    head:setLeftRight(true, false, 0, CoD.Options and CoD.Options.Width or CoD.ButtonList.DefaultWidth)
    head:setTopBottom(true, false, CoD.Menu.TitleHeight, CoD.Menu.TitleHeight + 3 * rowHeight + 10)
    menu:addElement(head)
    local pap = head:addDvarLeftRightSelector(controller, PapItem.label, ItemDvar(PapItem, controller), PapItem.hint)
    AddChoices(menu, pap, controller, PapItem)
    local papCurrent = head:addButton(PapCurrentItem.label, PapCurrentItem.hint)
    papCurrent:setActionEventName("bo2zm_page_button")
    papCurrent.bo2zmItem = PapCurrentItem
    local pageText = head:addText("")
    pageText:setTopBottom(true, false, 0, CoD.CoD9Button.TextHeight)
    pageText:setFont(CoD.fonts.Condensed)
    pageText:setRGB(CoD.trueOrange.r, CoD.trueOrange.g, CoD.trueOrange.b)

    local page = 1
    local columns = {}
    local function ShowPage(focus)
      for _, column in ipairs(columns) do
        column:close()
      end
      columns = {}
      if #guns == 0 then
        pageText:setText("THIS MAP'S GUNS ARE NOT KNOWN YET: OPEN THIS PAGE AGAIN IN A MOMENT.")
        return
      end
      pageText:setText("PAGE " .. page .. " OF " .. pages .. (pages > 1 and "   LB / RB: OTHER PAGES" or ""))
      local grid = {}
      local first = (page - 1) * GunsPerPage
      for c = 1, GunColumns do
        local column = CoD.ButtonList.new(nil, nil, nil, hint)
        local left = (c - 1) * GunColumnWidth
        local gridTop = CoD.Menu.TitleHeight + 3 * rowHeight + 20
        column:setLeftRight(true, false, left, left + GunColumnWidth - 10)
        column:setTopBottom(true, false, gridTop, gridTop + GunRows * rowHeight)
        menu:addElement(column)
        table.insert(columns, column)
        grid[c] = {}
        for r = 1, GunRows do
          local gun = guns[first + (c - 1) * GunRows + r]
          if gun then
            local text = (GunCategoryHints[gun.category] or "Gun") .. ". A: get it in place of the gun in your hands"
              .. (gun.upgradable and " (with PACK-A-PUNCH ON: upgraded)." or " (no Pack-a-Punch upgrade on this map).")
            local button = column:addButton(GunLabel(gun), text)
            button:setActionEventName("bo2zm_page_button")
            button.bo2zmItem = {action = "give_weapon", arg = gun.name, cheat = true}
            grid[c][r] = button
          end
        end
      end
      -- Left and right go to the same row of the next column (its last gun when it is shorter); the top row's up goes
      -- to the buttons above the grid, and their down to the first column.
      for c = 1, GunColumns do
        for r, button in ipairs(grid[c]) do
          local function Neighbour(other)
            local list = grid[other]
            return list and (list[r] or list[#list])
          end
          button.navigation = button.navigation or {}
          button.navigation.left = Neighbour(c - 1)
          button.navigation.right = Neighbour(c + 1)
        end
        if grid[c][1] then
          grid[c][1].navigation.up = papCurrent
        end
      end
      papCurrent.navigation = papCurrent.navigation or {}
      papCurrent.navigation.down = grid[1][1]
      if focus and grid[1][1] then
        papCurrent:processEvent({name = "lose_focus", controller = controller})
        grid[1][1]:processEvent({name = "gain_focus", controller = controller})
      end
    end
    ShowPage(false)

    local function ChangePage(step)
      if pages > 1 then
        page = (page - 1 + step + pages) % pages + 1
        ShowPage(true)
      end
    end
    menu:addRightButtonPrompt(CoD.ButtonPrompt.new("alt2", "PACK-A-PUNCH", menu, "bo2zm_toggle_pap"))
    if pages > 1 then
      menu:addRightButtonPrompt(CoD.ButtonPrompt.new("shoulderr", "NEXT PAGE", menu, "bo2zm_gun_page_next"))
      menu:addRightButtonPrompt(CoD.ButtonPrompt.new("shoulderl", "PREVIOUS PAGE", menu, "bo2zm_gun_page_prev"))
    end
    menu:registerEventHandler("bo2zm_gun_page_next", Protect(function(self, event)
      if not state.pageController(self, controller, event) then return end
      ChangePage(1)
    end))
    menu:registerEventHandler("bo2zm_gun_page_prev", Protect(function(self, event)
      if not state.pageController(self, controller, event) then return end
      ChangePage(-1)
    end))
    menu:registerEventHandler("bo2zm_toggle_pap", Protect(function(self, event)
      if not state.pageController(self, controller, event) then return end
      local blocked = BlockedText(PapItem, controller)
      if blocked then
        MessagePopup(self, event.controller or controller, blocked)
        return
      end
      state.execPlayer(controller,
        "bo2zm_give_pap " .. (UIExpression.DvarInt(nil, ItemDvar(PapItem, controller)) == 1 and "0" or "1"))
    end))
    menu:registerEventHandler("bo2zm_page_button", Protect(function(self, event)
      if not state.pageController(self, controller, event) then return end
      local item = event.button.bo2zmItem
      local blocked = BlockedText(item, controller)
      if blocked then
        MessagePopup(self, event.controller or controller, blocked)
      else
        Actions[item.action](event.controller or controller, item)
      end
    end))
    pap:processEvent({name = "gain_focus", controller = controller})
    Log("weapons page: " .. #guns .. " guns, " .. pages .. " pages")
    return menu
  end
end

local function PageBuilder(page)
  return function(controller)
    if page == "scripts" then
      Engine.ExecNow(controller, "bo2zm_scripts_refresh")
      local choices = {"NONE"}
      local count = math.min(32, math.max(0, UIExpression.DvarInt(nil, "bo2zm_scripts_count_ui") or 0))
      for i = 0, count - 1 do
        table.insert(choices, UIExpression.DvarString(nil, "bo2zm_script_" .. i .. "_name_ui"))
      end
      ExtrasPages.scripts = {
        title = "SCRIPT LOADER", parent = "main",
        {label = "SELECT MOD", setting = "script_pick", choices = choices,
         hint = state.campaign() and "Campaign scripts are not supported yet. This page manages the next Zombies launch."
             or "Folders in scripts/ beside the executable. Flat and nested layouts work. Only mods for this gamemode appear."},
        {label = "ACTIVE MOD", textDvar = "bo2zm_scripts_active_ui"},
        {label = "STATUS", textDvar = "bo2zm_scripts_status_ui"},
        {label = "PATH SOURCE", textDvar = "bo2zm_scripts_route_ui"},
        {label = "BASE PATH", textDvar = "bo2zm_scripts_base_ui"},
        {label = "SCAN RESULT", textDvar = "bo2zm_scripts_scan_ui"},
        {label = "CHECK RESULT", textDvar = "bo2zm_scripts_preview_ui"},
        {label = "CHECK SCRIPTS", page = "script_targets",
         hint = "Validate exact targets without loading. INTERNAL uses stored names. FILES requires base: / keeps the full hierarchy; a folder prefixes every script."},
        {label = "SETUP ISSUES", page = "script_issues",
         hint = "Missing or invalid mod.ini: view each folder and reason. Valid folders for another gamemode are not setup issues."},
        {label = state.campaign() and "ENABLE FOR ZOMBIES" or "LOAD SELECTED + RESTART", action = "scripts_load",
         hint = state.campaign() and "Saves the selected mod for the next Zombies launch. Loading in Zombies ends the current match."
             or "Saves this mode's selected mod. A change restarts the game and ends the current match."},
        {label = state.campaign() and "DISABLE SCRIPTS" or "UNLOAD ALL + RESTART", action = "scripts_unload",
         hint = "Keeps the files and disables this mode's scripts. A restart clears the mod's threads, HUD and effects."},
        {label = "REFRESH LIST", action = "scripts_refresh", refreshPage = "scripts",
         hint = "Read the mod folders again."},
      }
    elseif page == "script_targets" then
      -- Checking is synchronous: read the resolved paths only after the manager has validated them.
      Engine.ExecNow(controller, "bo2zm_scripts_check")
      local items = {
        title = "SCRIPT TARGETS", parent = "scripts",
        {label = "CHECK RESULT", textDvar = "bo2zm_scripts_preview_ui"},
        {label = "CHECK SCRIPTS", page = "script_targets",
         hint = "Check again without loading. Flat and nested layouts work. Press A on a script to view its full source and target."},
      }
      local count = math.min(32, math.max(0, UIExpression.DvarInt(nil, "bo2zm_scripts_target_count_ui") or 0))
      for i = 0, count - 1 do
        local source = UIExpression.DvarString(nil, "bo2zm_scripts_source_" .. i .. "_ui") or ""
        local target = UIExpression.DvarString(nil, "bo2zm_scripts_target_" .. i .. "_ui") or ""
        table.insert(items, {label = "SCRIPT " .. (i + 1), action = "message",
          hint = source .. " => " .. target .. ". Press A for details. Checking does not load scripts.",
          message = "SOURCE\n" .. source .. "\n=> REPLACEMENT TARGET\n" .. target})
      end
      ExtrasPages.script_targets = items
    elseif page == "script_issues" then
      Engine.ExecNow(controller, "bo2zm_scripts_refresh")
      local items = {
        title = "SETUP ISSUES", parent = "scripts",
        {label = "SCAN RESULT", textDvar = "bo2zm_scripts_scan_ui"},
        {label = "REFRESH LIST", page = "script_issues",
         hint = "Scan again for missing or invalid mod.ini files. Valid folders for another gamemode are not setup issues."},
      }
      local count = math.min(32, math.max(0, UIExpression.DvarInt(nil, "bo2zm_scripts_issue_count_ui") or 0))
      for i = 0, count - 1 do
        local issue = UIExpression.DvarString(nil, "bo2zm_scripts_issue_" .. i .. "_ui") or ""
        table.insert(items, {label = "ISSUE " .. (i + 1), action = "message",
          hint = issue .. ". Press A for details.", message = issue})
      end
      ExtrasPages.script_issues = items
    end
    local menu = Safely(ExtrasPages[page].custom and CreateWeaponsPage or CreatePage, controller, page)
    if menu then
      return menu
    end
    local popup = LUI.createMenu.Error(controller)
    popup:setMessage("This page could not open.")
    return popup
  end
end

state.pageBuilders = {}  -- {menu name, builder} for each page
for _, page in ipairs(PageNames) do
  if state.zombies() or page == "main" or page == "scripts" or page == "script_targets" or page == "script_issues"
      or page == "experimental" or page == "debug" or page == "purge" then
    table.insert(state.pageBuilders, {PageMenuName(page), PageBuilder(page)})
  end
end

-- An EXTRAS button in a menu's button list. priority: where it goes (the list keeps its buttons in priority order,
-- a new one after those with the same priority).
local function AddExtrasButton(menu, priority)
  local hint = state.zombies() and ExtrasHint or "Your username, experimental features and debug options."
  local button = menu.buttonList:addButton("EXTRAS", hint, priority)
  button:setActionEventName("bo2zm_open_extras")
  menu:registerEventHandler("bo2zm_open_extras", Protect(function(self, event)
    OpenPage(self, event.controller, "main")
  end))
  return button
end

---------------------------------------------------------------------------------------------------------------------
-- CHECK FOR UPDATES (home menu, under EXTRAS): a popup over the home menu, like Extras. The program does the work
-- (bo2zm_updates.h): "bo2zm_update check" asks the update server (a PC on the local network) whether it has an
-- update for this console, and downloads nothing. Builds without an update server have no button: app/CMakeLists.txt
-- sets UpdatesButton to false in the text it builds in. The popup shows what the program reports: bo2zm_update_ui is the
-- state (1 checking, 2 no update server set up, 3 none answered, 4 up to date, 5 update found, 6 restarting, 7 failed),
-- bo2zm_update_text_ui a title and up to three lines separated by '|', bo2zm_update_install_ui 1 when DOWNLOAD AND
-- RESTART can run ("bo2zm_update install": the game relaunches, and its start downloads the update with a progress bar
-- on the start-up screen). B leaves at any step but the restart itself ("bo2zm_update cancel" forgets a pending check).
--
-- The credits line ("Ported by: AlvaroEPena" with GitHub's mark, bo2zm_credits.cpp) is drawn by the program over the
-- home menu, not by the menus: "bo2zm_credits 1" while the home menu is up with nothing over it (again every half
-- second, so it goes away by itself if the menus are reset), 0 as soon as a popup covers it (the game's
-- occlusion_change event, ui/t6/codmenu.lua: CoD.Menu.setOccludedMenu sends it with occluded = true, and again with
-- false when the popup closes) or it closes (LOCAL, OPTIONS).
---------------------------------------------------------------------------------------------------------------------
-- Only these two are used further down (the top level's locals are limited: see the top of the file).
local AddUpdatesButton, TrackCredits
do
  local UpdatesButton = true
  local UpdatesMenu = "Bo2zmUpdates"
  local Checking, UpdateFound, Restarting = 1, 5, 6
  local UpdatesHint = "Asks the update server for a newer build."
  local UpdateButtons = {
    install = {label = "DOWNLOAD AND RESTART", hint = "Restarts the game: the update downloads as it starts, with a "
      .. "progress bar (B there skips it)."},
    check = {label = "CHECK AGAIN", hint = "Asks the update server again."},
  }

  local function Split(text)
    local parts = {}
    for part in string.gmatch(text or "", "[^|]+") do
      table.insert(parts, part)
    end
    return parts
  end

  -- A line of text in the popup, as the game's hints are made (ui/t6/hinttext.lua: an LUI.UIText one line tall,
  -- which wraps at its width).
  local function AddLine(menu, top, height, width)
    local line = LUI.UIText.new()
    line:setLeftRight(true, false, 0, width)
    line:setTopBottom(true, false, top, top + height)
    line:setAlignment(LUI.Alignment.Left)
    menu:addElement(line)
    return line
  end

  local function CreateUpdates(controller)
    local menu = CoD.Menu.New(UpdatesMenu)
    menu:setOwner(controller)
    menu:addTitle("CHECK FOR UPDATES")
    menu:addLargePopupBackground()
    menu.anyControllerAllowed = true  -- as the Extras pages (CreatePage)
    menu:addSelectButton()
    menu:addBackButton()

    local width = CoD.Options and CoD.Options.Width or CoD.ButtonList.DefaultWidth
    local top = CoD.Menu.TitleHeight
    local status = AddLine(menu, top, CoD.CoD9Button.TextHeight, width)
    status:setFont(CoD.fonts.Condensed)
    status:setRGB(CoD.trueOrange.r, CoD.trueOrange.g, CoD.trueOrange.b)
    -- Up to three lines below it, each with room for three wrapped lines.
    local lineRoom = 3 * (CoD.textSize.Default + 4)
    local linesTop = top + CoD.CoD9Button.TextHeight + 12
    local lines = {}
    for i = 1, 3 do
      lines[i] = AddLine(menu, linesTop + (i - 1) * lineRoom, CoD.textSize.Default, width)
      lines[i]:setRGB(CoD.DefaultTextColor.r, CoD.DefaultTextColor.g, CoD.DefaultTextColor.b)
    end
    local listTop = linesTop + 3 * lineRoom + 10

    -- The buttons this state offers, in a new list whenever they change.
    local list, listKey
    local function SetButtons(names)
      local key = table.concat(names, " ")
      if key == listKey then
        return
      end
      listKey = key
      if list then
        list:close()
        list = nil
      end
      if #names == 0 then
        return
      end
      list = CoD.ButtonList.new()
      list:setLeftRight(true, false, 0, width)
      list:setTopBottom(true, false, listTop, listTop + 720)
      menu:addElement(list)
      local first
      for _, name in ipairs(names) do
        local button = list:addButton(UpdateButtons[name].label, UpdateButtons[name].hint)
        button:setActionEventName("bo2zm_update_button")
        button.bo2zmAction = name
        first = first or button
      end
      first:processEvent({name = "gain_focus", controller = controller})
    end

    local shownText
    local function Refresh()
      local state = UIExpression.DvarInt(nil, "bo2zm_update_ui") or 0
      local text = UIExpression.DvarString(nil, "bo2zm_update_text_ui") or ""
      if state == 0 then
        return  -- the check was just asked for: the program shows it from its next frame
      end
      if text ~= shownText then
        shownText = text
        local parts = Split(text)
        status:setText(parts[1] or "")
        for i = 1, 3 do
          lines[i]:setText(parts[i + 1] or "")
        end
      end
      local names = {}
      if state == UpdateFound and UIExpression.DvarInt(nil, "bo2zm_update_install_ui") == 1 then
        table.insert(names, "install")
      end
      if state > Checking and state ~= Restarting then
        table.insert(names, "check")
      end
      SetButtons(names)
    end

    menu:registerEventHandler("bo2zm_update_button", Protect(function(self, event)
      Engine.ExecNow(event.controller or controller, "bo2zm_update " .. event.button.bo2zmAction)
      Refresh()
    end))
    menu:addElement(LUI.UITimer.new(250, "bo2zm_update_refresh", false))
    menu:registerEventHandler("bo2zm_update_refresh", Protect(Refresh))
    -- B: the game's own Back (it closes the popup), after the check is forgotten; nothing while the game restarts.
    local back = menu.m_eventHandlers and menu.m_eventHandlers.button_prompt_back
    menu:registerEventHandler("button_prompt_back", function(self, event)
      if UIExpression.DvarInt(nil, "bo2zm_update_ui") == Restarting then
        return
      end
      Safely(Engine.ExecNow, event.controller or controller, "bo2zm_update cancel")
      if back then
        return back(self, event)
      end
      self:goBack(event.controller or controller)
    end)

    status:setText("Checking for updates")
    Engine.ExecNow(controller, "bo2zm_update check")
    return menu
  end

  table.insert(state.pageBuilders, {UpdatesMenu, function(controller)
    local menu = Safely(CreateUpdates, controller)
    if menu then
      return menu
    end
    local popup = LUI.createMenu.Error(controller)
    popup:setMessage("This screen could not open.")
    return popup
  end})

  -- Keeps an element of the home menu at top/bottom (anchored to the bottom) through the menu's own state changes.
  -- The game registers each element's states while it builds the menu, at the stock place. SYSTEM LINK dims the list
  -- (animateToState "disabled") and shows the flyout ("show"); Back restores both ("default"). The list dropped back
  -- down a row as the flyout opened, so those states evidently carry the stock place; the element is put back after
  -- each one. Left and right, when given, are kept the same way (from the left edge).
  local function Place(element)
    element:setTopBottom(false, true, element.bo2zmTop, element.bo2zmBottom)
    if element.bo2zmLeft then
      element:setLeftRight(true, false, element.bo2zmLeft, element.bo2zmRight)
    end
  end
  local function Pin(element, top, bottom, left, right)
    element.bo2zmTop, element.bo2zmBottom = top, bottom
    element.bo2zmLeft, element.bo2zmRight = left, right
    Place(element)
    if element.bo2zmPinned then
      return
    end
    element.bo2zmPinned = true
    local animate = element.animateToState
    element.animateToState = function(self, ...)
      local a, b = animate(self, ...)
      Place(self)
      return a, b
    end
  end

  -- Right under EXTRAS (priority 4, added after it). The game's list has room for 9 rows above the button prompts
  -- (ui_mp/t6/mainmenu.lua: anchored to the bottom, from -(CoD.CoD9Button.Height * (8 + 1)) - CoD.ButtonPrompt.Height),
  -- enough for the console's 6 buttons and a hint of two lines; a 7th pushed the hint into the prompts' row. One more
  -- row at the top puts every hint back where it was.
  -- Without the button the list keeps the game's place.
  -- The System Link flyout (menu.systemLinkFlyoutContainer, LUI.createMenu.MainMenu in the same file): a container
  -- added after the list, at the default priority (the list has 10, so the list's labels draw over it), from the
  -- list's stock top one row down (SYSTEM LINK's row) plus 2, two rows tall plus 2, its left edge 15 past SYSTEM LINK's
  -- label (GetTextDimensions with CoD.CoD9Button.Font and TextHeight) and as wide as the wider of CREATE GAME and JOIN
  -- GAME plus 12. Its states: "default" (alpha 0) and "show" (alpha 1). Inside, in this order: a black tab (alpha 0.8)
  -- one row tall reaching back from the container's left edge over SYSTEM LINK's label, a black panel (alpha 0.8)
  -- filling the container, and the button list (4 in, its hint closed). The second row it spans is the row under
  -- SYSTEM LINK: LOCAL in the game, ZOMBIES MULTIPLAYER (or MULTIPLAYER) here, wider, so its label ran under the
  -- panel, and the tab put a black bar behind SYSTEM LINK.
  -- Here the game's two black images go, whichever order they are in. The container moves up with the list and
  -- starts 15 past the wider of the two labels it sits beside (each button's own measure, container.textWidth, set
  -- by CoD.CoD9Button.SetLabel). Clear of every label, the buttons need no backing: they sit on the background like
  -- the home list's own, and the flyout keeps the game's priority. Returns the new left and right edges,
  -- {left, right}.
  local function FitFlyout(menu, flyout)
    local list = flyout.buttonList
    if not list then return end
    local function Width(button)
      local container = button and button.container
      if container and type(container.textWidth) == "number" then return container.textWidth end
      local text = button and button.labelString
      if type(text) ~= "string" then return 0 end
      local measure = rawget(_G, "GetTextDimensions")
      if type(measure) == "function" then
        local ok, _, _, right = pcall(measure, text, CoD.CoD9Button.Font, CoD.CoD9Button.TextHeight)
        if ok and type(right) == "number" then return right end
      end
      return #text * CoD.CoD9Button.TextHeight * 0.5
    end
    local labels = math.max(Width(menu.systemLinkButton), Width(menu.localButton))
    local buttons = math.max(Width(flyout.openSystemLinkButton), Width(flyout.openServerBrowserButton))
    local child = flyout:getFirstChild()
    while child do
      local nextChild = child:getNextSibling()
      if child ~= list then child:close() end
      child = nextChild
    end
    local left = 6 + labels + 15
    return {left, left + buttons + 12}
  end

  function AddUpdatesButton(menu)
    local row, prompts = CoD.CoD9Button.Height, CoD.ButtonPrompt.Height
    local top = -(row * (8 + (UpdatesButton and 2 or 1))) - prompts
    Pin(menu.buttonList, top, -prompts)
    local flyout = menu.systemLinkFlyoutContainer
    if flyout then
      local edges = Safely(FitFlyout, menu, flyout) or {}
      Pin(flyout, top + row + 2, top + row + 2 + row * 2 + 2, edges[1], edges[2])
    end
    if not UpdatesButton then
      return
    end
    local button = menu.buttonList:addButton("CHECK FOR UPDATES", UpdatesHint, 4)
    button:setActionEventName("bo2zm_open_updates")
    menu:registerEventHandler("bo2zm_open_updates", Protect(function(self, event)
      self:openPopup(UpdatesMenu, event.controller)
    end))
  end

  function TrackCredits(menu)
    local function Send(shown)
      Engine.ExecNow(0, "bo2zm_credits " .. (shown and "1" or "0"))
    end
    menu:addElement(LUI.UITimer.new(500, "bo2zm_credits_beat", false))
    menu:registerEventHandler("bo2zm_credits_beat", Protect(function(self, event)
      Send(not self.occludedBy)
    end))
    OnEvent(menu, "occlusion_change", function(self, event)
      Send(not event.occluded)
    end, true)
    local close = menu.close
    menu.close = function(self, ...)
      Safely(Send, false)
      return close(self, ...)
    end
    Send(not menu.occludedBy)
  end
end

---------------------------------------------------------------------------------------------------------------------
-- Multiplayer and Zombies share ui_mp/t6/mainmenu.lua. Its buttons have priorities: Xbox LIVE 1,
-- System Link 2, Local 3, Options 4, then the store, Campaign, Multiplayer and Quit (5-9).
--   * "Xbox LIVE" becomes "LOCAL" in both modes, using the game's local lobby action.
--   * "Local" becomes "ZOMBIES MULTIPLAYER" or "MULTIPLAYER": internet play (the online popup below). "System Link" is
--     the game's own, with its flyout of create and join buttons (kept in place over the moved list:
--     AddUpdatesButton); they end online play first ("bo2zm_online off"), so a LAN game uses LAN addresses.
--   * EXTRAS right under OPTIONS (priority 4).
--   * Back opens the game's popup that switches mode with the installed modes only (state.modeInstalled), and only
--     when another mode is installed; the game's own CAMPAIGN / MULTIPLAYER / ZOMBIES buttons (the PC layout) go
--     the same way.
---------------------------------------------------------------------------------------------------------------------

---------------------------------------------------------------------------------------------------------------------
-- Internet play (ZOMBIES MULTIPLAYER / MULTIPLAYER on the home menu): a popup over the home menu, like CHECK FOR
-- UPDATES. The program does the work (bo2zm_online.cpp and the SDK's online_link.cpp): "bo2zm_online host" opens the
-- router's ports (UPnP) and finds the console's public address (STUN), then shows an 8-character join code (XXXX-XXXX);
-- "bo2zm_online join" asks for a code with the keyboard and prepares the same way for joining it. Both then meet
-- through public MQTT brokers (the "matchmaking relay", online_relay.cpp) and open each other's NAT with UDP hole
-- punching, for routers that ignore UPnP and for carrier NATs; the status lines say how far that got. Forwarding the
-- ports by hand stays the last resort (the help line). The game then runs an ordinary System
-- Link game over the internet: CONTINUE TO LOBBY is SYSTEM LINK's create button (CoD.MainMenu.OpenSystemLinkLobby),
-- FIND GAME its join button (CoD.MainMenu.OpenServerBrowser). The program reports bo2zm_online_ui (0 nothing yet,
-- 1 preparing, 2 ready to host, 3 ready to join, 4 failed), bo2zm_online_text_ui (the title), bo2zm_online_step1_ui
-- to bo2zm_online_step12_ui (the checklist, one line each, empty past the last), bo2zm_online_code_ui and bo2zm_online_help_ui (what to try when players cannot connect: under
-- every step, and in the server browser and the lobby too). B while preparing cancels ("bo2zm_online cancel").
---------------------------------------------------------------------------------------------------------------------
local OnlineMenu = "Bo2zmOnline"
do
  local Preparing, HostReady, JoinReady = 1, 2, 3
  local OnlineButtons = {
    host = {label = "HOST ONLINE GAME", hint = "Opens the router's ports and gives you a join code to share."},
    join = {label = "JOIN WITH CODE", hint = "Type the code the host's screen shows."},
    lobby = {label = "CONTINUE TO LOBBY", hint = "Opens your game's lobby; players join it with your code."},
    find = {label = "FIND GAME", hint = "Looks for the host's game: the host must be in its lobby."},
  }

  local function Help()
    local help = UIExpression.DvarString(nil, "bo2zm_online_help_ui") or ""
    if help == "" then
      help = "Can't connect? If UPnP and the relay both fail, the host can forward UDP ports 31000-31002 to their console."
    end
    return help
  end

  -- A line of text, as CHECK FOR UPDATES makes them (an LUI.UIText one line tall, which wraps at its width).
  local function AddLine(menu, top, height, width, bottomAnchored)
    local line = LUI.UIText.new()
    line:setLeftRight(true, false, 0, width)
    line:setTopBottom(not bottomAnchored, bottomAnchored == true, top, top + height)
    line:setAlignment(LUI.Alignment.Left)
    menu:addElement(line)
    return line
  end

  -- One line above a menu's button prompts (the server browser, the lobby): the code and what to try.
  state.onlineLine = function(menu, text)
    if not (LUI.UIText and LUI.UIText.new) or menu.bo2zmOnlineLine then return end
    local height = CoD.textSize and CoD.textSize.Default or 20
    local prompts = CoD.ButtonPrompt and CoD.ButtonPrompt.Height or 30
    local line = AddLine(menu, -(prompts + 2 + height), height, 1100, true)
    line:setRGB(CoD.trueOrange.r, CoD.trueOrange.g, CoD.trueOrange.b)
    line:setText(text)
    menu.bo2zmOnlineLine = line
  end

  local function CreateOnline(controller)
    local menu = CoD.Menu.New(OnlineMenu)
    menu:setOwner(controller)
    menu:addTitle(state.zombies() and "ZOMBIES MULTIPLAYER" or "MULTIPLAYER")
    menu:addLargePopupBackground()
    menu.anyControllerAllowed = true
    menu:addSelectButton()
    menu:addBackButton()

    local width = CoD.Options and CoD.Options.Width or CoD.ButtonList.DefaultWidth
    local top = CoD.Menu.TitleHeight
    local status = AddLine(menu, top, CoD.CoD9Button.TextHeight, width)
    status:setFont(CoD.fonts.Condensed)
    status:setRGB(CoD.trueOrange.r, CoD.trueOrange.g, CoD.trueOrange.b)
    -- The program's steps (a checklist that stays up as each one finishes), then the help. One row each, two for a
    -- line too long for one; the rows close up under the last step, and the buttons (with their hint under them)
    -- follow right after, so they stay clear of the button prompts.
    local rowHeight = CoD.textSize.Default + 4
    local linesTop = top + CoD.CoD9Button.TextHeight + 12
    local maxSteps = 12
    local lines = {}
    for i = 1, maxSteps + 1 do
      lines[i] = AddLine(menu, linesTop + (i - 1) * rowHeight, CoD.textSize.Default, width)
      lines[i]:setRGB(CoD.DefaultTextColor.r, CoD.DefaultTextColor.g, CoD.DefaultTextColor.b)
    end
    local perRow = math.max(20, math.floor(width / (CoD.textSize.Default * 0.45)))
    local listTop = linesTop + 4 * rowHeight
    local list
    local function Place(texts)
      local y = linesTop
      for i, line in ipairs(lines) do
        local text = texts[i] or ""
        if i == #lines then y = y + rowHeight / 2 end  -- a gap before the help
        line:setTopBottom(true, false, y, y + CoD.textSize.Default)
        line:setText(text)
        if text ~= "" then y = y + rowHeight * (string.len(text) > perRow and 2 or 1) end
      end
      listTop = y + 12
      if list then list:setTopBottom(true, false, listTop, listTop + 720) end
    end

    local listKey
    local function SetButtons(names)
      local key = table.concat(names, " ")
      if key == listKey then return end
      listKey = key
      if list then
        list:close()
        list = nil
      end
      if #names == 0 then return end
      list = CoD.ButtonList.new()
      list:setLeftRight(true, false, 0, width)
      list:setTopBottom(true, false, listTop, listTop + 720)
      menu:addElement(list)
      local first
      for _, name in ipairs(names) do
        local button = list:addButton(OnlineButtons[name].label, OnlineButtons[name].hint)
        button:setActionEventName("bo2zm_online_button")
        button.bo2zmAction = name
        first = first or button
      end
      first:processEvent({name = "gain_focus", controller = controller})
    end
    menu.bo2zmOnlineButtons = function() return list end  -- for the menu test

    local shownText
    local function Refresh()
      local onlineState = UIExpression.DvarInt(nil, "bo2zm_online_ui") or 0
      -- The title, then each line in a dvar of its own (bo2zm_online.cpp: a dvar holds about 255 characters).
      local texts = {}
      local key = UIExpression.DvarString(nil, "bo2zm_online_text_ui") or ""
      for i = 1, maxSteps do
        texts[i] = UIExpression.DvarString(nil, "bo2zm_online_step" .. i .. "_ui") or ""
        key = key .. "|" .. texts[i]
      end
      texts[maxSteps + 1] = Help()
      key = key .. "|" .. texts[maxSteps + 1]
      if key ~= shownText then
        shownText = key
        status:setText(UIExpression.DvarString(nil, "bo2zm_online_text_ui") or "")
        Place(texts)
      end
      if onlineState == Preparing then
        SetButtons({})
      elseif onlineState == HostReady then
        SetButtons({"lobby", "join"})
      elseif onlineState == JoinReady then
        SetButtons({"find", "join", "host"})
      else
        SetButtons({"host", "join"})
      end
    end

    -- The home menu's own System Link action, with this popup closed first.
    local function OpenGame(self, event, field, stockName)
      local home = self.bo2zmHome
      local c = event.controller or controller
      if self.goBack then self:goBack(c) else self:close() end
      local stock = home and home.bo2zmOnlineStock and home.bo2zmOnlineStock[field]
      local mainMenu = CoD.MainMenu
      stock = stock or (mainMenu and mainMenu[stockName])
      if home and stock then
        stock(home, {name = field, controller = c})
      else
        Log("online: the home menu's " .. stockName .. " is missing")
      end
    end

    menu:registerEventHandler("bo2zm_online_button", Protect(function(self, event)
      local action = event.button and event.button.bo2zmAction
      if action == "lobby" then
        return OpenGame(self, event, "lobby", "OpenSystemLinkLobby")
      elseif action == "find" then
        return OpenGame(self, event, "browser", "OpenServerBrowser")
      elseif action then
        Engine.ExecNow(event.controller or controller, "bo2zm_online " .. action)
      end
      Refresh()
    end))
    menu:addElement(LUI.UITimer.new(250, "bo2zm_online_refresh", false))
    menu:registerEventHandler("bo2zm_online_refresh", Protect(Refresh))
    local back = menu.m_eventHandlers and menu.m_eventHandlers.button_prompt_back
    menu:registerEventHandler("button_prompt_back", function(self, event)
      if UIExpression.DvarInt(nil, "bo2zm_online_ui") == Preparing then
        Safely(Engine.ExecNow, event.controller or controller, "bo2zm_online cancel")
      end
      if back then return back(self, event) end
      if self.goBack then self:goBack(event.controller or controller) else self:close() end
    end)
    Refresh()
    return menu
  end

  table.insert(state.pageBuilders, {OnlineMenu, function(controller)
    local menu = Safely(CreateOnline, controller)
    if menu then return menu end
    local popup = LUI.createMenu.Error(controller)
    popup:setMessage("This screen could not open.")
    return popup
  end})

  -- The server browser FIND GAME opens, and the lobby of an online host.
  state.onlineBrowser = function(menu)
    if UIExpression.DvarInt(nil, "bo2zm_online_ui") == JoinReady then
      state.onlineLine(menu, "Joining " .. (UIExpression.DvarString(nil, "bo2zm_online_code_ui") or "") .. ". "
        .. Help())
    end
  end
  -- Online, the lobby (host and joiner alike) and the server browser are titled like the home menu's button (ZOMBIES
  -- MULTIPLAYER or MULTIPLAYER) instead of SYSTEM LINK. The game titles them through the menu itself:
  --   * LUI.createMenu.PrivateLocalGameLobby (ui/t6/menus/privatelocalgamelobby.lua) calls
  --     menu:addTitle(Engine.Localize("PLATFORM_SYSTEM_LINK_CAPS")) and keeps the same string as its button pane's
  --     titleText (menu.panelManager.panels.buttonPane). CoD.Lobby.CurrentPanelChanged (ui/t6/lobby.lua) sets the
  --     title again from the current pane's titleText whenever the panel manager changes panes.
  --   * LUI.createMenu.ServerBrowser (ui/t6/menus/serverbrowserpopup.lua) calls
  --     menu:addTitle(Engine.Localize("PLATFORM_SYSTEM_LINK_GAMES_CAPS")): "SYSTEM LINK GAMES", not "SYSTEM LINK".
  --   * CoD.Menu.addTitle makes menu.titleElement and calls menu:setTitle, which keeps the text in menu.titleText and
  --     shows it (ui/t6/codmenu.lua).
  -- So once the game has built the menu, its setTitle answers the online title for either stock text, the panes'
  -- titles are swapped (for later pane changes), and the title is set again.
  state.onlineTitled = function(original)
    return function(controller, ...)
      local menu = original(controller, ...)
      local online = UIExpression.DvarInt(nil, "bo2zm_online_ui")
      if menu and (online == HostReady or online == JoinReady) then
        Safely(state.retitle, menu)
      end
      return menu
    end
  end
  state.retitle = function(menu)
    if menu.bo2zmRetitled or type(menu.setTitle) ~= "function" then return end
    menu.bo2zmRetitled = true
    local title = state.zombies() and "ZOMBIES MULTIPLAYER" or "MULTIPLAYER"
    local stock = {["SYSTEM LINK"] = true, ["SYSTEM LINK GAMES"] = true}
    for _, key in ipairs({"PLATFORM_SYSTEM_LINK_CAPS", "PLATFORM_SYSTEM_LINK_GAMES_CAPS"}) do
      local ok, text = pcall(Engine.Localize, key)
      if ok and type(text) == "string" then stock[text] = true end
    end
    local function Retitle(text)
      if type(text) == "string" and (stock[text] or stock[string.upper(text)]) then return title end
      return text
    end
    local setTitle = menu.setTitle
    menu.setTitle = function(self, text, ...)
      return setTitle(self, Retitle(text), ...)
    end
    local panels = menu.panelManager and menu.panelManager.panels
    if type(panels) == "table" then
      for _, pane in pairs(panels) do
        if pane.titleText ~= nil then pane.titleText = Retitle(pane.titleText) end
      end
    end
    if menu.titleElement then
      menu:setTitle(menu.titleText or title)
    end
    Log("online: " .. tostring(menu.menuName or "menu") .. " titled " .. title)
  end
  state.onlineLobby = function(menu)
    if UIExpression.DvarInt(nil, "bo2zm_online_ui") == HostReady then
      state.onlineLine(menu, "JOIN CODE " .. (UIExpression.DvarString(nil, "bo2zm_online_code_ui") or "") .. ". "
        .. Help())
    end
  end
end

local function FixSoloButton(menu)
  local solo = menu.mainLobbyButton
  if not solo then
    return
  end
  solo:setLabel("LOCAL")
  solo.hintText = menu.bo2zmSoloHint
  solo:setActionEventName("open_local_match_lobby")
  if solo.disabled then
    solo.disabled = nil
    solo:animateToState("default")
  end
end

local function PatchMainMenu(menu, controller)
  local localButton = menu.localButton
  if localButton then
    menu.bo2zmSoloHint = localButton.hintText
    localButton:setLabel(state.zombies() and "ZOMBIES MULTIPLAYER" or "MULTIPLAYER")
    localButton.hintText = "Play over the internet: host a game with a join code, or join one."
    localButton:setActionEventName("bo2zm_open_online")
  end
  FixSoloButton(menu)
  Refocus(menu.mainLobbyButton, controller)
  -- Signing in or out makes the game update the Xbox LIVE button again: restore LOCAL right after.
  OnEvent(menu, "first_signed_in", FixSoloButton, true)
  OnEvent(menu, "last_signed_out", FixSoloButton, true)

  menu:registerEventHandler("bo2zm_open_online", Protect(function(self, event)
    local popup = self:openPopup(OnlineMenu, event.controller)
    if popup then popup.bo2zmHome = self end
  end))
  -- SYSTEM LINK's own create and join buttons play on the LAN: online play ends first. The online popup calls the
  -- game's handlers directly (bo2zmOnlineStock).
  local handlers = menu.m_eventHandlers or {}
  menu.bo2zmOnlineStock = {lobby = handlers.open_system_link_lobby, browser = handlers.open_server_browser}
  for field, name in pairs({lobby = "open_system_link_lobby", browser = "open_server_browser"}) do
    local stock = menu.bo2zmOnlineStock[field]
    if stock then
      menu:registerEventHandler(name, function(self, event)
        Safely(Engine.ExecNow, event.controller or controller, "bo2zm_online off")
        return stock(self, event)
      end)
    end
  end

  AddExtrasButton(menu, 4)
  AddUpdatesButton(menu)
  TrackCredits(menu)

  -- The home menu's Back is CoD.MainMenu.Back. The System Link flyout takes B while it is open
  -- (CoD.MainMenu.OpenSystemLinkFlyout registers CloseSystemLinkFlyout and adds its own Back prompt); closing it
  -- registers CoD.MainMenu.Back again with a new prompt, so the port's Back is applied each time that happens. A home
  -- menu rebuilt with SYSTEM LINK focused opens with the flyout already up: B keeps closing it.
  -- CoD.MainMenu.Back opens the game's ConfirmLeave popup with one entry per other mode ({leaveText, leaveHandler,
  -- leaveEvent}, the events start_sp, start_mp and start_zombies); entries for modes that are not installed are left
  -- out on the way to the popup.
  local CoDTable = rawget(_G, "CoD")
  local stockBack = CoDTable and CoDTable.MainMenu and CoDTable.MainMenu.Back
  local leaveModes = {start_sp = "cp", start_mp = "mp", start_zombies = "zm"}
  local function HomeBack(self, event)
    if not (stockBack and state.otherModeInstalled()) then
      return
    end
    local own = self.openPopup  -- a menu is userdata here: no rawget
    self.openPopup = function(home, name, popupController, options, ...)
      home.openPopup = own
      if name == "ConfirmLeave" and type(options) == "table" and type(options.params) == "table" then
        local kept = {}
        for _, entry in ipairs(options.params) do
          local mode = type(entry) == "table" and leaveModes[entry.leaveEvent]
          if not mode or state.modeInstalled(mode) then table.insert(kept, entry) end
        end
        options.params = kept
      end
      return home:openPopup(name, popupController, options, ...)
    end
    local ok, a, b = pcall(stockBack, self, event)
    self.openPopup = own
    if not ok then error(a, 0) end
    return a, b
  end
  local register = menu.registerEventHandler
  menu.registerEventHandler = function(self, name, handler, ...)
    if name ~= "button_prompt_back" or handler ~= stockBack or not stockBack then
      return register(self, name, handler, ...)
    end
    local a, b = register(self, name, HomeBack, ...)
    if not state.otherModeInstalled() then
      CloseField(self, "backButton")
    end
    return a, b
  end
  for field, mode in pairs({spButton = "cp", mpButton = "mp", zombieButton = "zm"}) do
    if not state.modeInstalled(mode) then CloseField(menu, field) end
  end
  local current = menu.m_eventHandlers and menu.m_eventHandlers.button_prompt_back
  if stockBack and current == stockBack then
    menu:registerEventHandler("button_prompt_back", stockBack)
  end
end

---------------------------------------------------------------------------------------------------------------------
-- Pause menu in a match (ui_mp/t6/hud/class.lua): CoD.Class.AddButton(menu, label, event, disabled) adds each button
-- in order, with no priority (and again when the list is rebuilt); EXTRAS follows OPTIONS, so it sits above END
-- GAME. With MARK LOCATION and DEBUG on (Extras > DEBUG; DEBUG is the page's master switch) the list also gets MARK
-- LOCATION at the top: priority -1 puts it before every button without one (a button goes in before the first sibling
-- of a higher priority). It runs bo2zm_mark_here and closes the pause menu the way RESUME GAME does
-- (CoD.Class.ResumeGameButtonPressed sends the menu a button_prompt_back), so the program's screenshots show the game,
-- not the menu (bo2zm_debug_options.cpp).
---------------------------------------------------------------------------------------------------------------------
local function AddMarkLocationButton(menu)
  if UIExpression.DvarInt(nil, "bo2zm_mark_location_ui") ~= 1 or UIExpression.DvarInt(nil, "bo2zm_dbg_ui") ~= 1 then
    return
  end
  local button = menu.buttonList:addButton("MARK LOCATION", nil, -1)
  button:setActionEventName("bo2zm_mark_here")
  menu:registerEventHandler("bo2zm_mark_here", Protect(function(self, event)
    local controller = event.controller or 0
    Engine.ExecNow(controller, "bo2zm_mark_here")
    self:processEvent({name = "button_prompt_back", controller = controller})
  end))
end

local WrapPauseAddButton;
(function()
  -- sv_running/CoD.isHost describes the whole game. The menu owner must also be the primary local client;
  -- its controller need not be port zero. Stock lobby code uses this expression to distinguish local members.
  function state.primaryController(controller)
    if not state.validController(controller) or type(UIExpression.IsPrimaryLocalClient) ~= "function" then return false end
    local known, primary = pcall(UIExpression.IsPrimaryLocalClient, controller)
    return known and (primary == 1 or primary == true)
  end
  local function IsPauseLeader(menu, event, timer)
    if not menu or type(menu.getOwner) ~= "function"
        or type(UIExpression.IsPrimaryLocalClient) ~= "function" then return false end
    local ok, owner = pcall(menu.getOwner, menu)
    if not ok or type(owner) ~= "number" or owner < 0 or owner > 3 or owner % 1 ~= 0 then return false end
    if event and event.controller ~= nil then
      if event.controller ~= owner then return false end
    elseif event ~= nil and not timer then
      return false
    end
    return state.primaryController(owner)
  end

  local function Guard(original, timer)
    return function(menu, event, ...)
      if type(event) == "table" and IsPauseLeader(menu, event, timer) then return original(menu, event, ...) end
    end
  end

  WrapPauseAddButton = function(original)
    return function(menu, label, event, ...)
      if (event == "open_endGamePopup" or event == "openRestartGamePopup") and not IsPauseLeader(menu) then
        return
      end
      local button = original(menu, label, event, ...)
      if event == "open_options" then
        Safely(AddExtrasButton, menu)
        Safely(AddMarkLocationButton, menu)
      end
      return button
    end
  end

  state.installHostPause = function(wrap, createMenu, cod)
    for _, entry in ipairs({
      {cod.Class, {"EndGameButtonPressed", "RestartGameButtonPressed"}},
      {cod.EndGamePopup, {"YesButtonPressed", "FinishEndGame"}},
      {cod.RestartGamePopup, {"YesButtonPressed", "CancelUpload"}},
    }) do
      for _, name in ipairs(entry[2]) do wrap(entry[1], name, Guard) end
    end
    -- The demo-stream timer has no controller field. Its popup owner still has to be primary when it fires.
    for _, name in ipairs({"WaitForDemostream", "RestartLevel"}) do
      wrap(cod.RestartGamePopup, name, function(original) return Guard(original, true) end)
    end
    -- Constructors may have retained a stock callback before this installation. Guard the registered events too.
    for name, events in pairs({
      class = {open_endGamePopup = false, openRestartGamePopup = false},
      EndGamePopup = {endGamePopup_YesButtonPressed = false},
      RestartGamePopup = {restartGamePopup_YesButtonPressed = false, cancelUpload = false, waitForDemoStream = true},
    }) do
      wrap(createMenu, name, function(original)
        return function(...)
          local menu = original(...)
          if menu and menu.m_eventHandlers then
            for event, timer in pairs(events) do
              local handler = menu.m_eventHandlers[event]
              if type(handler) == "function" then menu:registerEventHandler(event, Guard(handler, timer)) end
            end
          end
          return menu
        end
      end)
    end
  end
end)()

---------------------------------------------------------------------------------------------------------------------
-- Lobby and playlist buttons are enabled repeatedly by host and game-state updates. LUI.UIButton.enable
-- always sends "button_up", which takes the focused button's highlight down and starts its pulse over (it gains focus
-- again). With lobby updates arriving all the time the highlight blinked at random. Its enable and disable now only
-- act when the state changes.
---------------------------------------------------------------------------------------------------------------------
local function SteadyButton(button)
  if not button or button.bo2zmSteady then
    return
  end
  button.bo2zmSteady = true
  local enable, disable = button.enable, button.disable
  button.enable = function(self, ...)
    if self.disabled then
      return enable(self, ...)
    end
  end
  button.disable = function(self, ...)
    if not self.disabled then
      return disable(self, ...)
    end
  end
  -- Some lobby events save the method itself when the button is created.
  for event, handler in pairs(button.m_eventHandlers or {}) do
    if handler == enable then
      button:registerEventHandler(event, button.enable)
    elseif handler == disable then
      button:registerEventHandler(event, button.disable)
    end
  end
end

local function WrapSteadyButtonNew(original)
  return function(...)
    local button = original(...)
    Safely(SteadyButton, button)
    return button
  end
end

-- START MATCH is self-explanatory. Its help occupied the countdown's area
-- after extra lobby rows were appended, so leave that help slot empty.
local function ClearStartMatchHint(pane)
  local body = pane and pane.body
  local button = body and body.startMatchButton
  if not button then return end
  local function Clear(self)
    if self.hintText == Engine.Localize("MPUI_START_MATCH_DESC") then self.hintText = nil end
    local list = body.buttonList
    if self:isInFocus() and list and list.updateHintText then list:updateHintText(self) end
  end
  if not button.bo2zmStartHint then
    button.bo2zmStartHint = true
    OnEvent(button, "gamelobby_update", Clear, true)
    OnEvent(button, "gain_focus", Clear, true)
    OnEvent(button, "start_game", Clear, true)
  end
  Clear(button)
end

-- The LOCAL Zombies lobby owns these rows, including when its button pane is
-- rebuilt. Read all three UI tokens together; an empty start location is "-"
-- on the command bridge. A changed lobby never reuses the previous map's pick.
-- A System Link lobby this console hosts gets the same row (systemLink = true): the
-- server runs here, so the bots do too. Its players are not all local controllers, so the
-- lobby reports its player count (Engine.PartyGetPlayerCount, which the game's own
-- start-location menu compares with a game type's player limit) for the program's
-- four-slot guard. A console that joined has no row and sends nothing.
local PatchBotsLobby
;(function()
  -- Construct private helpers in their own frame; a do block still uses the
  -- main chunk's registers while these closures are created.
  local labelSpan = 220
  local function AlignChoices(selector, list)
    local label = selector.label
    if not label or not selector.horizontalList then return end
    local left, _, right = label:getRect()
    if not left or not right or right <= left then return end
    local stock = list:getFirstChild()
    while stock do
      local control = stock.leftArrow or stock.choiceText
      if stock.m_settingName and control then
        local column = control:getRect()
        if column and column > left then
          -- getRect is in root coordinates; the fixed label span converts back to local units.
          column = (column - left) * labelSpan / (right - left)
          if selector.bo2zmChoiceColumn ~= column then
            selector.bo2zmChoiceColumn = column
            selector.horizontalList:setLeftRight(true, true, column, 0)
            selector.horizontalList:setLayoutCached(false)
          end
          return
        end
      end
      stock = stock:getNextSibling()
    end
  end

  local function Context()
    local map = UIExpression.DvarString(nil, "ui_mapname") or ""
    local gametype = UIExpression.DvarString(nil, "ui_gametype") or ""
    local start = UIExpression.DvarString(nil, "ui_zm_mapstartlocation") or ""
    local function Token(value, limit)
      return #value > 0 and #value <= limit and string.match(value, "^[a-z0-9_]+$") ~= nil
    end
    if string.sub(map, 1, 3) ~= "zm_" or map == "zm_frontend" or #map <= 3 or not Token(map, 48)
        or not Token(gametype, 32) or (start ~= "" and not Token(start, 48)) then
      return nil
    end
    return map .. " " .. gametype .. " " .. (start == "" and "-" or start)
  end

  -- The stock PopulateButtons_Project_Zombie (ui/t6/menus/privategamelobby.lua) creates the MAP row with its name and
  -- never fills in a value. The value is the same pair the lobby's map card shows (ui/t6/menus/mapinfoimage.lua): the
  -- map table's name column for the map and the game type table's name column for the start location, upper-cased.
  -- Before a map is picked (ui_mapname "zm_frontend", no row) the row keeps its own name, as the stock lobby leaves it.
  local function ShowMapChoice(body)
    local button = body and body.changeMapButton
    if not button then return end
    local label = Engine.Localize("ZMUI_MAP_CAPS")
    local map = UIExpression.DvarString(nil, "ui_mapname") or ""
    local maps = UIExpression.GetCurrentMapTableName()
    local key = type(maps) == "string" and map ~= "" and UIExpression.TableLookup(nil, maps, 0, map, 3) or nil
    if type(key) == "string" and key ~= "" then
      label = label .. ": " .. UIExpression.ToUpper(nil, Engine.Localize(key))
      local start = UIExpression.DvarString(nil, "ui_zm_mapstartlocation") or ""
      local location = start ~= "" and UIExpression.TableLookup(nil, CoD.gametypesTable, 0, 5, 3, start, 4) or nil
      if type(location) == "string" and location ~= "" then
        label = label .. " / " .. UIExpression.ToUpper(nil, Engine.Localize(location))
      end
    end
    if button.label ~= label then button:setLabel(label) end
  end

  -- CoD.PrivateGameLobby.IsHost (ui/t6/menus/privategamelobby.lua): out of a lobby, or its host.
  local function Hosting(menu, controller)
    local lobby = CoD.PrivateGameLobby
    if not lobby or type(lobby.IsHost) ~= "function" then return false end
    local ok, host = pcall(lobby.IsHost, menu, controller)
    return ok and host == true
  end

  local function ReportPlayers(menu, controller)
    local ok, players = pcall(Engine.PartyGetPlayerCount)
    players = ok and tonumber(players) or nil
    if not players or players ~= math.floor(players) then return end
    players = math.max(1, math.min(4, players))
    if menu.bo2zmPlayers ~= players then
      menu.bo2zmPlayers = players
      Engine.ExecNow(controller, "bo2zm_bots_humans " .. tostring(players))
    end
  end

  local function Refresh(menu, controller)
    if not state.zombies() or InGame() then return end
    local pane = menu.buttonPane
    local body = pane and pane.body
    Safely(ShowMapChoice, body)
    local list = body and body.buttonList
    if not list or not list.addDvarLeftRightSelector then return end
    if menu.bo2zmSystemLink then
      if not Hosting(menu, controller) then
        if body.bo2zmBotCount then
          body.bo2zmBotCount:close()
          body.bo2zmBotCount = nil
        end
        return
      end
      ReportPlayers(menu, controller)
    end
    local context = Context()
    if context then Engine.ExecNow(controller, "bo2zm_bots_context " .. context) end
    if not body.bo2zmBotCount then
      local selector = list:addDvarLeftRightSelector(controller, "BOT COUNT", "bo2zm_bots_count_ui", "", 170)
      body.bo2zmBotCount = selector
      -- Keep the label outside the flowing choices so neither text width sets their column.
      if selector.label and selector.horizontalList then
        selector.horizontalList:removeElement(selector.label)
        selector:addElement(selector.label)
        selector.label:setLeftRight(true, false, 0, labelSpan)
        selector.horizontalList:setLeftRight(true, true, labelSpan, 0)
      end
      SteadyButton(selector)
      selector:registerEventHandler("start_game", selector.disable)
      selector:registerEventHandler("cancel_start_game", selector.enable)
    end
    local selector = body.bo2zmBotCount
    local installed = UIExpression.DvarInt(nil, "bo2zm_bots_available_ui") == 1
    local maximum = math.max(0, math.min(3, UIExpression.DvarInt(nil, "bo2zm_bots_max_ui")))
    if selector.bo2zmInstalled ~= installed or selector.bo2zmMaximum ~= maximum then
      selector.bo2zmInstalled = installed
      selector.bo2zmMaximum = maximum
      selector:clearChoices()
      selector.m_currentChoice = nil
      selector.m_lastChoice = nil
      if installed then
        for count = 0, maximum do
          local label = count == 0 and "OFF" or tostring(count)
          selector:addChoice(controller, label, count, nil, Protect(function(params, userRequested)
            if userRequested then
              local current = Context()
              if not state.zombies() or InGame() or not current
                  or UIExpression.DvarInt(nil, "bo2zm_bots_available_ui") ~= 1
                  or count > UIExpression.DvarInt(nil, "bo2zm_bots_max_ui") then return false end
              Engine.ExecNow(controller, "bo2zm_bots_count " .. current .. " " .. tostring(count))
              return UIExpression.DvarInt(nil, "bo2zm_bots_count_ui") == count
            end
          end))
        end
      else
        selector:addChoice(controller, "NOT INSTALLED", -1, nil, function() end)
      end
      ApplyStatus(selector, {status = not installed or maximum == 0})
      selector:refreshChoice()
      selector:updateChoice(false)
    end
    local hint = installed and "Bots join next match." or "Install bot files to enable."
    if installed and maximum == 0 then hint = "All player slots are in use." end
    if installed and not context then hint = "Choose a map first." end
    local detail = UIExpression.DvarString(nil, "bo2zm_bots_status_ui") or ""
    if installed and string.find(detail, "Bots unavailable:", 1, true) then
      hint = "Unavailable with this mod."
    end
    selector.hintText = hint
    selector:refreshChoice()
    AlignChoices(selector, list)
  end

  function PatchBotsLobby(menu, controller, systemLink)
    if not state.zombies() or InGame() then return end
    controller = controller or 0
    if menu.buttonPane then menu.buttonPane.bo2zmBotMenu = menu end
    if not menu.bo2zmBotTimer then
      menu.bo2zmBotTimer = true
      menu.bo2zmSystemLink = systemLink == true
      menu.bo2zmPlayers = nil
      Engine.ExecNow(controller, "bo2zm_bots_open")
      menu.bo2zmBotController = controller
      menu:addElement(LUI.UITimer.new(250, "bo2zm_refresh_bots", false))
      OnEvent(menu, "bo2zm_refresh_bots", function(self) Refresh(self, controller) end, true)
    end
    Refresh(menu, controller)
  end
end)()

local function WrapLobbyPopulate(original)
  return function(pane, ...)
    local restoreBotFocus = pane and pane.body and pane.body.bo2zmBotCount
        and pane.body.bo2zmBotCount:isInFocus()
    if pane and pane.bo2zmBotMenu and pane.body then
      pane.body.bo2zmBotCount = nil
    end
    local a, b = original(pane, ...)
    if pane and pane.body then
      Safely(SteadyButton, pane.body.startMatchButton)
      local list = pane.body.buttonList
      local button = list and list:getFirstChild()
      while button do
        if type(button.enable) == "function" and type(button.disable) == "function" then
          Safely(SteadyButton, button)
        end
        button = button:getNextSibling()
      end
      ClearStartMatchHint(pane)
      if pane.bo2zmBotMenu then
        Safely(PatchBotsLobby, pane.bo2zmBotMenu, pane.bo2zmBotMenu.bo2zmBotController)
        if restoreBotFocus and pane.body.bo2zmBotCount and pane.body.bo2zmBotCount:isFocusable() then
          local controller = pane.bo2zmBotMenu.bo2zmBotController
          local focused = list and list:getFirstChild()
          while focused do
            if type(focused.isInFocus) == "function" and focused:isInFocus() then
              focused:processEvent({name = "lose_focus", controller = controller})
            end
            focused = focused:getNextSibling()
          end
          pane.body.bo2zmBotCount:processEvent({name = "gain_focus", controller = controller})
        end
      end
    end
    return a, b
  end
end

---------------------------------------------------------------------------------------------------------------------
-- Options > Graphics, in the front end and in a match (ui/t6/graphicssettingsoptions.lua): only BRIGHTNESS and DRAW
-- CROSSHAIR stay; the Anaglyph 3D selector becomes GRAPHICS PRESET, followed by each graphics option (a change makes
-- the preset CUSTOM) and the display settings. The program saves them and shows the preset in the top-right note.
---------------------------------------------------------------------------------------------------------------------
local function PatchGraphicsSettings(menu, controller)
  CloseField(menu, "safeAreaButton")
  CloseField(menu, "stereoscopic3dButton")
  CloseField(menu, "dualViewButton")
  -- The game's handler enables or disables the buttons just closed.
  menu:registerEventHandler("update_buttonlist", function(self, event)
  end)

  local preset = menu.anaglyphButton
  if preset then
    preset:setLabel(PresetItem.label)
    preset.hintText = PresetItem.hint
    preset.m_dvarName = ItemDvar(PresetItem)
    preset:clearChoices()
    preset.m_currentChoice = nil
    preset.m_lastChoice = nil
    preset.currentValue = preset:getCurrentValue()
    AddChoices(menu, preset, controller, PresetItem)
    if preset.disabled then
      preset.disabled = nil
      preset:animateToState("default")
    end
    local list = preset:getParent()
    if list and list.addDvarLeftRightSelector and not menu.bo2zmAdded then
      menu.bo2zmAdded = true
      local function Add(item)
        local selector = list:addDvarLeftRightSelector(controller, item.label, ItemDvar(item), item.hint)
        AddChoices(menu, selector, controller, item)
        ApplyStatus(selector, item)
        return selector
      end
      AddHeading(list, "CUSTOM SETTINGS")
      for _, item in ipairs(GraphicsItems) do
        if item.setting == "gfx_distance" and not state.zombies() then
          Add({label = item.label, setting = item.setting, choices = item.choices,
               hint = "How far the world is drawn. Shorter distances improve frame rate but hide distant scenery. "
                 .. "FAR OBJECTS on SIMPLE keeps large distant shapes visible."})
        else
          Add(item)
        end
      end
      AddHeading(list, "DISPLAY")
      Add(FpsCapItem)
      Add(VsyncItem)
      if UIExpression.DvarString(nil, "bo2zm_platform_ui") == "pc" then Add(WindowItem) end
      Add(ShowPresetItem)
      Add(FpsCounterItem)
      -- The map profile's two selectors go above everything else (Zombies' maps only).
      if list.addElementToTop and list.removeElement then
        local top = {}
        local profileItems = InGame() and {ProfileInUseItem, EnableProfileGameItem}
                               or {MapProfileItem, EnableProfileItem}
        if not state.zombies() then
          profileItems = {
            {label = "PROFILE IN USE", setting = "profile", choices = {"GLOBAL"}, status = true},
            {label = "ENABLE PROFILE", setting = "profile_on", choices = {"OFF", "ON", "N/A"}, status = true},
          }
        end
        for _, item in ipairs(profileItems) do
          table.insert(top, Add(item))
        end
        for i = #top, 1, -1 do
          if top[i] then
            list:removeElement(top[i])
            list:addElementToTop(top[i])
          end
        end
      end
      -- Always taller than the screen now. The hint lines up with the rows at 0 (at the game's
      -- CoD.Menu.SplitscreenSideOffset, 28, it stood that far to the right).
      FitList(menu, list, 0, CoD.Options and CoD.Options.Width or CoD.ButtonList.DefaultWidth)
    end
  end

  if menu.brightnessButton then
    menu.brightnessButton:processEvent({name = "gain_focus", controller = controller})
  end
end

---------------------------------------------------------------------------------------------------------------------
-- Options, in the front end and in a match (ui/t6/options.lua, LUI.createMenu.OptionsMenu): GRAPHICS goes first. The
-- game adds its buttons in order without a priority (menu.graphicsSettingsButton comes after the controls and audio);
-- the list is a LUI.UIVerticalList, whose addElementToTop puts an element before the first child.
---------------------------------------------------------------------------------------------------------------------
local function PatchOptions(menu, controller)
  local graphics = menu.graphicsSettingsButton
  local list = menu.buttonList
  if not graphics or not list or not list.addElementToTop or list:getFirstChild() == graphics then
    return
  end
  -- Whichever button had the focus gives it to GRAPHICS, now the first one.
  local child = list:getFirstChild()
  while child do
    if child ~= graphics and child.isInFocus and child:isInFocus() then
      child:processEvent({name = "lose_focus", controller = controller})
    end
    child = child:getNextSibling()
  end
  list:removeElement(graphics)
  list:addElementToTop(graphics)
  graphics:processEvent({name = "gain_focus", controller = controller})
end

---------------------------------------------------------------------------------------------------------------------
-- The match HUD (ui_mp/t6/hud.lua, LUI.createMenu.HUD): a player's HUD pick keeps their HUD
-- menu transparent, the way the game's own HUD_Hide does (setAlpha(0)). The pause menu and Extras are menus of their
-- own, beside it, and stay visible. The HUD is made by CoD.Menu.NewFromState("HUD", ...) (ui/t6/codmenu.lua sets
-- menuName), so the changes wrap that; a match builds its HUD while the menu system is being rebuilt, before these
-- changes are back, so the HUD is also looked for a few levels down every root. The pick in Extras applies at once
-- (state.applyHud); Install (once a second) and a timer in the HUD keep it applied. The timer alone is not enough: a
-- LUI timer is an animation, and the HUD's stop while the game is paused, which it is while Extras is open. CHEATS
-- off keeps the pick (the menu shows it) but shows the HUD.
---------------------------------------------------------------------------------------------------------------------
local function HudHiddenPicked(controller)
  return state.zombies() and state.validController(controller)
    and UIExpression.DvarInt(nil, state.playerDvar("hud", controller)) == 1
    and UIExpression.DvarInt(nil, state.playerDvar("cheats", controller)) == 1
end

local function SetHudHidden(menu, hidden)
  if hidden then
    local controller = state.pageController(menu, nil)
    if controller ~= nil then
      state.hudWasHidden = state.hudWasHidden or {}
      state.hudWasHidden[controller] = true
    end
    menu:setAlpha(0)
    if not menu.bo2zmHidden then
      menu.bo2zmHidden = true
      Log("match HUD hidden")
    end
  elseif menu.bo2zmHidden then
    menu:setAlpha(1)
    menu.bo2zmHidden = nil
    Log("match HUD shown")
  end
end

local function PatchHud(menu, controller)
  if menu.bo2zmHud then
    SetHudHidden(menu, HudHiddenPicked(state.pageController(menu, nil)))
    return
  end
  menu.bo2zmHud = true
  Log("match HUD: hiding ready")
  menu:addElement(LUI.UITimer.new(250, "bo2zm_hud_check", false))
  menu:registerEventHandler("bo2zm_hud_check", Protect(function(self, event)
    -- NewFromState runs before HUD's constructor sets its owner. First-snapshot events can change that owner too.
    SetHudHidden(self, HudHiddenPicked(state.pageController(self, nil)))
  end))
  SetHudHidden(menu, HudHiddenPicked(state.pageController(menu, nil)))
end

-- Every open match HUD under element, up to depth levels down: patched, then hidden or shown.
local function ApplyHudUnder(element, depth, where, controller, hidden)
  local child = element:getFirstChild()
  while child do
    if child.menuName == "HUD" then
      local owner = state.pageController(child, nil)
      if owner ~= nil and (controller == nil or owner == controller) then
        if not child.bo2zmHud then
          Log("match HUD found under " .. where)
        end
        -- Targeted updates must not reconcile another owner's stale published pick.
        PatchHud(child)
        SetHudHidden(child, hidden == nil and HudHiddenPicked(owner) or hidden)
      end
    elseif depth > 1 then
      ApplyHudUnder(child, depth - 1, where, controller, hidden)
    end
    child = child:getNextSibling()
  end
end

local HudRoots = {"UIRoot0", "UIRoot1", "UIRoot2", "UIRoot3", "UIRootFull"}

local function ApplyHud(controller, hidden)
  local roots = LUI.roots
  if not roots then
    return
  end
  for _, name in ipairs(HudRoots) do
    local root = roots[name]
    if root then
      ApplyHudUnder(root, 3, name, controller, hidden)
    end
  end
end
state.applyHud = ApplyHud

local function WrapNewFromState(original)
  return function(name, ...)
    local menu = original(name, ...)
    if name == "HUD" and menu then
      Safely(PatchHud, menu)
    end
    return menu
  end
end

---------------------------------------------------------------------------------------------------------------------
-- Test runs (bo2zm_test_start): Zombies uses "<map> <start location> <game type> <mode group>";
-- Multiplayer uses "<map> <game type>", such as "mp_hijacked dm". Both enter the game's local lobby.
-- LOCAL opens the map globe (ui_mp/t6/mainmenu.lua, OpenLocalMatchLobby: SelectMapZM in Zombies); there the match is
-- picked the way the start location menu does it (ui_mp/t6/zombie/selectstartloczombie.lua, GameTypeSelected: the
-- game type, four dvars, xupdatepartystate, then the local lobby SplitscreenGameLobby), and in the lobby it starts
-- the way the lobby's start does (ui/t6/menus/privategamelobby.lua, StartGame: Engine.PartyHostToggleStart).
---------------------------------------------------------------------------------------------------------------------
-- Only these three are used further down (the top level's locals are limited: see the top of the file).
local PatchMainMenuForTests, TestPickMap, TestStartMatch
do
  local function TestSpec()
    local spec = UIExpression.DvarString(nil, "bo2zm_test_start_ui")
    if spec == nil or spec == "" then
      return nil
    end
    if not state.zombies() then
      local map, gametype = string.match(spec, "^(mp_%S+) (%S+)$")
      if not map then
        return nil  -- a Zombies test specification must not drive Multiplayer
      end
      return {spec = spec, map = map, gametype = gametype}
    end
    local map, location, gametype, group = string.match(spec, "^(%S+) (%S+) (%S+) (%S+)$")
    if not map then
      Log("test start: expected a map, start location, game type and mode group, not " .. spec)
      return nil
    end
    return {spec = spec, map = map, location = location, gametype = gametype, group = group}
  end

  local function SetTestMatch(controller, t)
    Engine.Exec(controller, "resetCustomGametype")
    Engine.SetGametype(t.gametype)
    Engine.SetDvar("ui_gametype", t.gametype)
    if t.group then
      Engine.SetDvar("ui_zm_gamemodegroup", t.group)
      Engine.SetDvar("ui_zm_mapstartlocation", t.location)
    end
    Engine.SetDvar("ui_mapname", t.map)
    Engine.Exec(controller, "xupdatepartystate")
  end

  -- After 1.5 s on menu (so it has settled), or delay_ms, f(menu, controller, t) once per run under the flag name.
  local function TestStep(menu, controller, flag, f, delay_ms)
    local t = TestSpec()
    if not t or state[flag] then
      return
    end
    state[flag] = true
    local event = "bo2zm_" .. flag
    menu:addElement(LUI.UITimer.new(delay_ms or 1500, event, true))
    menu:registerEventHandler(event, Protect(function(self, e)
      if state[flag .. "Done"] then
        return
      end
      state[flag .. "Done"] = true
      f(self, controller, t)
    end))
  end

  -- The home menu sends itself LOCAL's action event, as pressing the button does: with bo2zm_test_start a test needs
  -- no button at all (the console's test toml uses it).
  local function TestOpenSolo(menu, controller)
    TestStep(menu, controller, "testSolo", function(self, c, t)
      local requested = tonumber(UIExpression.DvarString(nil, "bo2zm_test_start_controller"))
      if requested and requested >= 0 and requested <= 3 and requested == math.floor(requested) then
        c = requested
      end
      self:processEvent({name = "open_local_match_lobby", controller = c})
      Log("test start: local lobby opened by controller " .. tostring(c))
    end)
  end

  function PatchMainMenuForTests(menu, controller)
    PatchMainMenu(menu, controller)
    TestOpenSolo(menu, controller)
  end

  function TestPickMap(menu, controller)
    TestStep(menu, controller, "testPicked", function(self, c, t)
      SetTestMatch(c, t)
      local lobby = self:openMenu("SplitscreenGameLobby", c)
      if lobby then
        lobby:setPreviousMenu("MainMenu")
      end
      self:close()
      Log("test start: " .. t.spec .. " picked")
    end)
  end

  -- bo2zm_test_start_wait_s: the lobby waits for test pad players to join split screen.
  local function TestStartWaitMs()
    local seconds = tonumber(UIExpression.DvarString(nil, "bo2zm_test_start_wait_ui"))
    if seconds and seconds > 0 then
      return math.floor(seconds * 1000)
    end
    return nil
  end

  function TestStartMatch(menu, controller)
    ClearStartMatchHint(menu.buttonPane)
    PatchBotsLobby(menu, controller)
    -- Automated menu checks can stop at the selected lobby before starting play.
    if UIExpression.DvarInt(nil, "bo2zm_test_lobby_only") == 1 then return end
    TestStep(menu, controller, "testStarted", function(self, c, t)
      SetTestMatch(c, t)
      Engine.PartyHostToggleStart()
      Log("test start: " .. t.spec .. " started")
    end, TestStartWaitMs())
  end
end

---------------------------------------------------------------------------------------------------------------------
-- LOADING SCREEN VIDEO (Extras; the setting loading_video, shown in bo2zm_loading_video_ui). The loading screen
-- (ui_mp/t6/hud/loading.lua in patch_zm.ff, LUI.createMenu.Loading) plays the map's video only in Zombies, outside
-- Theater, offline or with one player (party_maxplayers), on Die Rise, Mob of the Dead, Buried or Origins, in a
-- classic match (CoD.Zombie.GAMETYPE_ZCLASSIC); then it sets ui_zm_useloadingmovie to 1 and shows SKIP. Anywhere else
-- it shows the map's image (loadscreen_<map>_<game type>_<start location>) and sets ui_zm_useloadingmovie to 0, which
-- keeps the program from starting the video (the level load plays "<map>_load" only while that dvar is 1; no
-- addresses here, since tools/direct_calls.py would count them as hooks). With the
-- setting off, that game type matches nothing while the menu is built, so it takes the image branch, as an online
-- match does. Only an explicit 0 turns the video off (the dvar is empty before the program first sets it).
---------------------------------------------------------------------------------------------------------------------
local function WrapLoading(original)
  return function(controller, ...)
    -- Map profiles (bo2zm_preset.cpp): which location is loading, so the program uses its profile.
    Safely(function()
      Engine.ExecNow(controller or 0, "bo2zm_profile_map " .. UIExpression.DvarString(nil, "ui_mapname") .. " "
        .. UIExpression.DvarString(nil, "ui_gametype") .. " "
        .. UIExpression.DvarString(nil, "ui_zm_mapstartlocation"))
    end)
    local CoD = rawget(_G, "CoD")
    local zombie = CoD and CoD.Zombie
    if not zombie or UIExpression.DvarString(nil, "bo2zm_loading_video_ui") ~= "0" then
      return original(controller, ...)
    end
    local zclassic = zombie.GAMETYPE_ZCLASSIC
    zombie.GAMETYPE_ZCLASSIC = "bo2zm_no_loading_video"
    local ok, menu = pcall(original, controller, ...)
    zombie.GAMETYPE_ZCLASSIC = zclassic
    if not ok then
      error(menu, 0)
    end
    Log("loading screen: the map's image (LOADING SCREEN VIDEO off)")
    return menu
  end
end

---------------------------------------------------------------------------------------------------------------------
-- The campaign's EXTRAS (ui/t6/menus/campaignmenu.lua and ui/t6/hud/objectiveinfomenu.lua in its patch.ff):
--   - its home menu, LUI.createMenu.CampaignMenu (RESUME STORY, MISSION SELECT, NEW GAME, OPTIONS), keeps its items in
--     menu.carousel, a CoD.VerticalCarousel of CoD.NavButtons (on a console; the PC layout is a CoD.ButtonList of
--     nav buttons, the menu's only one). EXTRAS goes last, as a nav button of the same kind;
--   - its pause menu, LUI.createMenu.ObjectiveInfoMenu, adds each button with CoD.ObjectiveInfoMenu.AddButton(list,
--     label, event), with no priority: EXTRAS follows OPTIONS. Its pages go back to "ObjectiveInfoMenu" (CreatePage).
-- Only these three are used further down (the top level's locals are limited: see the top of the file).
local PatchCampaignMenu, PatchCampaignPause, WrapCampaignPauseAddButton
do
  local CampaignExtrasHint = "Your username, experimental features and debug options."

  local function OpenExtrasOn(menu)
    menu:registerEventHandler("bo2zm_open_extras", Protect(function(self, event)
      OpenPage(self, event.controller, "main")
    end))
  end

  function PatchCampaignMenu(menu, controller)
    if menu.bo2zmExtras then
      return
    end
    if menu.carousel and CoD.NavButton then
      local button = CoD.NavButton.new(nil, "EXTRAS", "bo2zm_open_extras")
      button:setupCarouselMode()
      menu.carousel:addItem(button)
      menu.bo2zmExtras = button
    else
      local child = menu:getFirstChild()
      while child do
        if child.addNavButton then
          menu.bo2zmExtras = child:addNavButton("EXTRAS", "bo2zm_open_extras")
          break
        end
        child = child:getNextSibling()
      end
    end
    if menu.bo2zmExtras then
      menu.bo2zmExtras.hintText = CampaignExtrasHint
      OpenExtrasOn(menu)
    end
  end

  function PatchCampaignPause(menu, controller)
    OpenExtrasOn(menu)
  end

  -- The campaign's first menu, LUI.createMenu.MainMenu (ui/t6/mainmenu.lua in its patch.ff), switches mode: its nav
  -- buttons MULTIPLAYER, SINGLEPLAYER and ZOMBIES (events open_multiplayer, open_campaign, open_zombies) are the items
  -- of a CoD.VerticalCarousel on a console, which then starts on item 2, or the buttons of a ButtonList on the PC
  -- layout. Buttons for modes that are not installed are left out: the carousel's AddItem (each carousel takes it
  -- from CoD.VerticalCarousel when it is made) skips them while the menu is built, the start item moves with the kept
  -- ones, and getCurrentItemIndex still answers the game's own numbering (SetQuickLaunchMode reads it).
  local CampaignHomeModes = {open_multiplayer = "mp", open_zombies = "zm"}
  local function Dropped(button)
    local mode = type(button) == "table" and CampaignHomeModes[button.actionEventName]
    return mode and not state.modeInstalled(mode)
  end
  local function FixCampaignHome(menu, positions, seen)
    local carousel = menu.carousel
    if carousel and #positions < seen and type(carousel.getCurrentItemIndex) == "function" then
      local current = carousel.getCurrentItemIndex
      carousel.getCurrentItemIndex = function(self, ...)
        local index = current(self, ...)
        return positions[index] or index
      end
    end
    local child = menu:getFirstChild()
    while child do
      if child.addNavButton then
        local button = child:getFirstChild()
        while button do
          local nextButton = button:getNextSibling()
          if Dropped(button) then button:close() end
          button = nextButton
        end
      end
      child = child:getNextSibling()
    end
  end
  state.wrapCampaignHome = function(original)
    return function(controller, ...)
      local carouselClass = CoD.VerticalCarousel
      local common = CoD.CarouselCommon
      if not (carouselClass and common and type(carouselClass.AddItem) == "function"
              and type(common.SetStartItem) == "function") then
        local menu = original(controller, ...)
        if menu then Safely(FixCampaignHome, menu, {}, 0) end
        return menu
      end
      local add, start = carouselClass.AddItem, common.SetStartItem
      local positions, seen = {}, 0
      carouselClass.AddItem = function(self, item, ...)
        seen = seen + 1
        if Dropped(item) then return nil end
        table.insert(positions, seen)
        return add(self, item, ...)
      end
      common.SetStartItem = function(self, index, ...)
        for kept, position in ipairs(positions) do
          if position == index then
            index = kept
            break
          end
        end
        return start(self, index, ...)
      end
      local ok, menu = pcall(original, controller, ...)
      carouselClass.AddItem, common.SetStartItem = add, start
      if not ok then error(menu, 0) end
      if menu then Safely(FixCampaignHome, menu, positions, seen) end
      if menu and state.directCampaign() then
        -- No press-start screen to go back to.
        menu:registerEventHandler("button_prompt_back", function() end)
      end
      return menu
    end
  end

  -- Starting on the campaign's page (bo2cp_direct_campaign: the program sets bo2zm_cp_direct_ui to 1). The front end
  -- opens on its press-start screen, LUI.createMenu.LockOutMenu (ui/t6/menus/lockoutmenu.lua). START there
  -- (open_main_menu, CoD.LockOut.OpenMainMenu) makes that controller the primary one and selects the save device;
  -- when the device is chosen (save_device_selected, CoD.LockOut.FinishOpenMainMenu) it turns the other clients off,
  -- tells the level "deactivated", which plays the glasses intro, and opens the mode list. Here START is pressed
  -- for the primary controller as soon as the screen is up, and the device's answer instead takes the way the game
  -- itself takes when it comes back from a mission with ui_skipMainLockout set (ui/t6/main.lua): the level is told
  -- "skipped", so the camera is at the menu without the intro, and the landing menu is the campaign page. Back on
  -- the campaign page leads to the mode list as before; Back on the mode list no longer returns to press start.
  state.directCampaign = function()
    return UIExpression.DvarInt(nil, "bo2zm_cp_direct_ui") == 1
  end
  local function DirectFinish(menu, event)
    local controller = event.controller or UIExpression.GetPrimaryController()
    Engine.ExecNow(controller, "disableallbutprimaryclients")
    Engine.SetDvar("ui_skipMainLockout", 1)
    if CoD.ShadesHud then
      CoD.ShadesHud.bootupComplete = true
    end
    CoD.Menu.targetLandingMenu = "CampaignMenu"
    Engine.SendMenuResponse(controller, "lockout", "skipped")
    Engine.SetDvar("ui_onLockOutMenu", 0)
    local parent = menu:getParent()
    local campaign = LUI.createMenu.CampaignMenu(controller)
    if parent and campaign then
      parent:addElement(campaign)
    end
    menu:close()
    Log("campaign: started on the campaign page (controller " .. tostring(controller) .. ")")
  end
  -- The front end's root (LUI.createMenu.main, ui/t6/main.lua) holds every front-end menu. With the direct start it
  -- also gets a black veil over everything, from when the front end opens (or comes back from a mission) until the
  -- program keeps the settled scene (bo2zm_cp_backdrop_ui, set from the renderer), so the landing's glasses boot-up
  -- and camera settling look like the game still loading. Meanwhile the game gets no controller input
  -- (bo2zm_input_block). Then the veil fades out and the menu and its backdrop appear together, and input comes back.
  -- Ten seconds at most.
  -- What the veil shows: the game's own loading spinner (lui_loader, the one its popups use) above the phrase
  -- "Campaign Scene Loading" in the title font, with orange dots that count up like the game's "searching" text.
  -- The dots have their own box after the phrase, so the phrase never moves. The children fade with the veil.
  state.loadingScreen = function(veil)
    local fonts = CoD.fonts or {}
    local font = fonts.Big or fonts.Condensed or fonts.Default
    local height = 40
    local phrase = "Campaign Scene Loading"
    local measure = rawget(_G, "GetTextDimensions")
    local function Width(text, guess)
      if measure then
        local ok, left, _, right = pcall(measure, text, font, height)
        if ok and type(left) == "number" and type(right) == "number" and right > left then
          return right - left
        end
      end
      return guess
    end
    local phraseWidth = Width(phrase, height * 0.5 * #phrase)
    local dotsWidth = Width("...", height * 0.6) + 4
    local left = -math.floor((phraseWidth + dotsWidth) / 2)
    local white = CoD.offWhite or {r = 0.9, g = 0.9, b = 0.9}
    local orange = CoD.BOIIOrange or {r = 1, g = 0.5, b = 0.1}
    local content = LUI.UIElement.new()
    content:setLeftRight(true, true, 0, 0)
    content:setTopBottom(true, true, 0, 0)
    content:setAlpha(0)
    local spinner = LUI.UIImage.new()
    spinner:setLeftRight(false, false, -40, 40)
    spinner:setTopBottom(false, false, -100, -20)
    spinner:setImage(RegisterMaterial("lui_loader"))
    content:addElement(spinner)
    local text = LUI.UIText.new()
    text:setLeftRight(false, false, left, left + phraseWidth)
    text:setTopBottom(false, false, 8, 8 + height)
    text:setFont(font)
    text:setAlignment(LUI.Alignment.Left)
    text:setRGB(white.r, white.g, white.b)
    text:setText(phrase)
    content:addElement(text)
    local dots = LUI.UIText.new()
    dots:setLeftRight(false, false, left + phraseWidth, left + phraseWidth + dotsWidth)
    dots:setTopBottom(false, false, 8, 8 + height)
    dots:setFont(font)
    dots:setAlignment(LUI.Alignment.Left)
    dots:setRGB(orange.r, orange.g, orange.b)
    dots:setText("")
    dots.count = 0
    dots:registerEventHandler("bo2zm_loading_dots", function(self)
      self.count = (self.count + 1) % 4
      self:setText(string.rep(".", self.count))
    end)
    dots:addElement(LUI.UITimer.new(400, "bo2zm_loading_dots", false))
    content:addElement(dots)
    veil:addElement(content)
    content:beginAnimation("bo2zm_fade_in", 300)
    content:setAlpha(1)
  end
  state.wrapFrontEndRoot = function(original)
    return function(controller, ...)
      local root = original(controller, ...)
      if root and state.directCampaign() then
        Safely(function()
          -- A container (LUI draws no children of an image): the black image and the loading screen in it.
          local veil = LUI.UIElement.new()
          veil:setLeftRight(true, true, 0, 0)
          veil:setTopBottom(true, true, 0, 0)
          veil:setAlpha(1)
          local black = LUI.UIImage.new()
          black:setLeftRight(true, true, 0, 0)
          black:setTopBottom(true, true, 0, 0)
          black:setRGB(0, 0, 0)
          veil:addElement(black)
          veil:setPriority(10000)
          veil.ticks = 0
          Engine.ExecNow(controller or 0, "bo2zm_input_block 1")
          veil:registerEventHandler("bo2zm_backdrop_check", Protect(function(self)
            self.ticks = self.ticks + 1
            if not self.fading and (UIExpression.DvarInt(nil, "bo2zm_cp_backdrop_ui") == 1 or self.ticks > 100) then
              self.fading = true
              Engine.ExecNow(controller or 0, "bo2zm_input_block 0")
              self:beginAnimation("bo2zm_fade", 400)
              self:setAlpha(0)
              Log("front end: backdrop shown after " .. self.ticks * 100 .. " ms")
            end
          end))
          veil:registerEventHandler("transition_complete_bo2zm_fade", function(self)
            self:close()
          end)
          veil:addElement(LUI.UITimer.new(100, "bo2zm_backdrop_check", false))
          root:addElement(veil)
          Safely(function() state.loadingScreen(veil) end)
        end)
      end
      return root
    end
  end
  state.wrapLockOut = function(original)
    return function(controller, ...)
      local menu = original(controller, ...)
      if menu and state.directCampaign() then
        Safely(function()
          menu:registerEventHandler("save_device_selected", Protect(DirectFinish))
          menu:registerEventHandler("bo2zm_auto_start", Protect(function(self)
            self:processEvent({name = "open_main_menu", controller = UIExpression.GetPrimaryController()})
          end))
          menu:addElement(LUI.UITimer.new(100, "bo2zm_auto_start", true))
        end)
      end
      return menu
    end
  end

  function WrapCampaignPauseAddButton(original)
    return function(list, label, event, ...)
      local button = original(list, label, event, ...)
      if event == "open_options" then
        Safely(function()
          local extras = original(list, "EXTRAS", "bo2zm_open_extras")
          extras.hintText = CampaignExtrasHint
        end)
      end
      return button
    end
  end
end

---------------------------------------------------------------------------------------------------------------------
-- Installing: wraps each function that is not wrapped yet (or was replaced by the game since).
---------------------------------------------------------------------------------------------------------------------
-- Replaces tbl[key] with make(original), unless tbl[key] already is our wrapper.
-- The Local lobby (SplitscreenGameLobby in both modes: CoD.MainMenu.OpenLocalMatchLobby in ui_mp/t6/mainmenu.lua opens
-- it for Multiplayer, Zombies after its map) tells the program when it opens and closes: there a controller that has
-- not joined brings up the Switch's controller screen (bo2zm_controllers.cpp).
state.localLobby = function(menu, controller)
  Engine.ExecNow(controller, "bo2zm_local_lobby 1 " .. tostring(controller))
  local close = menu.close
  menu.close = function(self, ...)
    Safely(function()
      Engine.ExecNow(controller, "bo2zm_local_lobby 0")
    end)
    return close(self, ...)
  end
end

-- PrivateLocalGameLobby is the System Link lobby: Multiplayer opens it from SYSTEM LINK's create button
-- (CoD.MainMenu.OpenSystemLinkLobby), Zombies once the host has picked a map and start location
-- (CoD.SelectStartLocZombie in ui_mp/t6/zombie/selectstartloczombie.lua, when SessionMode_IsSystemlinkGame); a console
-- that joins from the server browser gets the same menu. Its other local players join as in the Local lobby: the game's
-- CoD.LobbyList.UnusedGamepadButton (ui/t6/lobbylist.lua) takes A from a controller no player uses and signs it in
-- ("signclientin"), in System Link as offline. Outside a lobby the program hides the other controllers' presses (the
-- front end is player 1's), so the System Link lobby announces itself like the Local one, on the host and on a console
-- that joined; on the Switch a new controller then brings up the controller screen there too. In Zombies its host also
-- gets BOT COUNT.
state.systemLink = function()
  if type(UIExpression.SessionMode_IsSystemlinkGame) == "function" then
    local ok, value = pcall(UIExpression.SessionMode_IsSystemlinkGame)
    return ok and value == 1
  end
  local CoD = rawget(_G, "CoD")
  if CoD and CoD.SESSIONMODE_SYSTEMLINK and type(Engine.SessionModeIsMode) == "function" then
    local ok, value = pcall(Engine.SessionModeIsMode, CoD.SESSIONMODE_SYSTEMLINK)
    return ok and value == true
  end
  return false
end
-- A System Link lobby takes as many players per console as the Local lobby (party_maxlocalplayers_local_splitscreen,
-- 4). The game allows two: its sign-in (signclientin) and the lobby list's join row and A press
-- (CoD.LobbyList.Update and UnusedGamepadButton) stop at party_maxlocalplayers, which the game sets from
-- party_maxlocalplayers_systemlink when the System Link lobby starts, and the Zombies start-location
-- menu sets that one to its game type's maxLocalPlayers (2) right after it opens this lobby. So the lobby raises both
-- dvars and its join list's own count (CoD.LobbyList.New keeps it) when it opens and twice a second. The total stays
-- the game's: the join row goes once the lobby holds party_maxplayers players.
state.systemLinkPlayers = function(menu)
  local most = UIExpression.DvarInt(nil, "party_maxlocalplayers_local_splitscreen")
  if type(most) ~= "number" or most < 2 then return end
  most = math.min(most, 4)
  for _, name in ipairs({"party_maxlocalplayers", "party_maxlocalplayers_systemlink"}) do
    if UIExpression.DvarInt(nil, name) ~= most then Engine.SetDvar(name, most) end
  end
  local lists = menu.lobbyPane and menu.lobbyPane.body and menu.lobbyPane.body.lobbyList
  for _, list in ipairs(lists and lists.playerLists or {}) do
    if list.addSplitscreenRow and list.maxLocalPlayers ~= most then list.maxLocalPlayers = most end
  end
end
state.systemLinkLobby = function(menu, controller)
  if not state.systemLink() then
    return state.localLobby(menu, controller)
  end
  Safely(state.localLobby, menu, controller)
  Safely(state.systemLinkPlayers, menu)
  menu:addElement(LUI.UITimer.new(500, "bo2zm_local_players", false))
  menu:registerEventHandler("bo2zm_local_players", Protect(state.systemLinkPlayers))
  ClearStartMatchHint(menu.buttonPane)
  PatchBotsLobby(menu, controller, true)
  Safely(state.onlineLobby, menu)
end

-- The Zombies home menu's background drifts: rock tiles (CoD.GameRockZombie, ui_mp/t6/zombie/gamerockzombie.lua) and
-- fog tiles (CoD.Fog, same file) move from node to node of their paths, each move one LUI animation that
-- RegisterMoveToNextState(element, reset, node, ...) starts ("move_to_next" over the node's forwardTime to its
-- position, or the position at once when reset), and each move's end starts the next one. Behind a popup the program
-- keeps the popup's first blurred image of the home menu (bo2zm_native_menu_blur_every), so the tiles would jump
-- ahead when the popup closes. While a popup covers the home menu (CoD.Menu.updateBlur with an occluded menu, outside
-- a match) each moving tile is held where it is, with its end-of-move handler set aside so the chain waits; when the
-- home menu is on top again the move finishes over the time it had left and the chain goes on from there.
state.home = {moves = {}, positions = {}}
state.homeClock = function()
  local ok, value = pcall(UIExpression.milliseconds, nil)
  if ok and type(value) == "number" then
    return value
  end
  return nil
end
state.homeNode = function(kind, node, front, frontB)
  local CoD = rawget(_G, "CoD")
  if kind == "fog" then
    return CoD.Fog.Path[node], CoD.Fog.TileWidth, nil
  end
  local G = CoD.GameRockZombie
  local path = front and (frontB and G.RockFrontBPath or G.RockFrontPath) or G.RockBackPath
  return path[node], G.RockTileWidth, G.RockTileHeight
end
state.homePlace = function(element, x, y, width, height)
  element:setLeftRight(false, false, x - width * 0.5, x + width * 0.5)
  if y and height then
    element:setTopBottom(false, false, y - height * 0.5, y + height * 0.5)
  end
end
state.wrapHomeMove = function(kind)
  return function(original)
    return function(element, reset, node, front, frontB, ...)
      Safely(function()
        local target, width, height = state.homeNode(kind, node, front, frontB)
        if not target or not element then
          return
        end
        local from = state.home.positions[element]
        local now = state.homeClock()
        state.home.positions[element] = {x = target.x, y = target.y}
        if reset or not from or not now or type(target.forwardTime) ~= "number" then
          state.home.moves[element] = nil
          return
        end
        state.home.moves[element] = {start = now, duration = target.forwardTime, from = from, to = target,
                                     width = width, height = height}
      end)
      return original(element, reset, node, front, frontB, ...)
    end
  end
end
-- Reset(element, node) puts a tile at a node at once (when the background starts or a map is picked).
state.wrapHomeReset = function(original)
  return function(element, node, ...)
    Safely(function()
      if element and type(node) == "table" then
        state.home.positions[element] = {x = node.x, y = node.y}
        state.home.moves[element] = nil
      end
    end)
    return original(element, node, ...)
  end
end
state.homeHold = function(hold)
  if UIExpression.IsInGame() == 1 then
    return
  end
  local now = state.homeClock()
  if hold and not state.home.held and now then
    local held = 0
    for element, move in pairs(state.home.moves) do
      local done = math.min(math.max((now - move.start) / math.max(move.duration, 1), 0), 1)
      if done < 1 then
        move.left = move.duration * (1 - done)
        move.handler = element.m_eventHandlers and element.m_eventHandlers.transition_complete_move_to_next
        if move.handler then
          element.m_eventHandlers.transition_complete_move_to_next = nil
        end
        local x = move.from.x + (move.to.x - move.from.x) * done
        local y = move.from.y and move.to.y and move.from.y + (move.to.y - move.from.y) * done
        element:beginAnimation("bo2zm_hold", 1)
        state.homePlace(element, x, y, move.width, move.height)
        move.held = {x = x, y = y}
        held = held + 1
      end
    end
    state.home.held = true
    Log("home background: " .. held .. " moving tiles held behind the popup")
  elseif not hold and state.home.held then
    state.home.held = nil
    local resumed = 0
    for element, move in pairs(state.home.moves) do
      if move.held then
        if move.handler then
          element.m_eventHandlers.transition_complete_move_to_next = move.handler
          move.handler = nil
        end
        move.from = move.held
        move.start = now or move.start
        move.duration = move.left
        element:beginAnimation("move_to_next", math.max(move.left, 1))
        state.homePlace(element, move.to.x, move.to.y, move.width, move.height)
        move.held = nil
        resumed = resumed + 1
      end
    end
    Log("home background: " .. resumed .. " tiles moving again")
  end
end
state.wrapUpdateBlur = function(original)
  return function(menu, ...)
    local result = original(menu, ...)
    Safely(state.homeHold, menu.occludedMenu ~= nil)
    return result
  end
end

local function WrapField(tbl, key, make)
  if type(tbl) ~= "table" or type(tbl[key]) ~= "function" then
    return
  end
  local mine = state.wrappers[tbl]
  if mine and mine[key] == tbl[key] then
    return
  end
  local wrapper = make(tbl[key])
  tbl[key] = wrapper
  if not mine then
    mine = {}
    state.wrappers[tbl] = mine
  end
  mine[key] = wrapper
end

-- Split screen: the game's Zombies HUD leaves out the two message windows (HUD_FirstSnapshot_Zombie skips them when
-- Engine.IsSplitscreen()), so iprintln and iprintlnbold, a script mod's toggle messages among them, show in no view.
-- Each view gets them back, where a single player has them (the same rectangles, in the view's own units).
state.wrapFirstSnapshot = function(original)
  return function(hud, ...)
    local result = original(hud, ...)
    local CoD = rawget(_G, "CoD")
    if hud and hud.safeArea and CoD and CoD.GameMessages and Engine.IsSplitscreen() then
      Safely(function()
        CoD.GameMessages.AddObituaryWindow(hud, {leftAnchor = true, rightAnchor = false, left = 13, right = 277,
          topAnchor = false, bottomAnchor = true, top = -320, bottom = -220})
        CoD.GameMessages.BoldGameMessagesWindow(hud, {leftAnchor = false, rightAnchor = false, left = 0, right = 0,
          topAnchor = true, bottomAnchor = false, top = 50, bottom = 70})
        Log("split-screen HUD: message windows added")
      end)
    end
    return result
  end
end

local function WrapMenu(patch)
  return function(original)
    return function(controller, ...)
      local menu = original(controller, ...)
      if menu then
        Safely(patch, menu, controller)
      end
      return menu
    end
  end
end

-- Offline class personalization uses only camos whose assets the program has checked.
-- Keep ownership queries intact; these wrappers change the local grid's presentation.
-- Build the table and closures in their own frame; HavokScript has little temporary-register headroom here.
(function()
  if state.campaign() then return end
  local camo = {
    -- Keys are attachment option IDs stored by SetClassItem, not camoNum ordinals or MTX package numbers.
    -- false marks Diamond; true marks an MTX camo with purchase presentation to remove locally.
    allowed = {[16] = false, [45] = true, [46] = true, [47] = true, [48] = true, [49] = true,
      [50] = true, [51] = true, [52] = true, [53] = true, [54] = true, [66] = true, [67] = true,
      [68] = true, [69] = true, [74] = true, [75] = true, [76] = true, [77] = true, [78] = true,
      [79] = true, [91] = true, [92] = true, [93] = true, [94] = true, [96] = true},
  }

  function camo.enabled()
    local cod = rawget(_G, "CoD")
    return cod ~= nil and cod.isMultiplayer == true and cod.isSinglePlayer == false and cod.isZombie == false
      and UIExpression.DvarInt(nil, "bo2mp_local_camos_ui") == 1
      and type(Engine.SessionModeIsMode) == "function" and type(Engine.GameModeIsMode) == "function"
      and cod.SESSIONMODE_OFFLINE == 0 and cod.GAMEMODE_LOCAL_SPLITSCREEN == 2
      and Engine.SessionModeIsMode(cod.SESSIONMODE_OFFLINE)
      and Engine.GameModeIsMode(cod.GAMEMODE_LOCAL_SPLITSCREEN)
  end

  function camo.wrapHidden(original)
    return function(option, ...)
      if camo.enabled() and camo.allowed[option] ~= nil then return false end
      return original(option, ...)
    end
  end

  function camo.wrapDisplay(original)
    return function(controller, option, ...)
      if camo.enabled() and camo.allowed[option] == true then return true end
      return original(controller, option, ...)
    end
  end

  -- Undo only our previous presentation before the stock refresh, including when the gate closes.
  function camo.restore(button, controller)
    local saved = button.bo2mpLocalCamo
    if not saved then return end
    button.bo2mpLocalCamo = nil
    button:setLocked(saved.locked, controller)
    if saved.mtxPurchased ~= nil then
      CoD.ContentGridButton.SetMTXPurchased(button, saved.mtxPurchased, controller)
    end
    button:setInactive(saved.inactive)
    button:setHintText(saved.hintText, saved.hintImage)
    button:setActionPromptString(saved.prompt)
    button.overrideLock, button.customEvent = saved.overrideLock, saved.customEvent
    button.showProgressBar = saved.progress
  end

  function camo.wrapUpdate(original)
    return function(button, event, ...)
      local controller = event.controller
      camo.restore(button, controller)
      local result = original(button, event, ...)
      local mtx = camo.allowed[button.weaponOption]
      if camo.enabled() and mtx ~= nil and not event.challengeMenu then
        button.bo2mpLocalCamo = {locked = button.lockImage ~= nil, inactive = button.inactive,
          hintText = button.hintText, hintImage = button.hintImage, prompt = button.actionPromptString,
          overrideLock = button.overrideLock, customEvent = button.customEvent, progress = button.showProgressBar}
        if mtx then
          button.bo2mpLocalCamo.mtxPurchased = button.mtxImage == nil
          CoD.ContentGridButton.SetMTXPurchased(button, true, controller)
        end
        button:setLocked(false, controller)
        button:setInactive(false)
        button:setHintText("")
        button:setActionPromptString(Engine.Localize("MENU_SELECT"))
        button.overrideLock, button.customEvent = nil, nil
        button.showProgressBar = false
      end
      return result
    end
  end

  function camo.wrapSelected(original)
    return function(container, event, ...)
      local button = event.button
      if button and button.bo2mpLocalCamo and not camo.enabled() then
        camo.restore(button, event.controller)
        return
      end
      return original(container, event, ...)
    end
  end

  -- The grid and its container save functions as event handlers. Replace only exact stock references.
  function camo.rebind(element, depth, original, wrapper)
    for name, handler in pairs(element.m_eventHandlers or {}) do
      if handler == original then element:registerEventHandler(name, wrapper) end
    end
    if depth == 0 or type(element.getFirstChild) ~= "function" then return end
    local child = element:getFirstChild()
    while child do
      camo.rebind(child, depth - 1, original, wrapper)
      child = child:getNextSibling()
    end
  end

  function camo.rebindRoots(original, wrapper)
    if type(original) == "function" and original ~= wrapper then
      for _, root in pairs(LUI.roots or {}) do camo.rebind(root, 12, original, wrapper) end
    end
  end

  state.installLocalCamos = function()
    local cod = rawget(_G, "CoD")
    if not cod then return end
    WrapField(cod.CACCamo, "IsCamoHidden", camo.wrapHidden)
    WrapField(cod.CACUtility, "ShouldDisplayWeaponOptionMTX", camo.wrapDisplay)
    if cod.CACCamo then
      local original = cod.CACCamo.CamoButtonSelected
      WrapField(cod.CACCamo, "CamoButtonSelected", camo.wrapSelected)
      camo.rebindRoots(original, cod.CACCamo.CamoButtonSelected)
    end
    local grid = cod.CamoGridButton
    if not grid then return end
    local original = grid.UpdateClass
    WrapField(grid, "UpdateClass", camo.wrapUpdate)
    camo.rebindRoots(original, grid.UpdateClass)
  end
end)()

local function Install()
  local createMenu = LUI.createMenu
  if state.installLocalCamos then state.installLocalCamos() end
  if state.campaign() then
    WrapField(createMenu, "GraphicsSettings", WrapMenu(PatchGraphicsSettings))
    WrapField(createMenu, "OptionsMenu", WrapMenu(PatchOptions))
    WrapField(createMenu, "CampaignMenu", WrapMenu(PatchCampaignMenu))
    WrapField(createMenu, "MainMenu", state.wrapCampaignHome)
    WrapField(createMenu, "LockOutMenu", state.wrapLockOut)
    WrapField(createMenu, "main", state.wrapFrontEndRoot)
    WrapField(createMenu, "ObjectiveInfoMenu", WrapMenu(PatchCampaignPause))
    for _, entry in ipairs(state.pageBuilders) do
      if createMenu[entry[1]] ~= entry[2] then
        createMenu[entry[1]] = entry[2]
      end
    end
    local CoD = rawget(_G, "CoD")
    if CoD then
      WrapField(CoD.ObjectiveInfoMenu, "AddButton", WrapCampaignPauseAddButton)
    end
    return
  end
  WrapField(createMenu, "MainMenu", WrapMenu(PatchMainMenuForTests))
  WrapField(createMenu, "GraphicsSettings", WrapMenu(PatchGraphicsSettings))
  WrapField(createMenu, "OptionsMenu", WrapMenu(PatchOptions))
  if state.zombies() then
    WrapField(createMenu, "HUD", WrapMenu(PatchHud))
    WrapField(_G, "HUD_FirstSnapshot_Zombie", state.wrapFirstSnapshot)
    WrapField(createMenu, "SelectMapZM", WrapMenu(TestPickMap))
    WrapField(createMenu, "Loading", WrapLoading)
  end
  WrapField(createMenu, "SplitscreenGameLobby", WrapMenu(function(menu, controller)
    Safely(state.localLobby, menu, controller)
    TestStartMatch(menu, controller)
  end))
  WrapField(createMenu, "PrivateLocalGameLobby", function(original)
    return state.onlineTitled(WrapMenu(state.systemLinkLobby)(original))
  end)
  WrapField(createMenu, "ServerBrowser", function(original)
    return state.onlineTitled(WrapMenu(state.onlineBrowser)(original))
  end)
  for _, entry in ipairs(state.pageBuilders) do
    if createMenu[entry[1]] ~= entry[2] then
      createMenu[entry[1]] = entry[2]
    end
  end
  -- The HUD tree is walked only while the HUD is hidden, and once when it is shown again; new HUDs are patched as
  -- they are made (WrapNewFromState).
  if state.zombies() then
    state.hudWasHidden = state.hudWasHidden or {}
    for controller = 0, 3 do
      local hidden = HudHiddenPicked(controller)
      if hidden or state.hudWasHidden[controller] then ApplyHud(controller, hidden) end
      state.hudWasHidden[controller] = hidden
    end
  end
  local CoD = rawget(_G, "CoD")
  if CoD then
    if state.zombies() then
      WrapField(CoD.Menu, "NewFromState", WrapNewFromState)
      WrapField(CoD.Menu, "updateBlur", state.wrapUpdateBlur)
      WrapField(CoD.GameRockZombie, "RegisterMoveToNextState", state.wrapHomeMove("rock"))
      WrapField(CoD.Fog, "RegisterMoveToNextState", state.wrapHomeMove("fog"))
      WrapField(CoD.GameRockZombie, "Reset", state.wrapHomeReset)
      WrapField(CoD.Fog, "Reset", state.wrapHomeReset)
    end
    WrapField(CoD.Class, "AddButton", WrapPauseAddButton)
    state.installHostPause(WrapField, createMenu, CoD)
    WrapField(CoD.PrivateGameLobby, "PopulateButtons", WrapLobbyPopulate)
    WrapField(CoD.PrivateLocalGameLobby, "PopulateButtons", WrapLobbyPopulate)
    -- Change Map creates playlist buttons when a category opens, after the menu itself was built.
    -- Their lobby refresh also calls enable for maps without a playlist lock index.
    WrapField(CoD.PlaylistButton, "new", WrapSteadyButtonNew)
  end
end

-- For the program's once-a-second check (bo2zm_ui.cpp).
state.install = function()
  Safely(Install)
end

Safely(Install)
local originalRequire = rawget(_G, "require")
rawset(_G, "require", function(...)
  local a, b, c = originalRequire(...)
  Safely(Install)
  return a, b, c
end)
local pages = 0
for _, entry in ipairs(state.pageBuilders) do
  if LUI.createMenu[entry[1]] == entry[2] then
    pages = pages + 1
  end
end
Log("menus.lua installed, " .. pages .. " of " .. #state.pageBuilders .. " menus (the Extras pages, CHECK FOR UPDATES)")
return true
