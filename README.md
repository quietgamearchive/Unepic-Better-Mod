# Unepic Better Mod v1.0.1

A trainer panel for [Unepic](https://store.steampowered.com/app/233980/) on Steam.
It adds a native Windows control panel with cheats, an item catalogue, save-slot
managers, achievement tools and a few quality-of-life features.

* Built for **Unepic v1.51.1 (build 15101), 32-bit, Steam version**. The mod uses
  fixed addresses inside that executable, so other game builds will not work.
* **Single-player first; co-op support is tested but not exhaustive.** God/spike
  immunity, in-room teleport, Move speed, infinite jump, the MP saves window and the
  session-only MP map unlock were tested in host and join sessions. Other multiplayer
  modes and peer setups were not tested; use it in co-op at your own risk.
* No network access, no installer beyond one script, no run key, no service, nothing
  that starts at boot. Uninstalling restores the game completely.

Nexus Mods page: https://www.nexusmods.com/unepic/mods/2

## Screenshot

The whole panel, one window, always on top of the game. Press the backquote key to
show or hide it. (This capture has the item language set to Japanese.)

<img src="img/panel.png" alt="Unepic Better Mod panel: Display, Cheats, Values, Status, Saves and Items groups" width="380">

*The screenshot predates the `MP saves...`, `Unlock all MP maps` and `Light this room` buttons;
they are in the Saves and Cheats groups now.*

Groups from top to bottom: **Display** (aspect and window size, applied next launch),
**Cheats** (god mode, max XP, infinite items, infinite jump, the Buffs window,
reveal whole map, Light this room, Light all rooms, right-click teleport, move/attack speed 1x-10x, Unlock all
MP maps (session-only), Complete all challenges and Unlock all achievements), **Values** (skill
points, money, UN - each with Set, Max and its valid range), **Status** (live HP, level,
XP, points, money, UN), **Saves** (force save - single-player only, the single-player
save manager with in-place load, and the multiplayer save-file manager), **Items**
(language, category, search, quantity, the catalogue list, to bag / to ground, and the
description box below the selected row).

## Requirements

- **Windows 10 or 11.** Steam itself no longer supports Windows 7, 8 or 8.1, and the C
  runtime the mod links against (the Universal CRT) is part of Windows 10 / 11, so there
  is nothing else to install - no framework, no redistributable, no .NET.
- **Steam version of Unepic, v1.51.1 (build 15101).** Other builds and other stores are
  not supported; the mod reads fixed addresses in that executable.
- Steam running and the game launched through Steam - needed for the achievement
  functions to reach Steam.
- **Windowed mode** (or a desktop-sized borderless window) for the panel to stay visible
  and for the window-size settings; see Installation.
- Do not combine with another mod that replaces the same game file.
- No save backups are made by the mod. Copy a slot before experimenting, and consider
  turning Steam Cloud off so cloud sync cannot overwrite what you just changed.

## Contents

**In the release archive** (what you download): `README.md`, `img/panel.png` (the
screenshot above), and a `mod` folder holding `SDL2.dll`, `unepic_mod_core.dll`,
`items.tsv`, the `unepic_mod.ini` settings template, `dict\` with the 19 language name
tables and 19 description tables, and the two PowerShell scripts.

**In this repository**:

| file | what it is |
|---|---|
| `src/unepic_mod_core.c` | the trainer: hooks, panel, item catalogue, save-slot window, teleport, achievements (6,159 lines of C) |
| `src/unepic_loader.c` | the proxy `SDL2.dll` the game loads: forwards the 14 SDL2 functions, sizes the window before it is created, loads and hot-reloads the core (294 lines) |
| `scripts/install.ps1` | renames the game's `SDL2.dll` to `SDL2_orig.dll`, copies the loader in, copies the settings template |
| `scripts/uninstall.ps1` | restores the original file (checks the hash first so it never deletes somebody else's DLL) |
| `build.ps1` | builds both DLLs from `src/` with the Zig toolchain |
| `data/` | `items.tsv` + the 57 language tables the archive ships + the `unepic_mod.ini` template |
| `tools/build_release.py` | assembles the release archive; the file list in it is the definition of the package (`build` / `check` / `verify`) |

Everything is plain C (C17, clang/GNU dialect) against the Win32 API and the Universal
CRT - no C++, no libraries, no generated code. `data/` is the text the archive ships:
`items.tsv`, the 57 language tables (item names, descriptions and effect names, 19
languages each) and the settings template. The tables are dumped from the game's own
dictionaries and are what the Items and Buffs windows show when you switch language; they
can also be regenerated in game with the mod's own `[debug] dumpdict` switch.

## Installation

1. Close Unepic.
2. Extract the archive into the game folder - the one containing `unepic.exe`. You
   should end up with a `mod` folder next to the executable.
3. Run `mod\install.ps1` (right-click and choose **Run with PowerShell**; if script
   execution is blocked: `powershell -ExecutionPolicy Bypass -File .\mod\install.ps1`).
4. Start the game and press the **backquote key** (the grave accent / tilde key,
   normally left of the `1` key) to show or hide the panel. It only reacts while the
   game window is in the foreground.

The panel needs the game in **windowed mode** (or a desktop-sized borderless window):
it is an ordinary topmost window, and on an exclusive-fullscreen display it can end up
hidden behind the game. The mod never changes the game's own screen-mode setting.

Updating the mod later is a file copy: replace `mod\unepic_mod_core.dll` while the game
is running and it reloads in about a second. If Steam updates or verifies the game
files, the original `SDL2.dll` comes back and the mod stops loading - run
`install.ps1` again.

Uninstall: close the game, run `mod\uninstall.ps1`, then delete the `mod` folder and
`unepic_mod.ini` if you want. Saves and Steam achievements are not touched.

## What it does

**Cheats** - god mode; max XP (XP kept at the next-level requirement); infinite
consumables; no ammo or durability use for bows, crossbows and wands; infinite jump;
the Buffs window (one checkbox per status effect, positive and negative, with All/None
for each half); move speed and attack speed 1x to 10x; `Unlock all MP maps` (see the map
section below).

**Achievements** - `Complete all challenges` finishes the 15 challenges and pushes their
Steam achievements; `Unlock all achievements` unlocks all 57 (42 logros plus the 15
challenge achievements). Both ask for confirmation, and both call the game's own Steam
API rather than faking anything locally. Achievements cannot be taken back.

**Values** - set skill points (0-999), money (2-999999) and UN (0-999999), with a Max
button and the valid range printed next to each field. Live HP, level, XP, points,
money and UN read-outs.

**Saves** - `Force save` (single-player only) writes through the game's own serializer;
in multiplayer the button is disabled and the game's own save menu is used. `Saves...`
opens a window listing all 16 slots with date, size and the slot in use, copies a slot to
another one, deletes a slot to the Recycle Bin, loads a slot in place and opens the save
folder. `Load selected` asks for confirmation and then uses the game's own loader on the
next frame: no trip back to the main menu, but unsaved progress is lost. The mod does not
keep backups of its own; copy a slot if you want a safety copy.
`MP saves...` (right of `Saves...`) is the same window for the independent multiplayer
files (`game_mp_N.sav`): copy, delete and open the folder. It has no load button - the
game loads multiplayer saves itself from its own multiplayer menu, so this window only
manages the files.

**Items** - a catalogue of the game's ~1,600 items, filterable by the game's own
categories and by free text, quantity 1-999, send to bag or drop at the player's feet,
with names and descriptions in the game's 19 languages. About 40% of items have
description text in the game's dictionaries; ordinary weapons and armour only ever
show numbers there, so they display a placeholder.

**Buffs...** - opens a separate window with two halves. The upper half lists all 56
positive effects with one checkbox each; a checked effect is kept at its maximum
duration. The lower half lists the 30 negative effects (poison, corrosion, sleep,
paralysis, slow, stun, fear, blindness, burning, freeze, the mind effects...); a checked
one is removed every 200 ms. Each half has `All` / `None`. The `Language` combo switches
the labels to the game's own effect names in any of its 19 languages (initially the
item language, then whatever you picked last). The selection is stored in
`mod\buffs.ini` (section `positive` / `debuff`, key = effect id; `[ui] lang` remembers
the language) and applied at startup. Defaults: all positive effects on, all negative
off. Effects with a number (spirit arrows, poisoned arrows, the six weapon masteries,
critical strikes, protections, strength, regeneration...) get the strongest magnitude
the game itself ships, and consumed charges are topped up every 200 ms, so the number
never sits at x0. This includes invisibility and ethereal, so the character may look
invisible; uncheck them if you want to see the character. Short buffs from potions are
not touched when they are unchecked.

**Right-click teleport** - off by default. Right-click anywhere in the room and the
character moves there - up onto a platform, down onto a floor, sideways, in one click.
The target is checked against the room's own walkability grid (63x31 cells of 16 pixels,
read from the game's memory on every click), and it stays in the column you clicked: thin
air drops you onto the floor under the cursor, a wall raises you onto its top, so you land
where you pointed instead of being pulled sideways towards some other floor tile. Only a
click outside the room, or in a column that has nothing standable in it at all, is
ignored - the character then simply does not move. The click is converted with the
transform the game itself uses: a 640-world-unit-tall view scaled to the window height,
with world (450,286) at the centre of the client area (the room centre, 54 units left and
40 up). Both scale with the client size, so resizing or maximizing the window is
automatic. `[cheats] tp_scale` zooms that mapping and `tp_bias_x` / `tp_bias_y` shift it
(world units); all three are re-read on every click. Measured on 16:9 at 2048x1152 and at
maximized 2560x1529 - if 4:3 or 16:10 lands off, those two bias keys are the knobs (no
restart needed). The game's own camera also sits a little differently in each room (up to
~15 world units, i.e. 30-75 screen pixels), and it is not stored anywhere readable; a room
that consistently lands slightly off can be nudged with the same two bias keys.
In multiplayer the landing is written to both the player entity and the network player
object the game draws from; this was tested in host and join sessions.

**Infinite jump** - every press of the jump key refreshes the full jump impulse, in the
air as well as on the ground. The MOD asks the same room grid whether there really is
floor under the character, because the game's own "grounded" flag is also 0 while falling
- that used to swallow the press exactly when you wanted to save a fall. In multiplayer
the press also switches the player to the game's own jump state; if the network snapshot
cancels the impulse, it is re-applied for a short moment. Ladders are left entirely to
the game, and the multiplayer position catch-up pauses while climbing.

**Big map** - open the map with M. `Reveal whole map` is a button: click it, confirm the
prompt, and every room of the current zone is marked explored, so the whole 16x16 map is
drawn and every room can be picked; right-click a room to travel there. The reveal is
written into the save object, so a save made afterwards keeps that zone revealed; load a
save from before the reveal to go back to normal. Travel uses the game's own room
transition and takes a moment; a room whose entry point is not known yet is entered at an
approximate spot, so right-click the same room again if the screen stays black.

**Light this room** (hotkey **F9** in game) - lights every torch, brazier and lamp in the
room you are standing in instead of only the one you are next to. The switch is written
into the game's own per-room object table, which is what the game rebuilds a room from, so
the lights stay on when you leave the room and come back. Fixtures that are already lit are
left alone. Some light objects only add ambient brightness and have no flame sprite, so
nothing visible changes for them. This writes live game memory only (nothing on disk);
whether it survives loading a save has not been tested. It does not replay the game's own
ignition effect, and it does not touch achievements; a room whose puzzle is "light every
torch" may not fire its script, so use it with that in mind.

**Light all rooms** - the same switch, but for every room of the current zone at once: the
room object tables of a zone are all preloaded in one static array, so the button walks all
256 slots and turns on every torch, brazier and lamp it finds. It asks for confirmation
first, because it reaches far beyond the room you are in: rooms whose puzzle is "light every
torch" are opened without their script running, the other player in a co-op game will not see
it, and there is no undo button (load a save to go back). The scope is the **current zone
only** - another zone has its own table set - and whether the state survives a save has not
been tested.

**Unlock all MP maps** - in the multiplayer lobby the official co-op maps are the
single-player dungeons, and each one normally needs the previous dungeon cleared before
it can be picked. This button marks every map in the save's map list as unlocked, for the
save slot that is selected right now. It is **only enabled on the scene step, once the
game has loaded the multiplayer save table and the selected slot carries a multiplayer
save** (the same "current" slot the saves window shows); on the mode/character steps and
before a save is loaded it stays greyed out. A brand new save with no dungeons cleared
counts as loaded - the list simply starts with every map locked.

This is a **session-only unlock**: it writes the game's map-list cache, and that cache
is rebuilt from real progress when the save is loaded, so maps you have not actually
finished are locked again after reloading the save. Entering one of the opened maps and
completing it records that completion normally (the map stays checked afterwards).
It only changes the co-op map list.

**Display** - 4:3 / 16:9 / 16:10 and window scale steps, applied on the next launch,
windowed mode only.

## Settings

`unepic_mod.ini` sits next to `unepic.exe` and is created from the shipped template on
install; the mod never overwrites an existing file.

```ini
[video]  resize / aspect / scale / width / height / disable_fso
[cheats] god / maxxp / nocons / nogun / airjump / speed / atkspd
         teleport / tp_scale / tp_bias_x / tp_bias_y
[items]  lang / hidenamed
```

Everything except `tp_scale`, `tp_bias_x` and `tp_bias_y` is read at startup, so
restart after editing.
`nogun` (ranged weapons do not spend ammo) has no checkbox and defaults to on.
Buff/debuff selections and the effect-name language are kept separately in
`mod\buffs.ini` and are edited in the `Buffs...` window; they are also loaded at startup.

| section | key | default | meaning |
|---|---|---|---|
| video | `resize` | 1 | 0 leaves the game window as the game set it |
| video | `aspect` | 0 | 0 = 4:3, 1 = 16:9, 2 = 16:10 |
| video | `scale` | 20 | tenths of the base size, 10 = 1.0x, capped to the desktop |
| video | `width`, `height` | 0 | optional exact size; both must be non-zero to override |
| video | `disable_fso` | 1 | disable Windows fullscreen optimizations for the game |
| cheats | `god`, `maxxp`, `airjump`, `teleport` | 0 | on / off |
| cheats | `nocons`, `nogun` | 1 | infinite consumables / no ammo or durability use |
| cheats | `speed` | 100 | movement speed percent, 10 - 1000 |
| cheats | `atkspd` | 10 | attack speed in tenths, 10 - 100 |
| cheats | `tp_scale` | 100 | teleport distance correction, percent, 10 - 400 |
| cheats | `tp_bias_x` | 0 | teleport click correction, world units, -400 - 400 |
| cheats | `tp_bias_y` | 0 | same for y; both shift the measured 640-unit view anchor |
| items | `lang` | en | item name and description language |
| items | `hidenamed` | 0 | hide catalogue rows that have no dictionary name |

## Troubleshooting

- **The panel does not open.** The game has to be the foreground window; press the
  backquote key. `unepic_mod.log` next to `unepic.exe` says what loaded.
- **The game does not start after installing.** Close it, run `mod\uninstall.ps1`,
  verify the game files in Steam, then install again.
- **It stopped working after a game update.** Only v1.51.1 build 15101 is supported;
  uninstall until a version for the new build exists.
- **A save slot is missing or in the wrong place.** The saves window works on the files
  in the Steam userdata folder; reopen the game's load menu after copying or deleting.
- **Achievements do not appear.** Launch through Steam with Steam online. Unlocking is
  permanent - test on an account you do not mind changing.
- **Rows show internal names.** Those items have no entry in the game's own name table;
  switch language or tick `hide rows without a name`.

## Building

Requirements: Windows, and the Zig toolchain (it carries clang/LLVM, the MinGW-w64
headers and the Windows import libraries). Nothing is downloaded. The shipped binaries
were built with Zig 0.17.0; 0.14+ works.

```
.\build.ps1                     # if zig is not on PATH: .\build.ps1 -Zig C:\path\to\zig.exe
.\build.ps1 -Warnings           # adds -Wall -Wextra
.\build.ps1 -Out mybuild        # output folder (default .\build)
.\build.ps1 -HistoricNames      # links the core as unepic_mod_core_new.dll; only needed
                                # when comparing against the v1.0.0 package
```

The two commands it runs, which is the whole build:

```
zig cc -target x86-windows-gnu -shared -O2 -o unepic_mod_core.dll src/unepic_mod_core.c -luser32 -lgdi32 -limm32 -ladvapi32
zig cc -target x86-windows-gnu -shared -O2 -o SDL2.dll           src/unepic_loader.c   -luser32
```

Diagnostics under `-Wall -Wextra`: two warnings, both known and harmless - an unused
parameter in the core (`edit_int`'s `def`, `src/unepic_mod_core.c:2411`) and an unused
variable in the loader (a leftover `RECT wa;`, `src/unepic_loader.c:100`). Nothing else.
Zig caches compilation output, so re-running the identical command can replay the cached
warnings; rename the output to force a fresh compile.

## Verifying that a build matches the release

Neither the DLLs nor the archive is bit-reproducible, so never compare whole-file hashes:

- LLD writes a randomised PE `TimeDateStamp` and build-ID into every link, and embeds the
  output file name; building in a different folder also moves the string table.
- The zip container stores each file's modification time, so re-packing the same content
  later gives a different archive hash. Compare the files *inside* instead - that is what
  `python -X utf8 tools/build_release.py check` does (entry by entry, md5).

To compare a rebuilt DLL with the one in the release archive, use the section tool:

```
python -X utf8 tools/sect_diff.py build/unepic_mod_core.dll <unpacked>/mod/unepic_mod_core.dll
python -X utf8 tools/sect_diff.py build/SDL2.dll             <unpacked>/mod/SDL2.dll
```

It prints a per-section byte-difference table and exits 0 when `.text` is identical.
Measured on the author's machine:

| build | `.text` | `.rdata` | `.data` / `.reloc` / `.tls` | header + `.buildid` |
|---|---|---|---|---|
| `build.ps1`, core | 0 bytes differ | 0 differ | 0 differ | 4 (timestamp) + 16 (GUID) |
| `build.ps1`, loader (`SDL2.dll`) | 29 differ | 388 differ | 0 differ | same 4 + 16 |

Not one instruction differs. The 29 + 388 bytes in the loader are the embedded PDB path
string (its length varies with the build directory) and the pointers in `.text` that
point into that string table - read as dwords they are all `0x1003exxx` with a constant
offset. Against the v1.0.0 package (whose core was linked as `_new`), building without
`-HistoricNames` changes those pointer bytes too (`.text` 918, `.rdata` 3247) purely
because of the embedded name. `-O0` compiles the same sources to three times the size,
confirming the release files are optimised builds.

Shipped hashes:

```
mod/unepic_mod_core.dll  398,336 B  sha256 f69a571c66ed8f78e4f75f9cf131de38732dc1e3f323f2f70884721c11a38ab5
mod/SDL2.dll             276,480 B  sha256 877531d9a9726f131ca678880d101e44980793566b27eaa276839527ca2ec080
```

## Why antivirus may flag it

At the time of writing, VirusTotal reports 8 of 68 engines on the release archive, and
all eight are the same BitDefender-based engine family with a generic detection name
(`Gen:Variant.Babar.*`) - no behavioural claim, and Defender, ESET, Kaspersky, Sophos
and Malwarebytes are clean. The reason is structural: Unepic has no scripting interface,
so a mod for it must arrive as a proxy DLL that forwards a library the game already
loads and then patch instructions inside the game process. That is the normal shape of
game modding and also the shape heuristics dislike. Rebuilding with different compiler
flags, adding a version resource or repacking does not change that verdict; the sources
here do.

## Permissions

Use and modify it for personal play however you like; please do not re-upload the
package or modified builds to other sites. Unepic and its assets remain the property of
their authors, and the game's own name/description dictionaries are their content, not
mine. The mod is provided as-is, without warranty.

(C) 2026 Quiet Game Archive
