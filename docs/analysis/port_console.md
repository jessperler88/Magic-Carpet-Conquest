# Port round 10, task B: debug commands, command language, scenario files, console

Phase 4 round 10 (docs/port/BRIEFING_round10.md task B). Port-only; nothing here exists in carpet.exe. Every
state-changing debug action is a **command packet** (ids 0x40..0x49) in the local player's command slot, applied
inside the tick by `g_hook_port_command`, so it is recorded in movies, replayed, and lockstep-safe. Queries,
assertions and the console itself only read the state between ticks.

## 1. What was built

| file | what |
|---|---|
| `src/mcengine/debug_cmd.{h,cpp}` | the debug packets: byte layout (header), builders (`debug_cmd_teleport / _spawn / _give_mana / _god / _kill_slot / _kill_class / _kill_wizard / _kill_castle / _claim / _heal / _spells / _level_end / _damage`), `debug_cmd_apply` (the `g_hook_port_command`), `debug_cmd_tick` (god mode, once per tick), `debug_cmd_allowed`, the host-side packet queue (`debug_cmd_queue / _pump / _pending / _clear_queue`), applied-packet messages (`debug_cmd_take_messages`), `debug_cmd_name / _describe` |
| `src/mcengine/scenario.{h,cpp}` (new) | the one-line command language shared by the console, stdin and scenario files (`cmd_parse`, `cmd_query`, `cmd_check`, `cmd_help`, `cmd_complete`, class / type names), the scenario file grammar (`scenario_parse / scenario_load`) and runner (`ScenarioRunner`: `start`, `step(tick)`, `local_input` for the reference_player_test input verbs; `scenario_runner()` shared instance) - engine side, no SDL |
| `src/mcport/console.{h,cpp}` (new) | the in-game console: key handling, line editing, history file, scrollback (reads D's `mclog` ring buffer), drawing, `run file.scn`, the stdin reader thread, `--console-stdin` / `--scenario` options, host commands handed to main.cpp |
| `src/tests/scenarios/*.scn` (new) | `spawn_kill.scn`, `teleport.scn`, `god.scn`, `give_mana.scn`, `input.scn`, `rts_debug.scn` |
| `src/tests/scenario_test.{cpp,cmake}` | every scenario headless over `${MC_SIM_ALL}` (twice, checksums), a deliberate failure, parse errors, record + replay |
| `src/tests/console_test.{cpp,cmake}` | parser, packets byte-exact, help, key-to-ASCII, editing, history file, the console in a level, a screenshot |

No `settings.h` field was added (section B is empty). `main.cpp` (task D) and `player.{h,cpp}` (integrator) need the
changes in sections 5 and 6.

## 2. Debug packets (debug_cmd.h)

The original's 10-byte packet: b0 = `cmd`, b1 = `arg`, b2 = `pad2`, b3 = `steer_x`, b4 = `steer_y`, b5 = `bits`,
b6..b9 = `pad6`. Little endian. **target** = player record 0..7, `0xff` = the sender. After a debug packet was applied
its b3..b5 are cleared, so `player_commands_process` applies "no steering, no keys" for that tick - identically in the
record run, the playback and on every peer (a reserved id 0x4a..0x5f does nothing and keeps the bytes).

| id | name | b1 | b2 | b3..b9 | effect |
|---|---|---|---|---|---|
| 0x40 | teleport | target | 1 = z given | b3-4 x, b5-6 y (world), b7-8 z (signed) | `thing_move_to` + `player_log_position`; without z: ground + 0x100 (the spawn height) |
| 0x41 | spawn | class | type | b3 x cell, b4 y cell, b5 count (1..64), b6 flags (1 = at the cell), b7 target, b8 cells ahead (0 = 4) | `level_spawn_thing_record` of a synthetic level record (cell centre, ground height, the per-class fix-ups); N Things in a square; without the flag: in front of the target's wizard along its yaw |
| 0x42 | give mana | target | 0 wizard, 1 castle, 2 ball | b3-6 amount (int32, may be negative) | wizard: `Thing.mana` + n, clamped 0..mana_total; castle: castle `mana` + n, clamped 0..its mana_total; ball: cheat 2 (a mana ball of n owned by the wizard, the wizard's mana refilled) |
| 0x43 | god | target | 0 off, 1 on, 2 toggle | - | the god flag (below) |
| 0x44 | kill | target | 0 slot, 1 class/type, 2 wizard, 3 castle | b3-4 slot, b5 class, b6 type (0xff = any) | creatures, castles, balloons: health -1 (the game's own death); wizards: health -1 and state 2 (dying) - see 7.2; anything else: marked for deletion. Mode 0 checks the slot still holds that class / type; mode 1 never kills the sender's own wizard, skips body segments (they die with their head) |
| 0x45 | claim | target | - | b3-4 slot, b5 class, b6 type | a mana ball: `mana_owner` = the wizard; anything else: `owner` = the wizard |
| 0x46 | heal | target | bits: 1 wizard, 2 castle (0 = 1) | - | health = max_health |
| 0x47 | spells | target | - | - | cheat 1's spell part: one spell Thing (cost 0) per spell id the wizard lacks; switches are not touched |
| 0x48 | level end | target | 0 win, 1 lose | b3 1 = mark only | win: status 10 (= command 0x1b, won and left); lose: status 0xc (= 0x1c: lost, the campaign restarts the level); mark: status \|= 2 / 4 and the level goes on |
| 0x49 | damage | target | 0 wizard, 1 castle | b3-6 amount | pending damage in slot 0 (attacker = the Thing itself): shields, invulnerability and god apply |

**Things** are named by slot + the class / type they must still have. `thing_slot_generation` (thing.h) is **not
usable inside the tick**: it is a process-local counter of `thing_alloc` calls, in no savestate, movie snapshot or
peer - a playback that starts from a snapshot has other counts than the record run and would refuse what the record
run applied (desync). Class / type are simulated state, so the check is deterministic. (Render-only users - D's
camera follow, C's inspector - may keep using the generation.) If later rounds want a stronger reference, the
generation has to become simulated state (pool extension or mode block).

**`debug_cmd_allowed()`** (unchanged rule, now documented): a mode run applies debug packets only with
`MODE_PARAM_DEBUG` (the run's `ModeParams`, agreed by all peers); otherwise any game that is not a network game
(`Config.flags & 0x10`): campaign levels, the viewer, movies (a recording's debug packets replay). Never in a network
game outside a debug mode run. `debug_cmd_queue` refuses (with the reason) when not allowed and while a movie plays.

**God mode.** In a mode run the flag is `g_mode.players[p].flags & MODE_PLAYER_GOD`. In the campaign it is bit 0 of
the player block byte **P+0x3ae** (`PlayerBlock.pad3ae[0]`, between `slot_left` and `slot_right`): zero in every one of
the original's 51924 per-tick reference dumps (all suites scanned), part of `GameState`, so savestates, movie
snapshots and `net_state_checksum` (which hashes the P block) carry it with no other change; `players_init_records`
clears it with the record at the next level start (god is per level, as in a mode run where `mode_level_start` clears
`ModePlayer`). `debug_cmd_tick()` runs once per tick after the mode tick (**requested hook, section 6.1**): for every
god player whose wizard is alive: health = max_health, mana = mana_total, `invuln_timer` >= 2 (pending damage is
discarded by the flyer / AI update, the HUD's wizard icon blinks). It reads only simulated state and is idempotent.

**Host-side queue.** One packet per player per tick: `debug_cmd_queue` appends to a FIFO (not simulated state),
`debug_cmd_pump()` - called right before each tick (D's `sim_tick` does) - moves the head into the local player's
command slot when it is empty (between ticks it is, except for the join packet of a level's first tick), so packets
queued together run in consecutive ticks, in order. The local input code leaves a pending packet alone
(`local_input_flight` 0x16dcb), so the packet travels as is. Nothing is pumped while a movie plays.

## 3. The command language (user documentation)

One command per line; `#` followed by a space / non-digit starts a comment (`#12` is slot 12). Case does not matter.
Numbers are decimal or `0x` hex. **Coordinates** of debug commands are **cells** (0..255, the cell centre) unless
written with a `w` suffix = world units (0x100 per cell): `teleport 60 70`, `teleport 3200w 4000w 2000`. Heights
(teleport Z) are world units (the ground of a cell is `height byte * 0x20`). **`@N`** anywhere (`@0`..`@7`, `@p2`,
`@me`) = the target player of a simulation command (default: the local player who sends it). **WHO** (queries,
assertions) = `me` | `pN` (the wizard of player N) | `castle` / `castleN` | `#S` / `slotS` (Thing slot S).
**CLASS [TYPE]**: a class name (`scenery`, `player`, `creature`, `weather`, `projectile`, `effect`, `switch`,
`spell`, plurals accepted) or number, then a type name or number (`any`); or a type name alone: player types
`wizard`, `computer_wizard` (`ai`), `castle`, `balloon`; creatures `dragon vulture bee worm archer crab kraken troll
griffon skeleton emu genie builder townie trader wyvern`; spells (`fireball` ... `mini_fireball`, spaces as `_`);
scenery `tree standing_stone dolmen bad_stone`; effects `mana_ball path wall canyon ridge_node black_smoke
white_smoke teleport`. A name alone is looked up in that order (so `castle` = the castle, `spell castle` = the spell).

### Game commands (become packets; run in the next free tick)

| command | |
|---|---|
| `teleport X Y [Z] [@P]` (`tp`) | the wizard to cell X,Y (Z: height, default ground + 0x100) |
| `spawn CLASS [TYPE] [X Y] [xN] [ahead D] [@P]` | N (1..64) Things at cell X,Y, or D (default 4) cells in front of the wizard: `spawn dragon x3`, `spawn creature 2 40 52`, `spawn spell fireball`, `spawn mana_ball 10 10` |
| `give mana N [wizard\|castle\|ball] [@P]` | add N (negative: take) to the wizard (default; up to its total), the castle, or as a mana ball (cheat 2) |
| `give spells [@P]` / `spells [@P]` | every spell the wizard lacks (cheat 1, cost 0) |
| `god [on\|off\|toggle] [@P]` | god mode (default toggle) |
| `kill #S [CLASS [TYPE]]` | Thing slot S (refused when it no longer holds that class / type) |
| `kill CLASS [TYPE]`, `kill creatures`, `kill all` | every live Thing of the class / type (never your own wizard); `all` = every creature |
| `kill wizard [N]`, `kill castle [N]` | player N's wizard / castle (default yours) |
| `claim #S [@P]` | the Thing now belongs to the player (a mana ball: its mana) |
| `heal [wizard\|castle\|all] [@P]` | health back to max |
| `win [mark] [@P]`, `lose [mark] [@P]` | the level is won and left / lost (campaign: restarts); `mark`: only marked, the level goes on |
| `damage N [wizard\|castle] [@P]` | N damage through the game's damage path |
| `cmd C [A [P2]]`, `cheat N`, `book`, `close` | raw original packets: command C / the original's cheat N (command 0x1e: 1 spells, 2 mana, 3-5 destroy wizards / castles / balloons, 6 heal, 7 kill creatures) / spell book open / close |
| `packet B0 [B1 .. B9]` | a raw 10-byte packet |

The console prints `queued: <description>` and, once the tick ran, the result (`spawned 3 x Creature Dragon (5,0) at
cell 40,52: 51 new Things`, `teleported player 0 to 15488,18048 z 256 (cell 60,70)`, ...).

### Host commands (main.cpp carries them out)

| command | |
|---|---|
| `time pause\|resume\|toggle\|normal`, `time step [N]`, `time speed X`, `time run` (= resume), `time` (= toggle) | D's time control; X = `4`, `x4`, `1/2`, `max` ... |
| `shot [NAME]` | screenshot of the next presented frame (D: PNG in `MC_SHOT_DIR` / `<save dir>/screenshots`) |
| `save N`, `load N` | state slot 0..9 |
| `sync` | network sync statistics (E's `net_sync_stats`, first differing checksum part) |
| `dump` | a JSON dump now (A's `dump_now`) |
| `report [file\|clip]` | the Conquest match report (`mode_report.h`: setup, result, one row per wizard, what every bot spent its ticks on, a 30 s timeline, events, a census) to `<save dir>/reports/conquest_s<seed>_<size>_<bots>bots_t<tick>.txt` and the clipboard; `file` / `clip`: only one of them. A match from the menus also writes the file by itself when it ends (or mcport is quit in the middle); `report` on the result screen or later copies that one |
| `cam free\|off`, `cam follow [#S]`, `cam to X Y` | D's debug camera (follow: the slot, else the inspected Thing) |
| `overlay list`, `overlay NAME [on\|off]` | C's overlays (grid, labels, anchors, occupancy, damage, net, all, none) |
| `inspect #S`, `inspect cursor`, `inspect off` | C's Thing inspector |
| `run FILE` | a scenario from now on (FILE, FILE.scn, or `<save dir>/scenarios/FILE[.scn]`; its level / rts header is ignored) |
| `quit` (`exit`) | leave mcport (in a scenario: end the scenario) |
| `echo TEXT` | print TEXT |

### Queries and assertions (read the state between ticks)

| command | |
|---|---|
| `where [WHO]` | slot, class / type, cell, world position, z, yaw, health, mana, state |
| `find CLASS [TYPE]` | the live Things (first 20: slot, cell, z, health, state, owner) |
| `count CLASS [TYPE]` | how many are alive |
| `players` | every player record: active, computer, wizard slot, health, mana, castle (level, mana), GOD, status |
| `queue` | packets waiting for the command slot |
| `help`, `help COMMAND`, `help sim\|host\|query\|assert\|scenario` | the table above (from the parser's own table) |
| `assert_health WHO OP V`, `assert_mana WHO OP V` | V = number or `max` (max_health / mana_total); OP = `== = != < <= > >=` |
| `assert_count CLASS [TYPE] OP N` | live Things: class (and type), not marked for deletion, creatures / players with health >= 0, body segments counted with their head |
| `assert_alive WHO`, `assert_dead WHO` | exists with health >= 0 / gone or health < 0 |
| `assert_pos WHO X Y [R]` | within R cells (default 1, Chebyshev, torus) of cell X,Y |
| `assert_status WHO won\|lost\|running` | PlayerRec.status (2 won, (status & 6) == 4 lost, neither) |
| `assert_god WHO on\|off` | the god flag |

In the console an assertion prints `ok: ...` or `FAILED: ...`; in a scenario it counts.

## 4. Scenario files (`*.scn`)

The `reference_player_test` script grammar, extended:

```
# comment
name TEXT                          title for the reports (failures are reported as file.scn:line)
level L                            campaign level L (0..69) from its first tick
rts seed S [bots B] [size 256] [map 50..69] [humans H] [reseed]
                                   a Conquest mode run; debug commands are always on in it
stop T                             end after tick T (default: when the last line ran)
T <command>                        in tick T
T-T2 <command>                     a game command every tick T..T2; an input verb held over the range
<command>                          untimed: at the cursor (starts at 1); `wait N` moves the cursor on by N
wait N
```

Every command of section 3 may appear (except `run`), plus the local player's **input verbs** of
reference_player_test (world units, they replace the human's input while the scenario runs): `steer X Y`, `keys B`
(1 faster, 2 slower, 4 / 8 strafe, 0x10 / 0x20 cast), `cast left|right`, `left S` / `right S` (the book slot holding
spell id S into that hand), `respawn` (only while dead and the castle stands), `rebuild`, `face X Y [Z]`, `fly X Y`,
`faceth S`, `face_class C TYPE [D]`. (`cmd`, `cheat`, `book`, `close` are queued packets here.)

**Timing.** Ticks count from the scenario start; with a `level` / `rts` header they are `PlayerRec.tick` (tick 1 = the
level's first tick; it carries the join packets, so a packet for tick 1 runs in tick 2). A game command of tick T is
queued before tick T and runs in tick T, or later while the slot is busy (one packet per tick: commands of the same
tick run in consecutive ticks, in file order). An assertion / query / host command of tick T sees the state **after**
tick T, and waits until every packet of an earlier line (tick, then file order) has run - so

```
spawn dragon 40 40 x2
spawn wyvern 60 60 x3
wait 2
assert_count dragon == 2
```

is safe. An assertion still pending at `stop` fails as `NOT REACHED`. A failure names the file and line:
`spawn_kill.scn:14: FAILED: assert_count dragon == 2 - count Creature Dragon (5,0) = 1 (== 2)`.

Runner API (`scenario.h`): `scenario_load(path, &s)` / `scenario_parse(text, name, &s)`; `ScenarioRunner::start(s)`;
`step(tick)` after every tick (and once with 0 before the first) -> `ScenarioStep` {packets queued, host commands,
failures, output, done}; when `has_input()`, the host installs `scenario_local_input` as
`g_hook_player_local_input`. The runner queues its packets itself (`debug_cmd_queue`); the host pumps before each
tick (`debug_cmd_pump`).

The six example files (`src/tests/scenarios/`): `spawn_kill.scn` (spawn dragons / wyverns / bees, kill by type, by
slot, all creatures, counts), `teleport.scn` (cells, world units with z, `where`), `god.scn` (50000 damage survived
with god after the spawn's 100-tick invulnerability ran out; the same kills without), `give_mana.scn` (negative /
capped mana, cheat-2 ball, all spells), `input.scn` (fly towards a point, steer, teleport back), `rts_debug.scn` (a
seed-1 3-bot Conquest run: god, spawn, teleport, kill a computer wizard, teleport / drain another player with `@2`).

## 5. The console (mcport/console.*) and headless use

- **Key: ` (Backquote)** opens / closes it (not a key the game reads: `g_input_bindings`, main.cpp `set1_scancode`;
  F10 is the game's 3D-mode key, so it is not used). Only in a level, the viewer or a movie. While it is open it
  takes every key before the game (F12 quit still works), the game's keys / buttons are let go and the pointer
  centred; the level keeps running (time control: `time pause`).
- **Esc** closes; **Enter** runs the line; **Left / Right / Home / End / Backspace / Delete** edit; **Up / Down**
  history (kept in `<save dir>/console_history.txt`, the last 500 lines, a line repeated right away kept once);
  **Page Up / Down** scroll back; **Tab** completes the command name; **Ctrl+U / Ctrl+C** clear the line,
  **Ctrl+L** the scrollback. Text from scancodes + Shift, US layout (`console_key_ascii`), no SDL text input.
- Drawn after the HUD (and over the pause-menu layer's position) into the 640-wide virtual screen with the HUD font
  (`ui_set_font(1)`, restored after), the top half shaded (`ui_shade_rect` level 0x38), the input line at its
  bottom, the scrollback above it (wrapped at the panel width). The scrollback is D's log (`mclog_since`), so every
  port message (saves, screenshots, dumps, time control, applied packets) appears in it; the console's own output
  goes through `mclog` too (and so to stdout and `mcport.log`). The HUD font has no `| \ ` ~ { }` glyphs: they are
  drawn as `/ / ' - ( )`.
- **stdin** (headless): `--console-stdin` or `MC_CONSOLE_STDIN=1`: a thread reads lines, each frame runs the lines
  that arrived (between ticks) exactly like typed ones; output on stdout through mclog.
- **`--scenario FILE`** or `MC_SCENARIO=FILE`: with no mode on the command line the file's header chooses the run
  (`level L` = `play L`, `rts ...` = `rts` with those parameters); with a mode (`play 3 --scenario f`,
  `rts --seed 5 --scenario f`) the header is ignored. The scenario starts with the level's first tick; when it
  ends mcport quits, exit code 1 if an assertion failed (0 otherwise). Example:

```
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy MC_SAVE_DIR=<dir> MC_TICK_HZ=1000 \
    mcport.exe <game> --scenario src/tests/scenarios/spawn_kill.scn          # exit 0, one line per assertion
printf 'god on\nspawn dragon x2\nwhere me\n' | mcport.exe <game> rts --seed 1 --bots 3 --console-stdin
```

## 6. Requested shared-file changes (exact code)

### 6.1 player.h / player.cpp - the per-tick debug hook (REQUIRED for god mode)

Without it god mode works only where a host calls `debug_cmd_tick()` itself (scenario_test / console_test do, so
they pass before and after the change; mcport does not: verified - the wizard dies with god on). With it,
`debug_cmd_register()` installs the hook automatically (`#ifdef MC_HAVE_DEBUG_TICK_HOOK` in debug_cmd.cpp; the
integrator may drop the `#ifdef` afterwards).

`src/mcengine/player.h`, after `extern void (*g_hook_mode_tick)();`:
```cpp
// Port debug commands (round 10 task B, debug_cmd.h): once per tick after the mode tick - god mode keeps the
// wizard's health / mana full (debug_cmd_tick). Null = nothing (the original).
extern void (*g_hook_debug_tick)();
#define MC_HAVE_DEBUG_TICK_HOOK 1
```
`src/mcengine/player.cpp`, after `void (*g_hook_port_command)(int, Thing *, CmdPacket *) = nullptr;`:
```cpp
void (*g_hook_debug_tick)() = nullptr;
```
and in `game_tick_sim`, right after the line `if (g_hook_mode_tick) g_hook_mode_tick();` (before D's
`tick_profile_mark(tp, TP_MODE, ...)` if D wants it counted in the mode phase, else after it):
```cpp
    if (g_hook_debug_tick) g_hook_debug_tick();                          // port: debug commands (god mode, debug_cmd.h)
```
Tested in a copy of today's tree: all six scenarios pass in mcport (Release) with the user's playing settings
(extended pool 8192, possession 130 %, composed): `god.scn` 8/8, `rts_debug.scn` 13/13 (god keeps the wizard alive
for 280 ticks among three bots and a dragon). Null by default: the references are unaffected.

### 6.2 Optional

- `src/CMakeLists.txt`: add `mcengine/scenario.cpp` to `MC_SIM_ALL` so other tests can run scenario files
  (scenario_test lists it explicitly today, with `mcengine/mode_level.cpp`).
- `src/mcengine/mc_types.h`: name the god byte: `uint8_t pad3ae[2];` -> `uint8_t debug_flags; uint8_t pad3af;
  // 0x3ae port-only: debug_cmd.h DEBUG_PLAYER_GOD (zero in every original dump)` and in debug_cmd.h
  `return rec.blk.debug_flags;`.
- `config.h` / `config.cpp` / main.cpp, if the console key should be configurable: `PlatformOptions`:
  `KeyChord console;  // round 10: the debug console (Backquote)`, constructor `console.scancode = SDL_SCANCODE_GRAVE;`,
  `k_desc[]` line after `menu`:
  `{"keys", "console", K_KEY, PO(console), 0, 0, nullptr, "the debug console (Backquote; 'none' = no console)"},`
  and in main.cpp after `s_console.init(...)`: `s_console.toggle_key = s_opts.console;`.

No change to net.cpp is needed (the god byte is in the hashed P block; the mode flag in the hashed mode block).

## 7. main.cpp integration (task D) - exact code

Tested as a patch of D's main.cpp of 2026-10-08 08:30 (which already has C's includes, D's `debug_cmd_pump` in
`sim_tick`, `time_command`, `dcam_*`, `request_screenshot`) in a copy of the tree: compiles with zero warnings, all
scenarios pass through `--scenario`, the stdin commands and every host command were exercised. Unified diff
(context = D's current lines):

```diff
@@ includes
 #include "debug_cmd.h"
+#include "console.h"
@@ after `static TimeControl s_time_neutral; ...`
+// Round 10 (task B, docs/analysis/port_console.md): the debug console (` Backquote), --console-stdin / --scenario.
+static Console s_console;
+static ConsoleOptions s_con_opts;
@@ sim_tick, after D's debug_cmd_take_messages line
     for (const std::string &m : debug_cmd_take_messages()) mclog(MCLOG_INFO, "%s", m.c_str());   // applied debug packets
+    s_console.after_tick();                   // round 10 (task B): the running scenario's step (assertions, host commands)
@@ main(), before `ReplayRunArgs replay;` (the options must leave argv before config_load)
+    // round 10 task B: --console-stdin / MC_CONSOLE_STDIN=1, --scenario FILE / MC_SCENARIO=FILE (console.h)
+    s_con_opts = console_parse_args(&argc, argv);
     ReplayRunArgs replay;
@@ after `s_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;`
+    s_console.init(s_save_dir);                                 // history: <save dir>/console_history.txt
+    if (s_con_opts.stdin_reader) s_console.start_stdin_reader();
+    // A scenario file given at start: its header (level L / rts ...) chooses the run unless a mode is on the
+    // command line; it starts with the level, mcport quits when it ends (exit code 1 = an assertion failed).
+    Scenario scn_hdr;
+    if (!s_con_opts.scenario.empty() && !scenario_load(s_con_opts.scenario, &scn_hdr)) {
+        mclog(MCLOG_ERROR, "scenario: %s", scn_hdr.error.c_str());
+        return 1;
+    }
+    const bool scn_mode = !s_con_opts.scenario.empty() && (argc <= 2);   // no mode on the command line
+    if (scn_mode && !scn_hdr.rts && scn_hdr.level < 0) {
+        mclog(MCLOG_ERROR, "scenario %s: no `level L` / `rts ...` header and no mode on the command line", scn_hdr.name.c_str());
+        return 1;
+    }
@@ the mode flags
-    const bool play = std::strcmp(mode, "play") == 0;
+    const bool play = std::strcmp(mode, "play") == 0 || (scn_mode && !scn_hdr.rts);
-    const bool rts = std::strcmp(mode, "rts") == 0;          // `rts [--seed S] [--size N] [--bots B]` (rts_run.h)
+    const bool rts = std::strcmp(mode, "rts") == 0 || (scn_mode && scn_hdr.rts);   // `rts [--seed S] [--size N] [--bots B]` (rts_run.h)
-    int level = play ? (argc > 3 ? std::atoi(argv[3]) : 0) : viewer ? std::atoi(mode) : 0;
+    int level = play ? (scn_mode ? scn_hdr.level : argc > 3 ? std::atoi(argv[3]) : 0) : viewer ? std::atoi(mode) : 0;
@@ the rts branch
-            if (!rts_parse_args(argc, argv, 3, &mp, &err)) { mclog(MCLOG_ERROR, "rts: %s", err.c_str()); return 1; }
+            if (scn_mode) mp = scn_hdr.rts_params;                // round 10: the scenario's `rts ...` header
+            else if (!rts_parse_args(argc, argv, 3, &mp, &err)) { mclog(MCLOG_ERROR, "rts: %s", err.c_str()); return 1; }
@@ right before `Camera cam = demo ? player_camera(...) : engine_default_camera();`
+    // round 10 (task B): the --scenario file runs from the level's first tick
+    if (!s_con_opts.scenario.empty()) {
+        std::string err;
+        if (run != RUN_LEVEL) { mclog(MCLOG_ERROR, "scenario: needs a level (play / rts / a level / rts header)"); return 1; }
+        if (!s_console.run_file(s_con_opts.scenario, true, &err)) { mclog(MCLOG_ERROR, "scenario: %s", err.c_str()); return 1; }
+    }
@@ the key loop: first thing inside `for (int i = 0; i < in.key_event_count; i++) {`
+                // round 10 (task B): the open console takes every key (quit_now still works); its key closes it
+                if (s_console.is_open()) {
+                    const int sc = in.key_events[i].scancode;
+                    in.key_events[i].scancode = 0;
+                    if (!in.key_events[i].down) continue;
+                    int slot = 0;
+                    if (config_key_action(s_opts, sc, kmods, &slot) == PORT_KEY_QUIT) { port_act = PORT_KEY_QUIT; continue; }
+                    if (s_console.is_toggle(sc, kmods)) s_console.close();
+                    else s_console.key(sc, kmods);
+                    continue;
+                }
                 if (s_menu.is_open()) {
@@ the key loop: right before `const DebugAction da = debug_key_action(...)` (after C's inspect_tool_key block)
+                // round 10 (task B): the console key (Backquote; not a game key) opens the console in a level / the
+                // viewer / a movie; the game's keys and buttons are let go (the level keeps running)
+                if (s_console.is_toggle(in.key_events[i].scancode, kmods) && run != RUN_FE) {
+                    in.key_events[i].scancode = 0;
+                    s_console.open();
+                    release_game_input();
+                    prev_l = prev_r = release_l = release_r = false;
+                    if (run == RUN_LEVEL) { s_level_mx = 320; s_level_my = virtual_h() / 2; input_mouse_move(s_level_mx, s_level_my); }
+                    continue;
+                }
@@ after `if (port_act == PORT_KEY_QUIT) break;`, before `for (const DebugAction a : debug_acts) {`
+        // Round 10 (task B): lines from stdin, then the console's host commands (time, shot, save, load, cam, ...).
+        s_console.poll_stdin();
+        for (const HostCmd &h : s_console.take_host_commands()) {
+            switch (h.op) {
+            case HostOp::TIME:
+                if (h.sub == "step") time_command(DA_STEP, h.n);
+                else if (h.sub == "normal") time_command(DA_NORMAL);
+                else if (h.sub == "toggle" || (h.sub == "pause") != s_time.paused()) time_command(DA_PAUSE);
+                else if (h.sub == "speed") {
+                    if (g_cfg->flags & 0x10) mclog(MCLOG_WARN, "time control: not in a network game");
+                    else if (!s_time.set_speed_text(h.text.c_str())) mclog(MCLOG_WARN, "time speed %s: not a speed (max, 4, 1/2, ...)", h.text.c_str());
+                    else mclog(MCLOG_INFO, "time: speed %s", TimeControl::speed_name(s_time.speed16()).c_str());
+                }
+                break;
+            case HostOp::SHOT: request_screenshot(h.text); break;
+            case HostOp::SAVE:
+                if (run != RUN_LEVEL) { mclog(MCLOG_WARN, "save: no level"); break; }
+                if (savestate_save(h.n, nullptr)) notice("state saved to slot %d (level %d)", h.n, level);
+                else notice("slot %d not saved: %s", h.n, savestate_error());
+                break;
+            case HostOp::LOAD: port_act = PORT_KEY_LOAD; port_slot = h.n; break;   // the save / load block below
+            case HostOp::SYNC: {
+                const NetSyncStats st = net_sync_stats();
+                mclog(MCLOG_INFO, "sync: %s, %u exchanges, %u checks, %u mismatches (first at exchange %d, first part %s)",
+                      (g_cfg->flags & 0x10) ? "network game" : "not a network game", (unsigned)st.exchanges, (unsigned)st.checks,
+                      (unsigned)st.mismatches, (int)st.first_mismatch, st.first_part >= 0 ? net_checksum_part_name(st.first_part) : "-");
+                break;
+            }
+            case HostOp::DUMP: dump_now(s_ticks_run); break;                  // (task A) prints its own line
+            case HostOp::CAM:
+                if (run != RUN_LEVEL) { mclog(MCLOG_WARN, "cam: in a level only"); break; }
+                if (h.sub == "free") dcam_enable(true);
+                else if (h.sub == "off") dcam_enable(false);
+                else if (h.sub == "follow") {
+                    const int slot = h.n > 0 ? h.n : debug_inspect_slot();
+                    if (slot > 0) { dcam_follow(slot); debug_inspect_set(slot); }
+                    else mclog(MCLOG_WARN, "cam follow: which Thing? (cam follow #SLOT, or inspect one first)");
+                }
+                else if (h.sub == "to") dcam_to_cell(h.x >> 8, h.y >> 8);
+                break;
+            case HostOp::OVERLAY:                                         // (task C's code, port_inspect.md)
+                if (h.sub.empty() || h.sub == "list") mclog(MCLOG_INFO, "%s", debug_overlay_list().c_str());
+                else if (!(h.on < 0 ? debug_overlay_toggle(h.sub.c_str()) : debug_overlay_set(h.sub.c_str(), h.on != 0)))
+                    mclog(MCLOG_WARN, "overlay: unknown '%s' (overlay list)", h.sub.c_str());
+                break;
+            case HostOp::INSPECT:                                         // (task C's code, port_inspect.md)
+                if (h.sub == "off") { debug_inspect_set(-1); debug_inspect_set_cell(-1, -1); }
+                else if (h.sub == "cursor") inspect_tool_set_cursor_mode(plat, true);
+                else {
+                    debug_inspect_set(h.n);
+                    std::vector<std::string> lines;
+                    debug_inspect_lines(h.n, &lines);
+                    for (const std::string &l : lines) mclog(MCLOG_INFO, "%s", l.c_str());
+                }
+                break;
+            case HostOp::QUIT: in.quit = true; break;
+            default: break;
+            }
+        }
         for (const DebugAction a : debug_acts) {
@@ the device-feeding block
-        if (devices && !s_menu.is_open() && !dcam_input) {
+        if (devices && !s_menu.is_open() && !dcam_input && !s_console.is_open()) {   // round 10: the console takes the keys
@@ both draw_view_frame lambdas (game view and free camera), right after `draw_pace_overlay();`
                     draw_pace_overlay();
+                    ui_set_target(frame());
+                    s_console.draw(virtual_h());                  // round 10 (task B): the debug console
@@ the end of main
-    return 0;
+    return s_console.exit_code();                               // round 10: 1 when a --scenario assertion failed
```

(`dcam_follow` / `dcam_to_cell` lose their `[[maybe_unused]]` once this is in. If the 6.2 config key lands:
`s_console.toggle_key = s_opts.console;` after `s_console.init`.)

## 8. Verification

- **scenario_test** (Debug and Release; ctest, 2.6 s Release): 6 files, 50 assertions, all pass; each scenario run
  twice in one process with identical per-tick `net_state_checksum` (165 / 300 / 160 / 40 / 7 / 5 ticks); the
  deliberate failure is reported as `deliberate.scn:3: FAILED: assert_count creature dragon == 12345 - count
  Creature Dragon (5,0) = 0 (== 12345)`; a parse error as `bad.scn:3: unknown class / type - usage: ...`; an
  assertion behind `stop` as `NOT REACHED`. Record + replay (demo.cpp recorder, the original's mvi format): god.scn
  164 recorded ticks, input.scn 159, spawn_kill.scn 6 - the playback equals the record run's state on every tick
  (reference_player_test's normalised hash), with 5 / 1 / 6 debug packets applied again from the movie.
- **console_test** (ctest): 41 packets byte-exact (38 command lines and 3 builder calls), 40 malformed lines rejected
  with their usage, every command in `help` and `help <cmd>`, host commands / assertions / queries / input verbs
  classified, the scenario grammar (header, cursor, `wait`, ranges, `packets_before`), key-to-ASCII, line editing,
  Tab completion, Up / Down history, the history file round trip (and trimming a 700-line file to 500); in level 1:
  typed commands queue, wait out the join tick, run in the next ticks, their messages reach the scrollback,
  assertions / `where` / `players` / `find`, `run` of a scenario (header ignored, host command `shot` handed on),
  the console drawn over the view (console_open.ppm, looked at: 320x200 and 640x480 modes).
- **Gates** (Release, build_B, after the last engine change): reference_test (0 divergences over 1396 ticks),
  reference_levels, reference_gen, reference_player, reference_player_test, render_reference_test,
  render_reference2_test, render_reference_options, render_reference_hud, render_reference_fe_test (33 / 33
  pixel-identical), net_test, config_test, game_menu_test: all pass. Default settings never send a packet >= 0x40,
  so `debug_cmd_apply` never runs in a reference.
- **mcport** (Release, a copy of the tree with sections 6.1 + 7 applied, `SDL_VIDEODRIVER=dummy`, the user's
  playing settings): `--scenario` for all six files - exit 0, 50 / 50 assertions; a failing one exits 1;
  `--console-stdin` with `god on / spawn dragon x2 / where me / players / assert_god / shot / kill wizard 9 / bogus`
  and `overlay list / overlay grid on / inspect #890 / cam follow / sync / dump / time step 5 / time speed 4 / save 3
  / quit` - every line answered, the screenshot written, the dump line printed; the console opened by an injected
  Backquote (`MC_TEST_INPUT=40@k53`) and captured with `MC_SHOT` in `rts --seed 1 --bots 3` (composed 1280x960).
- Zero warnings (/W4) in debug_cmd.cpp, scenario.cpp, console.cpp and both tests, Debug and Release.

## 9. Deviations, findings, gaps

1. **The AI wizard ignores `health = -1` while invulnerable** (`ai_wizard.cpp` 938-952: invulnerability -> the
   pending damage is discarded, then `health += health_regen` before the death check; inside its castle
   `invuln_timer` is set to 2 every tick). The original's cheat 3 ("destroy all players") only writes the health, so
   in the original too it does nothing to a computer wizard in its castle or within 100 ticks of a spawn (faithful;
   observed in the port: health -1 -> 19 a tick later). `kill wizard` therefore sets health -1 **and state 2**
   (what the update itself does on death, minus the death sound for the flyer).
2. `thing_slot_generation` cannot name Things in packets (section 2) - class / type checks instead.
3. **Running a level twice in one process**: the two Thing stacks keep stale entries above their tops from the
   previous run (the original's image does the same), and `net_state_checksum`'s `pool` part hashes all 1000
   entries, so the second run's checksums differ from tick 1 although the game is identical. scenario_test clears
   `free_list` / `active_list` before every run; A's / E's "twice in one process" tests need the same (or a fresh
   process).
4. One packet per player per tick (the shared contract): N commands of one tick take N ticks. Fine for debugging;
   round 15's order blob (movie v3 records) can carry batches.
5. `give mana` to the castle clamps to the castle's `mana_total`; a castle without mana_total (0) is not clamped
   above. The cheat-2 ball path is the original's.
6. `spells` does not unlock the level's switches (cheat 1 does).
7. The god flag lives in two places (mode block / P+0x3ae) by design of the briefing; a savestate of a campaign level
   carries it, one of a mode run carries it in the MODE chunk.
8. Scenario input verbs use floating point (`face_point`, like reference_player_test): it is input generation (a
   player's hand), recorded in the packet, never inside the tick.
9. The console's scrollback is the mclog ring buffer (512 lines) plus its own 1000-line copy; lines logged while
   nothing pulls (no frame drawn) are picked up at the next `after_tick` / draw.

## 10. Next round

- Round 12 / 15: an order blob per tick (movie v3 variable records) so a scenario / the RTS UI can send several
  orders per tick; then `spawn` with an owner, `order` lines in scenarios, cursor spawn (C's
  `inspect_tool_cursor_cell` -> `debug_cmd_spawn(..., at_cell)`).
- Make the slot generation simulated state if packets need to name Things across slot reuse.
- Round 11 (32-bit world): teleport / spawn / assert_pos coordinates through the world descriptor (int32, cells
  beyond 255); the packet's 16-bit x / y then need the order blob.
- A `[keys] console` ini key (6.2) and, with round 16's native overlay, a sharper console font.
