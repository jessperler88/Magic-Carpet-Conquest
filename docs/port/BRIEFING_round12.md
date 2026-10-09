# Port round 12 briefing (2026-10-09): Phase 4 round 3 - launching the Conquest mode

Read first: `docs/port/PORTING.md` (conventions, report format), `docs/ROADMAP_PHASE4.md` sections 1-2 and 3 / round 12,
`docs/analysis/phase4_code_platform.md` section 4 (front end: urn, setup screen) and `phase4_code_gameplay.md` (death /
respawn, castles, the `owner == owner` tests), `docs/analysis/port_world_session3.md` (what round 11 left), then the
**contract headers** below and the files of your task.

## Goal and gate

Round 12 makes the mode playable from the menus, end to end, offline and in the lobby:

- the red **urn** bottom right of the main menu opens a **setup screen** (seed, size, bots, teams, generator controls,
  win conditions, minimap preview); "Start" plays the match; the match ends by its **rules v1** (respawn at the
  castle, elimination, desecration, win by elimination / mana dominance / timer) and shows a **result screen**, then
  the main menu again;
- the **bots** are the original's wizard AI made fit for a match (skill, allies, a generated world);
- the **lobby** has the same urn: the session plays Conquest with the host's setup.

**Gate**: a human vs 3 bots match on 1024 from the urn to the result screen (driven headless by a test, and by hand
in `build/Release/mcport.exe`); the same match from the network lobby between two processes stays in sync (a
`net_test` / `world_net_test` mode part). Every existing test and reference stays identical (69 ctest tests today).

## What the integrator already did (commits `ee81f7b`, `daf3859` on branch `round12`, 69/69 ctest)

- **`mode.h`**: `MatchRules` (32 bytes, 0 = default in every field: teams per record, win bits, mana %, hold
  seconds, time limit, respawn seconds + per castle level, desecration seconds, bot skill) inside `ModeParams`
  (now 64 bytes); `MODE_TICKS_PER_SECOND` = 25; **`ModePlayer`** fields (side, place, respawn_ticks,
  desecrate_rec1 / _ticks, eliminated_tick, stats kills_wizards / kills_creatures / deaths / castles_razed /
  mana_peak, mana_hold_ticks) and flags `MODE_PLAYER_IN_MATCH / _BOT / _ELIMINATED / _DESECRATING`;
  `ModeState.match_state / match_end_tick / winner_side / end_reason` (`MATCH_RUNNING / MATCH_OVER`,
  `MATCH_END_*`). The block stays all zero by default. MODE chunk version 2 (version 1 still loads), movie
  format **v4** (`DEMO_VERSION_MODE`; v3 movies still play), `NetGameRules` carries the bigger ModeParams.
- `g_hook_mode_match_tick` (mode.cpp, called by `mode_tick`) -> `mode_rules_tick`; mode_level.cpp's level-start hook
  calls `mode_rules_level_start()` then `mode_bots_level_start()` (after the human start spells).
- **Stubs to replace** (they compile and link today): `mode_rules.{h,cpp}` (task B), `mode_bots.{h,cpp}` (task C),
  `fe_mode.{h,cpp}` (task A). Both engine files are in `MC_SIM_ALL`. `same_side()` (inline, mode_rules.h) with
  `g_mode_teams` / `g_hook_mode_side_of_owner` / `mode_same_side_records` defined in **mode.cpp** (the simulation
  core calls same_side and every unit test links mode.cpp).
- `frontend.h`: `FE_START_MODE = 4`. `fe_mode.h`: `FE_STATE_MODE_SETUP = 11`, `FE_STATE_MODE_RESULT = 12`,
  `fe_mode_params()`, `fe_mode_set_params()`, `fe_mode_set_result(final ModeState, local record)`, `fe_mode_result()`.
- `mode_set_proposal(mode, params)` / `mode_proposal()` (mode.h): `net_local_rules` proposes it while no mode is
  active, so the lobby's existing `net_agree_rules` exchange already carries the host's mode + ModeParams.
- `PortSettings.mode_menu` (settings.h, default false = the faithful menus).

The contract headers (`mode.h`, `mode_rules.h`, `mode_bots.h`, `fe_mode.h`, `frontend.h`'s FeResult) are fixed:
add functions to *your* header freely; a change to a struct / a declaration someone else implements goes into your
report as exact code.

## Tasks

### A - front end: urn, setup screen, lobby urn, result screen, match HUD (build_A)

Files: `frontend.{h,cpp}`, `fe_mode.{h,cpp}`, `hud.{h,cpp}`, `ui_draw.*` (if needed), `worldgen.{h,cpp}` (**only** an
added preview function), tests `frontend_test.*`, `render_reference_fe_test.cmake` (link line only), new
`fe_mode_test.*`.

1. **Main-menu urn** (phase4_code_platform.md section 4): SpriteDesc 0x4d -> tmaps chunk 116, remapped to the menu
   palette with `palette_find_nearest`, drawn bottom right as part of the background overlay, rectangle hit test
   **before** the `mmmask.dat` lookup, a hover highlight. Only when `g_settings.mode_menu` - with it off every
   front-end pixel stays identical (`render_reference_fe_test`, 33 screens). Click -> state 11. Keyboard reachable
   (Tab cycle includes it when shown).
2. **Setup screen** (state 11, lobby screen as the template): seed (random button, typed digits, copy to clipboard
   is optional - no SDL in mcengine: expose the text, mcport may copy), world size 512 / 1024 / 2048 (256 generated
   allowed too), bots 0..7 and skill (MatchRules.bot_skill), teams (free for all / 2 v 2 / 4 v 4 / custom per seat),
   generator controls (WorldParams: water, roughness, continent, springs, wild, lairs, triggers, portals - sensible
   steps), win conditions (elimination / mana dominance / timer + minutes), respawn and desecration seconds, and a
   **minimap preview** of the generated heightmap (water / land / height shading, seats marked) that updates when
   seed / size / water / roughness / continent / player count change. Generation for the preview must not leave
   anything changed that a level depends on (world size, maps, RNGs, g_state): add e.g. `worldgen_preview(params,
   out, w, h)` that generates the heightmap into its own buffer (or saves / restores what it touches) - prove it
   in a test (a level loaded after a preview is byte-identical to one loaded without). "Start" -> `FE_START_MODE`
   (fe_frame returns it like FE_START_LEVEL, fe_mode_params() holds the choice); Esc / "Back" -> main menu. The
   front end is 320x200, pixel-doubled to 640x400: a dense options screen may draw its own 640x400 image for state
   11 / 12 (fe_present path) instead of 320x200 - your call, keep it readable and the original's look (its fonts,
   palette, panels).
3. **Lobby urn** (state 4): with mode_menu on, an urn toggle in the multiplayer screen; selected =
   `mode_set_proposal(GAME_MODE_CONQUEST, setup params)` before the lobby's rules exchange (mp_join_done), with a
   way to open the setup screen from the lobby and come back. Not selected = `mode_set_proposal(0, {})`. Only the
   host's proposal matters (net_agree_rules); show "Conquest" in the lobby when selected.
4. **Result screen** (state 12): from `fe_mode_result()`: victory / defeat for the local record, end reason, match
   time, the winners, a table per seat (name or "Bot n", side colour, kills, deaths, castles razed, peak mana,
   placing), "Continue" -> main menu (state 2). Entered by mcport with `fe_enter(FE_STATE_MODE_RESULT)`.
5. **Match HUD** (hud.cpp, render only, reads g_mode / mode_rules.h queries): respawn countdown while dead, match
   timer when there is a limit, desecration progress (yours being channelled / you channelling), "eliminated -
   spectating", a short victory / defeat banner while the match is over before the level ends. Nothing when no mode
   is active (every HUD reference identical).
Tests: `fe_mode_test` drives `fe_frame` with mouse / key input: main menu (mode_menu on) -> urn -> setup -> change
seed / size / bots -> Start returns FE_START_MODE with exactly those params; Esc returns; result screen renders a
synthetic ModeState; PNG/PPM screenshots of the menu with the urn, the setup screen (two seeds), the lobby with the
urn, the result screen - **look at them**. With mode_menu off, the menu is the reference image.

### B - match rules v1 and teams (build_B)

Files: `mode_rules.{h,cpp}`, `mode.cpp` (only if needed), `player.cpp`, `castle.cpp`, `spatial.cpp`,
`creature_common.cpp`, `creatures.cpp`, `creatures3.cpp`, `effects.cpp`, `projectiles.cpp`, `constructors.cpp`,
`spells.cpp`, `thing.cpp` (owner tests only), `debug_cmd.{h,cpp}`, new test `mode_rules_test.*`.

1. **Sides**: `mode_rules_level_start` resolves MatchRules.team into ModePlayer.side (records with team 0 get a side of
   their own that no team uses), sets IN_MATCH / BOT for the seats of the run (humans + bots), `g_mode_teams`, and
   installs `g_hook_mode_side_of_owner` (wizard Thing index -> its record's side; anything a wizard owns has that
   wizard as owner - check). Replace the original's owner comparisons where an ally must count as own by
   `same_side()` (~25 sites: spatial.cpp, creature_common.cpp, creatures*.cpp, effects.cpp, projectiles.cpp,
   castle.cpp, player.cpp, constructors.cpp, thing.cpp; ai_wizard.cpp is task C's - list in your report which AI
   sites you saw). Decide per site (damage, targeting, collision, mana claims, castle entry) and document it in a
   table in your report; mana stays per wizard (no shared team mana in v1).
2. **Respawn**: in a mode run a dead wizard (human or bot) with a castle respawns at the castle after
   `respawn_s + respawn_level_s * castle level` seconds (ModePlayer.respawn_ticks counts down; a human may need to
   press Space = cmd 0xf after the timer, a bot respawns at 0). Keep the original's paths for everything else.
3. **Elimination**: dead without a castle = eliminated (instead of the original's "level lost" for a human / "leaves
   the game" for a bot); a wizard alive without a castle is not eliminated (can still build one). Eliminated
   records stay in the world as spectators (no Thing to fight, no respawn); their castle-less mana stays where it is.
4. **Desecration** (`desecrate_s`, 0xff = off): an enemy wizard alive inside a castle's courtyard (the test of
   `castle_crush_wizards`) channels; leaving / dying resets; when complete the castle is razed (every level, its
   mana spilled as in the original's destruction) and its owner is eliminated if dead, otherwise castle-less;
   castles_razed++ for the channeller. Damage to the channeller does not need to interrupt beyond killing him.
5. **Win conditions** (MatchRules.win, default elimination + mana): last side standing; a side's castle + transit
   mana >= mana_pct of the world's mana (the total the original's `game_check_level_won` divides by, `total_mana`)
   for mana_hold_s seconds; time limit -> most castle mana (ties: most kills, then lowest record). Then
   `match_state = MATCH_OVER`, `winner_side`, `end_reason`, `match_end_tick`, places; after ~5 s the level ends for
   **every record on the same tick** (the status bits game_level_tick / game_level_end read: winners won, the rest
   lost) so every peer leaves together. Debug commands `win` / `lose` (debug_cmd.cpp) end the match in a mode run
   (MATCH_END_DEBUG). `MATCH_WIN_NONE` = never ends (tests, sandbox).
6. **Stats**: kills of wizards / creatures by owner (the killer field of the death paths), deaths, castles razed,
   mana peak. Implement `mode_respawn_seconds`, `mode_desecration_pct`, `mode_time_left_seconds`.
All of it only under `mode_active()`; everything deterministic (integers, index order, no local player).
Tests: `mode_rules_test` (MC_SIM_ALL, rts-style runs on a small generated world, scripted with debug packets / direct
state): same_side with and without teams; respawn timing for human and bot; elimination without a castle;
desecration completes / resets on leaving; each win condition fires with the right winner and the level ends for
all records on one tick; ally projectiles / creatures do not hurt allies; checksums of two identical runs equal.

### C - bots v1 (build_C)

Files: `ai_wizard.{h,cpp}`, `mode_bots.{h,cpp}`, `creatures2.cpp` (if a creature AI needs same_side), new tests
`mode_bots_test.*`, and the balance harness (a script under `tools/` or a test mode).

1. **Skill**: `mode_bots_level_start` maps MatchRules.bot_skill to the player block's AI fields (aggression +4,
   accuracy, reaction / respawn delay +8 - see phase4_code_gameplay.md / ai_wizard.cpp) for every bot seat; normal =
   what generated worlds give today.
2. **Allies**: the AI's own target / threat / castle-attack choices skip same_side wizards, castles, balloons,
   creatures (the ai_wizard.cpp owner sites); a bot does not claim an ally's mana if that is cheap to avoid.
3. **Playable end to end on a generated world**: bots build their castle on their seat (round 11 did it), keep it
   growing, respawn through task B's rules (coordinate: B owns player.cpp's death / respawn), roam for mana on a
   1024 world without getting stuck. Fix what a 20-minute headless match shows; the "passive bots" note of round 11
   (template aggression, few spells) is round 17's - only make them functional here.
4. **AI determinism**: the AI's CRT `rand()` seed is already per run (`g_ai_rand_seed` from the match seed); check no
   AI path reads the local player / g_ai_human_wizard in a way that differs per peer in a network mode run.
Tests / harness: `mode_bots_test`: idle human (god mode, debug packet) + 3 bots on 512 and 1024 for N thousand ticks -
every bot has a castle, castles grow, bots die and respawn, no bot attacks an ally in a 2 v 2; the same seeds give
the same checksums. A bot-vs-bot harness (headless `mcport rts --ticks` with dumps, or a test switch) that runs K
seeds and prints a table (castle levels, mana, kills, who won by the rules) - report the numbers.

### D - flow, CLI, lobby path, network (build_D)

Files: `src/mcport/main.cpp`, `rts_run.{h,cpp}`, `config.{h,cpp}`, `mode_level.{h,cpp}`, `net.{h,cpp}`,
`net_tcp.*`, `scenario.{h,cpp}`, `game_menu.*` (if needed), tests `net_test.*`, `world_net_test.*`,
`rts_headless_test.*`, `config_test.*`, new `mode_flow_test.*`.

1. **mcport flow**: `FE_START_MODE` -> start the run with `fe_mode_params()` (rts_begin path, a fresh seed when the
   setup says random), play; when the mode level ends: `fe_mode_set_result(g_mode, local)`, stop the run, back to the
   front end in `FE_STATE_MODE_RESULT` (today `s_rts` quits mcport when the level ends - keep that for the command
   line `rts` run unless `--result` / a scenario says otherwise; from the menu it always returns). Pause menu "Leave
   level" in a mode run = forfeit -> result screen. Save / load states of a mode run keep working.
2. **Config**: `[game] mode_menu` in mcport.ini (play default **on**, `--faithful` off), k_desc line.
3. **CLI** (`rts_run.cpp`, scenario `rts` header): `--teams 1,1,2,2` (per record) / `--teams 2v2`, `--win
   elim,mana,timer|none`, `--mana-pct`, `--hold S`, `--time M`, `--respawn S`, `--respawn-level S`, `--desecrate
   S|off`, `--bot-skill 1..4`, `--result` (show the result screen at the end instead of quitting). Update the usage
   line in main.cpp and rts_run.h's comment.
4. **Lobby path**: after the lobby's `FE_START_LEVEL` with an agreed mode (gameplay_rules().mode / the agreed
   NetGameRules), start the mode run with the host's params (split `mode_start_net_run` into "agree" and "start with
   agreed rules" so the lobby does not exchange twice; humans := the session's player count, bots trimmed to 8
   records); level `MODE_LEVEL_INDEX`; at the end every peer shows the result screen, then the lobby (as the
   original returns to the lobby after a network game). `rts --net N` keeps working.
5. **Network determinism**: a `net_test` / `world_net_test` part that runs the **lobby path** (two processes, a
   proposal from the host, 1024 world, 3 bots, match rules with a short time limit) and checks lockstep checksums
   to the end of the match and that both peers end on the same tick with the same result.
6. `mode_flow_test`: the urn-to-result gate headless - fe_frame driven (main menu with mode_menu on -> urn ->
   setup -> Start) -> the run (`MATCH_WIN_TIMER` with a 1-minute limit or a debug `win`) -> result screen state ->
   main menu; the same with the pause-menu forfeit. (Depends on A's screens: write it against the fe_mode.h contract,
   it passes once A lands; until then drive fe_mode_set_params + FE_START_MODE directly and say so in the report.)

## Rules (as in rounds 6-11)

- **Four agents (A..D) work in the same tree at the same time.** Only modify the files your task names. A change you
  need elsewhere goes into your report as **exact code (file, place)**; the integrator applies it. Shared files
  nobody edits this round: `mode.h`, `mc_types.h`, `mc_image.h`, `world.h`, `mc_globals.*`, `sim.*`, `sim_all.cpp`,
  `thing.h`, `engine.*`, `settings.h`, `src/CMakeLists.txt`, `docs/ROADMAP*.md`, `README.md`, `docs/CONTROLS.md`,
  `docs/ENGINE.md`. New test files and their `.cmake` are yours (tests/*.cmake are picked up by CMake - reconfigure).
  A test of yours that needs another task's work: write it against the contract, report what is pending.
- **Faithful by default, the references are the gate.** Without a mode (and with `mode_menu` off) every reference
  stays byte / pixel identical. After your changes run the whole `ctest -C Release -j 2` in your build dir. If a test
  fails in a file you do not own, another agent is mid-edit: wait and rerun; never edit it.
- **Determinism**: integers only in the tick, index order, nothing simulated reads the local player, the camera,
  settings or real time. Everything new in a match goes through g_mode (hashed, saved, recorded).
- **CPU (user's request)**: build with `cmake --build ../build_X --config Release --parallel 2 -- -p:CL_MPCount=1`
  and run `ctest -C Release -j 2`. Never more. `timectl_test` shares %TEMP%/mc_timectl_test across build dirs: if it
  fails while another agent runs it, rerun it alone.
- **Bash pitfall:** never edit source with python / sed / cat in a Bash heredoc - `\n` reaches the file as a real
  newline and replacements silently miss. **All source edits through the Edit / Write tools.** Throwaway scripts go in
  your scratch dir via Write and run with `python <file>`; never run python with `C:\Magic Carpet` as the working
  directory (`ghidra/` shadows a package).
- Build in **your own build directory** `build_X`: `cd "C:/Magic Carpet/src" && VCPKG_ROOT=C:/vcpkg cmake --preset
  msvc-x64 -B ../build_X` (once; reconfigure after adding a test). **Zero warnings (/W4).** Never build in `build/`.
- Scratch: `<scratchpad>/round12_X/`. Headless mcport runs: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
  MC_SAVE_DIR=<your scratch>`; game dir `C:/Magic Carpet/MagicCarpet/magic`. TCP listeners in tests and runs bind
  `127.0.0.1` (`MC_NET_BIND=127.0.0.1`, set in the test's .cmake ENVIRONMENT), or the user gets Windows Firewall
  prompts. Tests that load several levels in one process reset the carried-over globals (port_world_test.md).
- `far` / `near` are windows.h macros: no variables with those names in files including windows.h. Cast WCoord to
  `(int)` in printf arguments.
- The original game files under `MagicCarpet/` are never modified. GPL: no code from remc2 / mgcarpet / DOSBox.
  **No git commits** - the integrator commits.

## Report

`docs/analysis/port_r12_{fe,rules,bots,flow}.md` (A, B, C, D): what you did, decisions with reasons, test results
(numbers), screenshots you looked at (paths), the exact code you need in files you do not own, what is left open.
