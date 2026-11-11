# Menus and settings

Every page and option the port adds, with its choices. Choices are remembered in `bo2zm_settings.txt` in the game's
`user` folder. A value set in `bo2zm.toml` (below) wins over the remembered one at every start; the Script Loader's
saved selection wins over its initial TOML value.

## Home menu

| Item | What it does |
|---|---|
| **LOCAL** | An offline match: the Zombies map globe, or the Multiplayer local lobby. Up to four players in split screen |
| **ZOMBIES MULTIPLAYER** / **MULTIPLAYER** | Internet play with a join code ([below](#internet-play)) |
| **SYSTEM LINK** | The game's own LAN play, between Switches and PCs on the same network |
| **OPTIONS** | The game's options, with the Graphics page below |
| **EXTRAS** | The Extras pages below |

Back on the home menu switches to the other modes when they are installed. Campaign is its own program (`bo2cp.nro`
beside `bo2zm.nro` on the Switch, its own `.exe` on Windows).

## Playing together

### Split screen

- Up to four local players in Zombies and Multiplayer, each with an offline profile of their own.
- The first controller drives the menus. In the Local lobby, a controller that has not joined yet brings up the
  Switch's controller screen; joining there adds the player. The screen's player 1 leads the lobby, players keep their
  places, and gaps close when someone leaves.
- Cheats, crosshairs and field of view are per player; the leader alone pauses the match.
- Script mod menus are drawn inside each player's view, and every local player gets the host's menu.

### System Link

The game's own LAN play. Hosts are found by broadcast, so every console and PC must be on the same network. On
Windows, allow the game through the firewall for inbound UDP on ports 31000-31001. A Zombies host gets **BOT COUNT**;
players who join keep their own cheats, which the host applies to them.

### Internet play

**ZOMBIES MULTIPLAYER** (or **MULTIPLAYER**) on the home menu opens the online page:

| Item | What it does |
|---|---|
| **HOST ONLINE GAME** | Prepares the connection and shows an 8-character join code (`XXXX-XXXX`) to share |
| **CONTINUE TO LOBBY** | Opens your game's lobby; players join it with your code |
| **JOIN WITH CODE** | Type the code the host's screen shows |
| **FIND GAME** | Looks for the host's game; the host must be in its lobby |

The page lists each step as it happens. The game first asks the router to open its ports (UPnP). Many routers accept
the request and forward nothing, so both sides also meet through free public MQTT servers, which only pass the two
addresses along, and open a path through both routers by sending each other packets (hole punching). The game itself
then goes straight between the players. If neither works, the host forwards UDP ports 31000-31002 to their console or
PC by hand. Switch and Windows players can play together.

### Zombies bots

**BOT COUNT** in a Zombies Local or System Link lobby adds bots: OFF, 1, 2 or 3. Bots and players share four slots, so
the choice shrinks as players join, and every new lobby starts at OFF. The bots run on the host and need the bot pack
(`bo2zm_bots.gscpack`) beside the game; without it the row shows **NOT INSTALLED**. They work alongside a script mod
selected in the Script Loader ([script mods](script-mods.md#zombies-bots)).

## Options > Graphics

| Option | Choices | What it does |
|---|---|---|
| **MAP PROFILE** (menus) | GLOBAL, TOWN, FARM, BUS DEPOT, TRANZIT, DINER, NUKETOWN, DIE RISE, MOB OF THE DEAD, BURIED, ORIGINS | Which profile the settings below show and change |
| **ENABLE PROFILE** | OFF, ON | ON: that map keeps its own settings (starting as a copy of GLOBAL) |
| **PROFILE IN USE** (in a match) | — | The profile this map is using |
| **GRAPHICS PRESET** | LOWEST, VERY LOW, LOW, MEDIUM, HIGH, CUSTOM | Sets every option below; changing one makes it CUSTOM. HIGH is the Mac default; VERY LOW is the Switch default |
| **SPLIT-SCREEN QUALITY** | SAME, LOWEST, VERY LOW, LOW, MEDIUM, HIGH | The highest preset split-screen views use; SAME applies no extra cap |
| **TEXTURE STREAMING** | 4 MB/S, 16 MB/S, UNLIMITED | The in-match texture read-rate limit |
| **SPLIT-SCREEN MODEL SCALE** | 1X, 1.5X, 2X, 3X, 4X | How much sooner models simplify in split screen; 1X matches one player's detail |
| **TEXTURE MEMORY** | 384 / 768 MB, 768 / 1536 MB, 1536 / 3072 MB, 3072 / 6144 MB | Soft and hard texture-cache limits, applied immediately |
| **TEXTURE DETAIL** | LOWEST, LOW, MEDIUM, HIGH | Texture sharpness |
| **MODEL DETAIL** | LOW, MEDIUM, HIGH, ULTRA | How soon models switch to simpler versions |
| **DRAW DISTANCE** | SHORTEST, SHORT, MEDIUM, FAR, FULL | How far the world is drawn |
| **FAR OBJECTS** | HIDE, SIMPLE | SIMPLE keeps big far shapes in their simplest form, so the sky never shows through Buried's caves |
| **SHADOWS** | OFF, LOW, MEDIUM, HIGH | Sun and spotlight shadows |
| **DYNAMIC LIGHTS** | OFF, LOW, MEDIUM, HIGH | Lights from muzzle flashes, explosions and effects |
| **SCREEN EFFECTS** | OFF, ON | Depth of field, flames and water on screen, clouds |
| **DECALS** | OFF, NEAR, MEDIUM, FAR | Blood, scorch and bullet marks |
| **CORPSES AND DEBRIS** | FEWEST, FEW, SOME, ALL | How many bodies and pieces stay around |
| **FPS CAP** | 30, 60 | The highest frame rate in a match |
| **VSYNC** | OFF, ON | Frames wait for the screen's refresh; with the cap at 30, a steady 30 FPS |
| **SHOW PRESET** | DISABLED, ENABLED | The preset and the CPU cores in use, top right |
| **FPS COUNTER** | OFF, ON | Frames per second, top left |

**Shortcut:** hold Minus and press Up or Down for the next higher or lower preset, in menus or in a match.

| Preset | Texture | Model | Distance | Far objects | Shadows | Lights | Effects | Decals | Corpses |
|---|---|---|---|---|---|---|---|---|---|
| LOWEST | LOWEST | LOW | SHORTEST | HIDE | OFF | OFF | OFF | OFF | FEWEST |
| VERY LOW | LOW | LOW | SHORT | HIDE | OFF | OFF | OFF | OFF | FEW |
| LOW | MEDIUM | MEDIUM | MEDIUM | HIDE | LOW | LOW | OFF | MEDIUM | SOME |
| MEDIUM | HIGH | HIGH | FAR | HIDE | MEDIUM | MEDIUM | ON | FAR | ALL |

HIGH is the Xbox 360's own settings. Map profiles apply to Zombies only; Multiplayer and Campaign show GLOBAL and
**ENABLE PROFILE: N/A**.

## Extras

Every mode's Extras starts with:

| Item | Choices | What it does |
|---|---|---|
| **SCRIPT LOADER** | page | Choose, check, load or unload a script mod ([script mods](script-mods.md)) |
| **DEFAULT GAMEMODE** | CAMPAIGN, MULTIPLAYER, ZOMBIES | The mode the game opens in (home menu only). Lists the installed modes; not shown when only one is installed |
| **LOADING SCREEN VIDEO** | DISABLED, ENABLED | Zombies: the DLC maps' loading videos, or their still image for a faster start |
| **EDIT USERNAME** | — | The name other players see (home menu only) |
| **EXPERIMENTAL** | page | The fourth CPU core |
| **DEBUG** | page | Bug reporting tools |

The cheat pages below are in Zombies. Items marked *match* appear only in a match. Every cheat acts only while
**CHEATS** is on; items marked *pref* are preferences and always apply. In split screen each player sets their own.

### Cheats

| Item | Choices | What it does |
|---|---|---|
| **CHEATS** | OFF, ON | Master switch; your picks are kept while it is off |
| **GOD MODE** | OFF, ON | No damage |
| **INFINITE AMMO** | OFF, ON | The clip stays full |
| **EXPLOSIVE BULLETS** | OFF, ON | Bullets explode on impact, never hurting you |
| **NO RECOIL** | OFF, ON | Shooting does not kick your aim |
| **GIVE MAX POINTS** | — | 1,000,000 points (*match*) |
| **GAME SPEED** | VERY SLOW, SLOW, NORMAL, FAST, VERY FAST | 0.25x to 2x for the whole match |
| **RESET CHEATS** | — | Every cheat back to normal play; the combo's settings stay |

### EXPERIMENTAL

| Item | Choices | What it does |
|---|---|---|
| **CPU CORES** | 3, 4 | 4 also uses the fourth core; needs a forwarder made in sphaira with CPU Cores: 4 |
| **IN USE** | — | The cores in use; SWITCHED OFF means the game went back to three because the system needed the core |

### MOVEMENT

| Item | Choices | What it does |
|---|---|---|
| **MOVE SPEED** | NORMAL, FAST, FASTER, FASTEST | Walking and running speed |
| **UNLIMITED SPRINT** | OFF, ON | Sprint without tiring |
| **JUMP HEIGHT** | NORMAL, HIGH, HIGHER, HIGHEST, INSANE, EXTREME | How high you jump (use god mode: landings hurt) |
| **FEATHER FALL** | OFF, ON | Fall slowly and steer in the air |
| **GRAVITY** | MOON, LOW, NORMAL, HIGH | Floatier or heavier jumps and falls |
| **NOCLIP** | OFF, ON | Fly with the stick, triggers for up and down |
| **UFO** | OFF, ON | Fly where the camera looks |
| **COLLISION** | DISABLED, ENABLED | Whether walls stop the fly modes (*pref*) |
| **COMBO** | DISABLED, ENABLED | Aim + D-pad Down toggles a fly mode (*pref*) |
| **COMBO SETS** | NOCLIP, UFO | Which fly mode the combo toggles (*pref*) |

### CAMERA

| Item | Choices | What it does |
|---|---|---|
| **THIRD PERSON** | OFF, ON | The game's own third-person camera |
| **TPP CROSSHAIR** | DISABLED, WHEN AIMING, ALWAYS | A crosshair in third person; aiming moves over the shoulder (*pref*) |
| **TPP CROSSHAIR TYPE** | DOT, SMALL CROSS | Its shape (*pref*) |
| **TPP CROSSHAIR COLOR** | WHITE, RED, GREEN, CYAN, YELLOW, MAGENTA, ORANGE | Its color (*pref*) |
| **FIELD OF VIEW** | 55 to 110 | In degrees; the game's is 65 |
| **GUN** | SHOWN, HIDDEN | Your gun and hands |
| **HUD** | SHOWN, HIDDEN | Points, ammo and round |

### ZOMBIES

| Item | Choices | What it does |
|---|---|---|
| **ZOMBIES IGNORE YOU** | DISABLED, ENABLED | Zombies leave you alone |
| **ZOMBIES FREEZE** | DISABLED, ENABLED | Every zombie stops |
| **STOP SPAWNS** | DISABLED, ENABLED | No new zombies; the round waits |
| **KILL ALL ZOMBIES** | — | Kills every zombie, credited to you (*match*) |
| **ROUND** | 1 to 255 | The next round is the one you pick (*match*) |

### POWER UPS (*match*)

Spawns a power-up in front of you. Only the map's own are listed:

| Power-up | Maps |
|---|---|
| NUKE, MAX AMMO, INSTA-KILL, DOUBLE POINTS | every map |
| CARPENTER | TranZit and its locations (not Grief), Die Rise, Buried |
| DEATH MACHINE | TranZit and its locations (not Grief) |
| FIRE SALE | Nuketown, Mob of the Dead, Buried, Origins |
| FREE PERK | Die Rise, Buried, Origins |
| RANDOM WEAPON | Buried |
| ZOMBIE BLOOD, BONUS POINTS | Origins |

### PERKS

| Item | Choices | What it does |
|---|---|---|
| **KEEP PERKS** | DISABLED, ENABLED | Keep your perks when you go down |
| **REMOVE PERKS** | — | Lose every perk (*match*) |
| **CROSS-MAP PERKS** | DISABLED, ENABLED | Also list other maps' perks, marked OTHER MAP (*pref*) |
| one row per perk | — | Get it (*match*); the map's own perks are listed |

On a map without its machine, a perk shows no HUD icon, and only part of it may work:

| Perk | On another map |
|---|---|
| Juggernog, Quick Revive, Speed Cola, Double Tap, Stamin-Up, Deadshot Daiquiri, Mule Kick | Works |
| PhD Flopper | In part: no explosion or fall damage, but no dive-to-prone blast |
| Tombstone, Who's Who, Electric Cherry, Vulture Aid | Does nothing |

### WEAPONS (*match*)

| Item | Choices | What it does |
|---|---|---|
| a grid of the map's guns | — | The gun replaces the one in your hands; LB and RB change pages |
| **PACK-A-PUNCH** | OFF, ON | Guns come Pack-a-Punched when they have an upgrade (*pref*) |
| **PACK-A-PUNCH CURRENT WEAPON** | — | Upgrades the gun in your hands |

Only the map's own guns can be given: the engine registers weapons when a map loads.

### DOORS + POWER (*match*)

| Item | What it does |
|---|---|
| **OPEN ALL DOORS** | Opens every door and bought debris for free (Mob of the Dead's Afterlife doors too) |
| **CLOSE ALL DOORS** | Closes the open doors, which can then be bought again; power doors stay open |
| **POWER ON** | Perk machines, Pack-a-Punch and electric doors; on Origins, captures every generator |
| **POWER OFF** | Machines and Pack-a-Punch stop until POWER ON; on Origins, every generator is lost |
| **REMOVE FREE DEBRIS** | Clears Buried's barricades that Leroy breaks |
| **PUT DEBRIS BACK** | Restores the barricades REMOVE FREE DEBRIS cleared |

Mob of the Dead has no power to switch.

### DEBUG

For reporting a bug. DEBUG is the master switch: the options below apply only while it is on.

| Item | Choices | What it does |
|---|---|---|
| **DEBUG** | OFF, ON | Turns the options below on |
| **DEBUG PRESET** | CUSTOM, BASIC, VERBOSE, GRAPHICS, PERFORMANCE, CRASH, FULL | A set of options for a kind of bug |
| **MARK THE LOG** | — | A marked log line and a screenshot |
| **TAKE SCREENSHOT** | — | Saved in `sdmc:/switch/bo2zm/captures` |
| **MARK LOCATION** | OFF, ON | Adds MARK LOCATION to the pause menu: saves your position, view, map, round and four screenshots |
| **BUILD AND LOG FILE** | — | This build and this run's log |
| **MAX LOG FILES** | 5, 10, 20, OFF | Logs kept, one per start (10 by default; OFF keeps all) |
| **PURGE DEBUG INFO** | page | Deletes LOGS, SCREENSHOTS, TRACES AND TEST FILES or ALL, after showing the count; settings and saves stay |
| **LOG DETAIL** | NORMAL, DETAILED | Adds the game's debug lines |
| **STACK PROFILE** | OFF, ON | Thread profile in `logs/rex/rex_profile.log` from the next start (costs frame rate) |
| **TEXTURE CHECKS** | FAST, FULL | FULL rechecks every texture, for flickering or stale textures |
| **SHADER CONSTANTS** | FAST, FULL | FULL copies every constant, for wrong colors or lighting |
| **GEOMETRY CACHES** | ON, OFF | OFF re-uploads geometry, for broken models |
| **SAFE RENDERER** | OFF, ON | The three shortcuts above off at once |
| **RESET DEBUG** | — | Every debug option back to BASIC |

With DEBUG off the log holds only warnings and errors.

## On screen

- **PREPARING SHADERS**: pipelines compiled before the game starts, only after an update or a driver change.
- **FAILSAFE: REVERTED TO 3 CORES**: the fourth core was switched off because the system needed it.

## On the Switch

- **Clocks**: the game never changes them; a tool such as sys-clk can. Without one, `bo2zm_switch_gpu_mhz = 460`
  requests the official 460.8 MHz handheld GPU profile.
- **Docked and handheld**: the game's 1280x720 image, scaled to the TV when docked.
- **Controllers**: face buttons by position, as on an Xbox controller, or by letter (`input_xbox_layout`).

## bo2zm.toml

The settings file next to the game (`sdmc:/switch/bo2zm/bo2zm.toml`). Remove the `#` in front of a setting to use
it. The same names work as `--name=value` on the PC.

| Setting | Default | What it does |
|---|---|---|
| `input_xbox_layout` | `true` | Face buttons by position, as on an Xbox controller; `false`: by letter |
| `bo2zm_preset` | `"very_low"` | The preset for every map at every start (map profiles are then ignored) |
| `bo2zm_preset_on_screen` | `true` | SHOW PRESET |
| `bo2zm_front_end_preset` | `"lowest"` (Switch), `"same"` (PC) | In `bo2cp.toml`: the highest preset the Campaign front end's 3D scene gets, with FAR OBJECTS on SIMPLE and every model kept; on the Switch the scene is also drawn without its depth prepass, and only when its view changes (the menus are drawn every frame); missions keep the chosen preset |
| `bo2zm_show_fps` | `false` | FPS COUNTER |
| `bo2zm_loading_video` | `true` | LOADING SCREEN VIDEO |
| `bo2zm_cpu_cores` | `3` | CPU CORES |
| `bo2zm_max_log_files` | `"10"` | MAX LOG FILES |
| `log_level` | follows DEBUG | `"info"` keeps the full log |
| `bo2zm_switch_gpu_mhz` | not set | `460` requests the official handheld GPU profile |
| `net_online_brokers` | four public servers | The relay servers for internet play (`host:port`, comma-separated); every player should use the same list |
| `bo2zm_mod_hud_fit` | `true` | Fits script mod menus into each split-screen view |

Leave the renderer, streaming and benchmark settings as shipped.
