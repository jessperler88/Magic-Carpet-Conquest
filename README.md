# Magic Carpet (1994) - decompilation and native Windows port

A reverse-engineering of Bullfrog's **Magic Carpet** (DOS, 1994: the retail `carpet.exe`, Watcom C / DOS4GW)
and a translation of it to modern C++17 that runs natively on Windows x64 with SDL2.

- **Faithful**: the port is checked tick by tick against the original running in DOSBox. Every Thing, player
  record and random number is byte-identical over the shipped movie and eight recorded levels, and the drawn
  frames are pixel-identical (including 33 front-end screens). `--faithful` plays exactly like the original.
- **Quality of life** (each one a setting, off in faithful mode): draw distance up to 127 cells instead of
  ~20, the 3D view at your display's native resolution (1080p-4K, widescreen), no 1000-object limit (late
  levels stay winnable), smooth frame rates with a fixed 25 Hz simulation, controller support, save anywhere,
  a pause menu, `mcport.ini` configuration.
- **Debug suite**: in-game console, scenario scripts with assertions, Thing inspector and overlays, pause /
  step / fast-forward, free and orbit cameras, headless runs with JSON dumps, desync tools.
- The whole game works: front end, the 50-level campaign, sound effects, General MIDI or emulated OPL2 FM
  music, the intro / level movies, network play over TCP.
- **Hidden Worlds**: the 25 levels of the *Magic Carpet Plus* expansion (snow worlds included) continue the
  campaign after level 50, played exactly as the original `HIDDEN.EXE` plays them (see below).

A sister project, **magic-carpet-conquest**, builds a new PvP RTS / RPG mode on top of this port
(`docs/ROADMAP_PHASE4.md`). This repository stays the plain decompilation + port.

## You need your own copy of the game

**No game files are included, and none may be added.** Magic Carpet is © Bullfrog Productions / Electronic
Arts. The port loads all data (levels, graphics, sounds, music, text) from your copy at run time, and even the
constant tables the code needs are extracted from *your* `carpet.exe` when you build (`tools/port/`); the
repository holds only original code, analysis and documentation. The game is sold by GOG as
*Magic Carpet Plus*; the port needs the DOS retail files (`carpet.exe`, `data/`, `levels/`, `sound/`, ...) in one
folder - by default `MagicCarpet/magic/` next to `src/` (ignored by git), or anywhere via `-DMC_GAME_DIR=<dir>`.

This is an unofficial fan project, not affiliated with or endorsed by Electronic Arts.

### Hidden Worlds (optional)

The GOG *Magic Carpet Plus* CD image also holds the Hidden Worlds expansion (`HIDDEN.EXE`, its own textures with
snow, palette, sky, sprites and `ddlevels.dat`). Copy those files from your CD next to the game once:

```
python tools/port/install_hidden_worlds.py "C:/Games/MagicCarpet"     # reads the GOG CARPET.CD/game.gog image
```

(writes `<game>/hidden/`; a different source: `install_hidden_worlds.py <game> <image or extracted CARPET folder>`,
or point `MC_HIDDEN_DIR` at an extracted CARPET folder). With the files present, winning base level 50 continues
with Hidden Worlds level 1 ("Goyaan") and the outro plays after level 25; `mcport <game> play 100` .. `play 124`
starts a Hidden Worlds level directly. Hidden Worlds levels run the expansion's changes (snow terrain pass, the
reworked Fire Wall, ...) and the 1995 engine behaviour `HIDDEN.EXE` was built with (AI, castles); both are
verified tick by tick against `HIDDEN.EXE` (`docs/analysis/hidden_worlds_re.md`, `port_hidden_engine1995.md`,
`port_reference_hw.md`). Base levels are unaffected.

## Building (Windows)

Prerequisites: Visual Studio 2022 or newer (C++ workload), CMake 3.25+, Python 3.10+ (on the PATH; the build runs
`tools/port/*.py`), [vcpkg](https://vcpkg.io) with `vcpkg install sdl2:x64-windows` and the `VCPKG_ROOT`
environment variable pointing at it.

```
cd src
cmake --preset msvc-x64 -DMC_GAME_DIR="C:/Games/MagicCarpet"     # the folder with your carpet.exe
cmake --build --preset release                                    # or debug
..\build\Release\mcport.exe "C:/Games/MagicCarpet"                # play (front end, campaign)
cd ..\build && ctest -C Release                                   # tests (the reference ones skip without dumps)
```

**How to play: [`docs/CONTROLS.md`](docs/CONTROLS.md)** (keyboard, mouse, controller, debug suite).

(`msvc-x64-vs2022` is the same preset for Visual Studio 2022.) Analysis tools: `pip install -r requirements.txt`
(`propack`, `pillow`, `numpy`; `capstone` / `unicorn` for disassembly; `pyghidra` only for the Ghidra workflow).

## Licence

GPL-3.0-or-later (`LICENSE`). It covers the code, tools and documentation in this repository - not the game,
its data or anything extracted from it.

## Repository layout

| Path | What |
|---|---|
| `src/` | The port: `mcdata` (C data layer), `mcengine` (the translated game, C++17, no SDL), `mcport` (SDL2 platform, the executable), `tests/` (60 ctest tests). |
| `tools/mctools/` | Python toolkit: RNC decoder, LE parser, level / sprite / tmaps / palette decoders, data extractor. |
| `tools/port/` | Table generators run by the build (`gen_exe_tables.py`, `gen_dispatch.py`), OPL check, PPM to PNG. |
| `tools/analysis/` | Helpers for the Ghidra export and the name files (see its README). |
| `tools/reference/` | The reference harness: patches a *copy* of carpet.exe (or HIDDEN.EXE: `patch_hidden.py`, `run_level_hw.py`) to dump its state per tick in DOSBox, and the diff / desync tools. |
| `ghidra/` | Ghidra scripts and the curated function / label / type names (`names/`); `export/carpet_functions.csv` is the function inventory. The Ghidra database and the decompiler output are not in the repository (rebuild them from your exe with the scripts). |
| `docs/` | `ROADMAP.md` (plan + status), `ENGINE.md` (how carpet.exe works), `FORMATS.md` (data formats), `port/PORTING.md` (translation conventions), `analysis/` (per-subsystem reports), `ROADMAP_PHASE4.md` (the Conquest plan). |
| `MagicCarpet/`, `extracted/`, `build*/` | Local only (git-ignored): your game copy, decoded data and reference dumps, build trees. |

## Developer quick reference

```
pip install -r requirements.txt
cd tools && python -m mctools.extract            # decode all game data into extracted/
cd ../build && ctest -C Debug                    # unit tests, whole-simulation tests, renderer, references
python ../tools/reference/make_refgame.py && python ../tools/reference/run_reference.py   # per-tick dumps of the original (DOSBox, ~1 min)
python ../tools/reference/run_level.py 44           # per-tick dumps of the original playing a level (record + replay); fb/ = frame-buffer dumps
Debug/reference_test.exe <game dir> ../extracted/reference/movie0   # diff the port against them
Debug/mcport.exe <game dir>                      # the game: front end, campaign, saves, sound + music
Debug/mcport.exe <game dir> play 0               # straight into a level: mouse steers, WASD / arrows speed/slide, Tab / Enter spell book, mouse buttons cast, Esc pause menu
Debug/mcport.exe <game dir> 38                   # free-fly viewer: WASD/QE/RF, mouse (RMB) look, L/K levels
Debug/mcport.exe <game dir> demo                 # play the shipped movie (Tab free camera, Space pause, [ ] speed)
Debug/mcport.exe <game dir> network              # multiplayer lobby (MC_NET_HOST / MC_NET_PORT)
Debug/mcport.exe <game dir> load 3               # resume save slot 3 (Ctrl+F1..F10 save, Shift+F1..F10 load)
Debug/mcport.exe <game dir> --faithful           # the original's behaviour (320x200 view, 20-cell fog, 1000 Things)
Debug/mcport.exe <game dir> play 5 --set render.draw_distance=127   # any mcport.ini key
python ../tools/reference/run_player.py           # port-recorded scripted player runs replayed by the original (per-tick dumps)
```

Settings: `mcport.ini` in the save directory (`%APPDATA%/Bullfrog/MagicCarpet`) is written on the first run
with every key and its default commented out (far view, native resolution, 8192 Things, interpolation on).
Precedence: defaults < ini < environment < `--set`. Alt+Enter = fullscreen, F11 = frame-time overlay.

Ghidra (headless) workflow:

```
set GH=C:\tools\ghidra_12.0.4_PUBLIC\support\analyzeHeadless.bat
%GH% "C:\Magic Carpet\ghidra\project" MC1 -process carpet.exe -noanalysis ^
     -scriptPath "C:\Magic Carpet\ghidra\scripts" ^
     -postScript ApplyNames.java "C:\Magic Carpet\ghidra\names\carpet_names.csv" ^
     -postScript ExportAll.java "C:\Magic Carpet\ghidra\export"
```

Name files: `carpet_names_tables.csv` is generated from the Thing class/model handler tables
(see `docs/ENGINE.md`), `carpet_names.csv` is hand/agent curated and applied second (wins on
conflict), `carpet_labels.csv` names code inside functions (rasteriser internals; apply with a
second argument `labels`), `carpet_types.txt` defines the structs and typed globals (`ApplyTypes.java`).
Other scripts in `ghidra/scripts`: `FixPointerTargets` (functions at data-referenced code pointers,
list in `names/code_ptr_targets.txt`), `FindGapFunctions` (bounded gap filler), `RepairFunctions`
(create functions at listed addresses, `names/repair_functions*.txt`), `DeleteStubs` +
`ReflowBodies` (remove bogus functions, clear their stale code units and regrow the real owners,
`names/delete_stubs*.txt` / `reflow_ranges*.txt`), `FixJumpTables` (switch tables the analyzer
refused, `names/jump_tables.txt`), `ListNoReturn`, `DumpUnits`, and the no-return repair set
`ClearNoReturn` + `RestoreCallFallthrough` + `MergeCallFallthrough` + `MergeJumpFragments`
(keep the "Non-Returning Functions - Discovered" analyzer off for carpet.exe).
Round-4 repairs: `ClearJumpOverrides` (stale CALL_RETURN overrides on jumps, which killed the decompiler),
`FixConventions` (functions created by the repair scripts had an "unknown" calling convention),
`RemoveTableFunctions` (functions sitting on `switchdataD_` jump tables). `ExportAll` retries a function that
fails or times out with the pointer-to-struct globals untyped (Ghidra 12.0.4 decompiler bug on `game_main_32a00`).
Round 6: `repair_functions_9.txt` (the HMI song timer callback 0x66898 and its lock markers); `carpet_types.txt`
now also types the resource lists, the spiral ring tables, the camera log and the sprite cache, and labels the
FLI / front-end / sound globals (`docs/analysis/names_round6.md`).


Prerequisites installed on this machine: Ghidra 12.0.4 with the `ghidra-lx-loader`
extension and a custom Watcom calling-convention spec (`x86watcom.cspec`),
Python 3.13 (`propack`, `pillow`, `pyghidra`), vcpkg with `sdl2:x64-windows`,
Visual Studio 18 Build Tools, CMake 4.3.

## Where things stand (2026-10-08, Conquest round 11 session 2 - branch `round11`)

- **Generated worlds** (`src/mcengine/worldgen.h`): `mcport <dir> rts --size 1024 --bots 3` builds a seeded
  1024-cell world (512 .. 4096; `--generate` for a generated 256 world, `--tiled` for the old tiled test bed): a torus
  fractal with the campaign's local relief plus continent-scale levels, symmetric for fairness (point / mirror / 90°
  / dihedral, from the player count or `--symmetry`), a sea level (`--water %`), rivers, a flattened plateau per
  seat, and the original's own Things placed as features - mana springs, neutral towns, creature lairs graded 1-5 by
  distance, wild creatures, groves, linked **teleport portal pairs** and hidden **trigger areas** (mana, ambush,
  earthquake, volcano, storm, craters, an elite, a spell) that fire for any wizard, computer wizards included.
  Controls: `--rough % --continent % --springs N --wild % --lairs N --triggers N --portals N` (`none` for the counts).
  1024 builds in ~0.15 s, 2048 in ~1 s. Generated worlds run with a 16384-Thing pool (32768 from 2048 cells) and
  campaign-like scenery density; draw distance goes to 255 there (127 stays the cap for the campaign and Hidden
  Worlds). 66 ctest tests, every reference set still identical.
- Positions are 32-bit and the world size is a run-time parameter (256 .. 4096 cells, `src/mcengine/world.h`); the
  original game runs through the same code and stays byte-identical to the original (all reference sets).
  Plan: `docs/ROADMAP_PHASE4.md` round 11 (session 3: wake-near-any-wizard, world tests, two-instance sync).

## Before that (2026-10-08, Hidden Worlds)

- The Hidden Worlds expansion is part of the campaign (campaign index 100 + k = `ddlevels.dat` entry k, data set 1 via
  `mcengine/world_set.h`). All 25 levels generate byte-identically to `HIDDEN.EXE`, and idle-player playbacks of six
  of them match it over 5000 ticks each; 60 ctest tests, base references identical.

## Before that (2026-10-08, after port round 10 = Phase 4 round 1: debug suite + mode skeleton)

- Round 10 (`docs/port/BRIEFING_round10.md`, reports `docs/analysis/port_{mode,console,inspect,timectl,desync}.md`),
  59 ctest tests, references identical:
  - `mcport <dir> rts [--seed S] [--bots B] [--map 50..69] [--reseed] [--ticks N] [--dump-every N --dump-dir D]`: a
    Conquest-mode run (256 map from a multiplayer template until round 11) with JSON state dumps; mode state
    outside GameState (`mode.h`), carried by save states, network rules, movie format v3.
  - Console: **`` ` ``** (history, Tab completion, `help`); headless `--console-stdin`, `--scenario file.scn`
    (exit 1 on a failed assertion; examples in `src/tests/scenarios/`). Debug commands (teleport, spawn, give,
    god, kill, claim, heal, spells, win / lose, damage) are command packets: recorded, replayed, never in a
    network game.
  - Time: **Pause** pause, **End** / Shift+End step 1 / 10 ticks, **PageUp / PageDown** faster / slower (x1/16 ..
    x64, max), **Insert** x1; `MC_TIME_SPEED=max` headless. **Delete** free debug camera (Shift+Delete teleports
    the wizard there). **Ctrl+F11** PNG screenshot (`<save dir>/screenshots`). **F11** frame time / + tick profile.
  - Inspector / overlays: **Home** or middle mouse = cursor mode (the spell book's pointer; left click inspects a
    Thing, left click on nothing closes, right click picks a cell, right drag orbits a followed Thing),
    **Backspace** closes the panel, keypad 8 / Ctrl+Home / `cam follow` follow (the mouse orbits the camera around
    the Thing, wheel zooms, steering frozen), keypad 7 / 9 previous / next Thing; keypad 1-6 overlays grid /
    labels / anchors / occupancy / damage / net, keypad 0 all off.
  - Log `<save dir>/mcport.log`; desync tooling: checksum parts, `desync_*.mcs` dumps on a network mismatch,
    `tools/reference/diff_state.py`, `tick_log_diff.py`, `mcport <dir> --replay-check N`.

## Before that (2026-10-07, after port round 9 = Phase 3 round 3)

- Round 9 (`docs/analysis/port_round9.md`): Esc in a level opens a pause menu (save / load state slots,
  options written back into `mcport.ini`, leave level, quit; `keys.menu = none` restores the original's
  Esc); far-away mana balls show their current owner's colour (a claimed wizard hoard no longer stays in the
  dead wizard's colour); save states keep the gameplay rules they were played with.

- Round 8 (`docs/analysis/port_round8.md`): dragon / worm segments no longer flicker in the far view (drawn
  follow-the-leader every frame); `game.possession_range_pct` (play default 130) lengthens the Possession shot
  and its target pick; movies (`mvx` v2) and network sessions (host's rules) keep that setting and the pool size
  consistent; interpolation tells reused Thing slots apart; MSVC links without incremental linking;
  WASD flight and Tab for the spell book (`keys.wasd`, `keys.book_tab`, on in play, arrows / Enter keep working).
- Round 7: extended renderer (draw distance up to 127 cells, any resolution, threaded), native-resolution
  composition with the HUD scaled on top, Thing pool beyond 1000 slots, fixed-timestep simulation with render
  interpolation, `mcport.ini` config, game controller, save anywhere. All of it is off with the default
  `PortSettings` (tests, references); see `docs/ROADMAP.md` Phase 3.

## Before that (after port round 6)

- carpet.exe: 1,690 functions after the inventory cleanup (was 2,083 with ~450 bogus
  entries), all named, none marked uncertain, all decompiled (one via the ExportAll fallback); `docs/ENGINE.md` describes renderer and rasteriser, Things, AI,
  castles, terrain generation, tick order, input/sound/network/demo, front end, save
  format, the C runtime and the HMI sound drivers.
- Phase 0 and Phase 1 done. Phase 2 (port): every Thing handler of the game is translated
  (terrain, rasteriser, renderer, things, projectiles, spells, effects, castles, all 17 creature
  types, the computer wizards' AI, local input, the HUD, the game-side sound manager). Since round 4
  the port is checked tick by tick against the original: a patched copy of carpet.exe in the bundled
  DOSBox dumps the game state every tick of the shipped movie (`tools/reference/`), and
  `reference_test` shows every Thing, player record and the RNG byte-identical over the whole
  movie (a ctest gate). Round 5 (2026-10-07) made it a playable game: `mcport.exe <dir>` runs the front end,
  the campaign with level results and save games, sound effects and General MIDI music; the original's
  per-tick state is matched on eight more levels (every creature type, the AI on six levels) and its drawn
  frames are matched pixel for pixel over movie 0 (render_reference_test). Round 6 closed Phase 2: FM music on an
  emulated OPL2 through the original's driver (default), network play over TCP (`mcport <dir> network`),
  movie recording (Alt+R) checked by replaying port recordings of every spell in the original, and pixel
  references for 640x480, every render option and the front end. Conventions: `docs/port/PORTING.md`; per-subsystem reports `docs/analysis/port_*.md`;
  plan and status `docs/ROADMAP.md`.
- Ghidra structs: `ghidra/names/carpet_types.txt` (applied by `ApplyTypes.java`) types the
  game state, config, Thing pool and the renderer tables, so decompiled code reads
  `g_state->things[i].health`.
- To continue a naming session: read `docs/ENGINE.md` and `docs/ROADMAP.md`, query functions
  with `tools/analysis/mc.py` (decompiled C) and `tools/analysis/img.py` (raw disassembly),
  add rows to `ghidra/names/carpet_names.csv`, run the headless chain (tables CSV, curated
  CSV, labels CSV, then ExportAll), check `ghidra/export/carpet_functions.csv`.
- Full maintenance chain after an analysis change (all scripts in `ghidra/scripts`, lists in
  `ghidra/names`): `DeleteStubs` -> `ReflowBodies` -> `RepairFunctions` -> `FixJumpTables`
  -> `ClearJumpOverrides` -> `FixConventions` -> `RemoveTableFunctions` -> `ApplyTypes` -> `ApplyNames` x3
  -> `ExportAll`. Pitfalls are in the script headers and
  `docs/ROADMAP.md`; in short: keep "Aggressive Instruction Finder" and "Non-Returning
  Functions - Discovered" off, never trust a function that starts on padding bytes.
