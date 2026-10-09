# Phase 4 roadmap - "Conquest" mode (working title): PvP RTS / RPG hybrid on a generated world

Written 2026-10-07 from five research reports (read them for the evidence behind every claim here):

| report | what it covers |
|---|---|
| `docs/analysis/phase4_research_games.md` | Sacrifice, Warcraft III, Dungeon Keeper, Diablo II / PoE, ten other hybrids; control schemes, lockstep netcode, bot architectures, fair procgen; ranked design lessons |
| `docs/analysis/phase4_research_mc2.md` | Magic Carpet 2: decomp repo (remc2 / magic-carpet-2-hd, GPL), 26 spells, 30 creatures, caves, deformation, the GOG CD's data files (extracted to `extracted/mc2_cd`), TCRF notes |
| `docs/analysis/phase4_code_world.md` | what hard-wires 256x256 (430 sites), the u16 position wall, terrain generator, spatial index, renderer projection, overflow risks, the world-module plan (B1-B9, 35-50 days) |
| `docs/analysis/phase4_code_gameplay.md` | Thing dispatch and free ids, creature roster and state machine, wizard AI, castles and villages, the 24 spells, damage model and the damage hook, player record, feature -> reuse table, hard-coded traps |
| `docs/analysis/phase4_code_platform.md` | lockstep model and its limits, determinism rules, modes / rules plumbing, front end (urn sprite, hit test), HUD / projection, input, the 43 tests and headless switches, the proposed debug suite, compose layers |

Everything below is a plan, not a status. Rounds continue the numbering of Phases 2-3 (the last integrated round
was 9). The round style stays the one that worked: integrator writes a briefing (`docs/port/BRIEFING_roundN.md`)
and placeholders, 4-6 agents own named files and build dirs, every agent reports to `docs/analysis/`.

---

## 1. The mode in one page

**Pitch**: Magic Carpet with armies. Up to 8 wizards (humans and named bots, teams allowed) on a generated torus
world 4x the linear size of a campaign level (1024x1024 cells = 16x the area; 512 and 768 for shorter matches).
Each wizard flies first person as in the base game, grows a castle, founds towns whose villagers gather mana,
food, lumber and ore, summons monsters from a lair and sends them against rivals, hunts named elite monsters
for spell drops, and can switch at any time to an RTS overview camera to command the army. Matches last 30-45
minutes and end by castle desecration, mana dominance or elimination. Offline against bots, online in
lockstep.

**Design pillars** (from the ranked lessons in `phase4_research_games.md` section 7, mapped to the engine):

1. **Death is cheap, the castle is the objective.** A killed wizard respawns at the castle (10 s + 5 s per
   level); the base game's "no castle = level lost" rule becomes the elimination rule. Elimination of a wizard
   with a castle = *desecration*: an enemy wizard or unit channels 20-30 s inside the courtyard, interruptible
   by killing or possessing the channeler (Sacrifice's altar; the castle's existing `castle_crush_wizards`
   courtyard test tells us who is inside).
2. **Essence economy, not a pop cap.** Summons cost essence (tier 1-3 = 1-3, elites 5); essence returns to the
   summoner when the monster dies unless an enemy *converts* the corpse within 20 s x tier. Castle mana pays
   the upkeep trickle above two thresholds (WC3 upkeep) so armies stay small and the wizard stays the star.
3. **Rarity-tiered wild monsters with visible affixes and guaranteed drops** (Diablo II). Normal / Champion /
   Named Elite / Boss, colour-coded plates, 2-3 affixes built from MC's own effects, drop tables with a pity
   counter. This is the "come back tomorrow" hook and the cheapest pillar to build.
4. **Territory = mana springs + towns.** Springs scattered by the generator are claimed with a shrine (Sacrifice
   manaliths); towns extend an influence radius that bounds building and feeds the castle. "Hold N springs for
   3 minutes" is the alternate win condition and the bots' waypoint system.
5. **Thin, autonomous RTS layer.** Villagers self-assign, buildings go on fixed plots around the town centre,
   monsters guard / retaliate / retreat on their own. First person has exactly four orders (rally to me,
   attack-move to reticle, hold here, return to lair) plus a type filter; the overview adds groups, drag
   select and rally flags. Every Sacrifice / SpellForce complaint was attention load; the player is flying a
   carpet.
6. **Buildings as recruitment filter, levels earned in combat** (Dungeon Keeper). A hive unlocks bees, a
   quarry crabs / trolls, a swamp pool worms, a spire dragons; monsters above level 4 only level by fighting
   and become veterans with names.
7. **Scheduled global pressure.** Day / night (wild monsters sleep by day, raids are blind at night) and an
   optional escalating *blight* that revokes outer springs for battle-royale style matches, so matches end.
8. **Pre-match loadout, in-match discovery.** Bring 6 spells; the rest drop from elites, biased toward what you
   carry. Spell levels (MC2 has three per spell; MC1 has none) are a side struct scaling damage / duration.
9. **Bots with personality**, difficulty as resource multiplier + reaction interval hidden in fiction.
10. **Replays and desync dumps from day one.**

**Camera and control**: first person is the base game. The overview is a new oblique renderer over
`render_ext_raster.cpp`'s display list (the existing projection cannot pitch; the radar / map painter is the
minimap seed). One key toggles (Tab is taken by the book: default `M` / controller Back). Spellcasting in
the overview is limited to global spells; fighting happens in first person or through units.

**Numbers to design around** (`phase4_code_world.md` section 5): 1 cell = 256 units = about half a metre at the
game's scale; wizard top speed about 7.8 cells/s, so a 1024 world is ~130 s across (teleport and MC2's Castle
Port matter); awake radius 24 cells; pool hard ceiling 32768 Things (i16 indices) - campaign density on
1024^2 would peak ~40k, so the placer must thin wild density with distance from spawns; 8 players maximum
(records, sprites, colours are baked in - teams are cheap, a 9th wizard is not).

---

## 2. Engineering decisions (apply to every round)

1. **Faithful by default.** The mode is a `GameplayRules` flag (`settings.h`) carried by `DemoExtRules`,
   `NetGameRules` and the `RULE` savestate chunk. All 49 ctest gates (per-tick references, pixel references,
   33 front-end screens) must stay identical with default settings after every round.
2. **New simulated state lives outside `GameState`** in a mode block (`g_mode` / per-player `ModePlayer`),
   hashed by `net_state_checksum`, saved in a new savestate chunk, carried by movie format v3 and the `gax`
   snapshot. `GameState` is a fixed 0x38d03 image and stays one.
3. **New Thing types go through a port-only extension dispatch table** consulted only when the faithful record
   is missing (`phase4_code_gameplay.md` 1.2): class 4 for buildings / resource nodes / markers, class 5
   types 17-19 and an extension keyed on `type` for units, creature states 102-119, effect gaps 39 / 47 / 49 /
   50, projectile types 6 / 15 / 19. Per-unit sub-state in a side array indexed by slot, never a new u8 state.
4. **32-bit world** (`phase4_code_world.md` section 7, approach B): `Pos` / `Thing.x/.y` become int32, a world
   descriptor replaces every `mc_cell` / `cell_xy` / `(uint8_t)` neighbour, `world_diff` replaces the 106
   `(int16_t)` torus differences, squared distances go int64, `math_atan2` gets an int32 octant reduction;
   GameState is serialised to the original layout for movies / saves / references / net checksum. 256 through
   the accessors is bit-identical (that is the proof the references give us).
5. **Orders are packets.** Every state-changing action - RTS order, build, debug spawn, god mode - is a command
   packet (cmd ids 0x20.. in the 10-byte packet while it fits; a second length-prefixed per-player order blob
   exchanged next to `net_exchange_frame` when it does not), so it is recorded, replayed and lockstep-safe.
   Orders reference Things by slot + `thing_slot_generation`.
6. **Determinism rules** (`phase4_code_platform.md` section 2): integers only, index-order iteration, LCGs seeded
   from the match seed (the mode gets its own `ModeRng`; the AI's CRT `rand()` seed is hashed or replaced),
   nothing reads the local player, the camera, settings or real time. The creature awake gate becomes
   "near any wizard" (deterministic) under the mode flag - today it reads the local player and is switched off
   in network games, which would leave every monster asleep.
7. **Tests per round**: a record + replay determinism test of the new command stream (pattern:
   `reference_player_test`, `config_test rules_movie`), the two-process lockstep test (`net_test` parts 2-3)
   on the mode, headless scenario files, and screenshots looked at. Bot-vs-bot headless matches are the
   balance harness.
8. **Licensing**: remc2 / MC2-HD and mgcarpet stay reference only (GPL). MC2 *data* is loaded from the user's own
   GOG copy via `MC_MC2_DIR` (never redistributed, never modified in place); our loaders are extended (only
   `mctools.level` / `mcdata/level.c` need an MC2 variant - sprites, tmaps, palettes, RNC are identical).
9. **GPU renderer**: not needed until late - the CPU `render_ext` does 4K at dd 127 in 6 ms on 24 threads and the
   Release build plays at 165-190 fps at dd 127 (2026-10-08). The overview renderer reuses its rasteriser and
   thread pool. A GPU path is round 22, the last round of Phase 4 (user, 2026-10-08).
   What the mode does need is a **native-resolution RGBA overlay layer** in `ComposeOutput` with a real font
   (health plates, damage numbers, RTS UI) - the 640-wide virtual 2D layer is fine for a first version of plates
   and numbers, not for the RTS UI.

---

## 3. Rounds

Effort is one-engineer days from the reports; rounds run 4-6 agents in parallel so calendar time is shorter.
Each round lists its gate (what must be demonstrable before the next starts).

### Round 10 - debug and testing suite, mode skeleton (first, per the user's requirement) - DONE 2026-10-08

Goal: the user and Claude can start, drive, observe and assert on a running game headless or on screen, and
the plumbing every later round needs exists. Items from `phase4_code_platform.md` section 7. Briefing
`docs/port/BRIEFING_round10.md`; reports `docs/analysis/port_{mode,console,inspect,timectl,desync}.md` (tasks A-E).
The user's control reference is the README ("Where things stand").

- [x] **Mode skeleton** (task A + integrator): `GameplayRules.mode` (0 = original, 1 = conquest; carried by
      `NetGameRules.mode`, `RULE` chunk v2, movie v3), `ModeState g_mode` outside GameState (`mode.h`: params seed /
      size / bots / humans / map / flags, tick, `ModeRng`, per-player flags) + `MODE` savestate chunk + checksum part
      (only while a mode runs, so the original checksum is unchanged; also hashes the AI seed / terrain globals),
      `sim_load_level_data` + `g_hook_level_source` (level start from memory, restarts work), `mcport <dir> rts
      --seed S --bots B [--map 50..69] [--reseed] [--ticks N]` (256: a multiplayer map chosen from the seed; the
      3 AI wizards build, collect and fight), `MC_DUMP_EVERY` / `--dump-every` JSON dumps (players, census per
      class, pool, mode block, checksum + parts, tick profile) with one stdout line per dump, rts save states load.
- [x] **Console** (task B, `mcport/console.*`, Backquote): line editor, history file, Tab completion, scrollback
      (shows `mcport.log`), parser shared with scenario files and `--console-stdin`; debug packets 0x40..0x49
      (teleport, spawn, give mana / spells, god, kill, claim, heal, spells, win / lose, damage) applied in the tick
      (`debug_cmd.*`, recorded and replayed, refused in network games); host commands (time, shot, save / load,
      sync, dump, cam, overlay, inspect, run, quit), queries (players, where, find, count, queue), help.
- [x] **Time control** (task D, `mcport/timectl.*`): Pause, End / Shift+End step 1 / 10, PageUp / PageDown x1/16 ..
      x64 / max, Insert x1, `MC_TIME_SPEED`; scheduling only (same ticks, same checksums); not in network games.
- [x] **Debug camera** (task D, `debug_camera.h`, render-only): Delete = free camera (viewer keys, R / F up / down),
      Shift+Delete teleports the wizard there (debug packet), `cam free|follow|to|off`.
- [x] **Projection export** (task C, render.h): `render_project_world` for the faithful and the extended renderer,
      per-frame Thing anchors, `render_pick_ground` / `render_pick_thing`, display-resolution pointer (platform),
      compose mappings. Faithful pixels unchanged (`project_test`).
- [x] **Thing inspector** (task C, `debug_overlay.*`, `mcport/inspect_tool.*`): Home / middle mouse cursor mode
      (spell-book pointer drawn), click to inspect, panel with Thing / PlayerBlock (AI mode) / MoveDesc / cell list,
      keypad 7 / 9 previous / next, Backspace or a click on nothing closes; follow camera (keypad 8 / Ctrl+Home /
      `cam follow`) with mouse orbit, wheel zoom, right-drag orbit in cursor mode (play-test follow-ups 2026-10-08).
- [x] **Overlays** (task C; keypad 1-6, 0 off, `overlay`): grid, labels (slot / class / state / AI mode), anchors,
      occupancy (cell lists on the map screen), damage (health deltas), net (sync status + first differing part);
      tick cost per phase and per class on the F11 line (task D, `tick_profile.*`).
- [x] **Scenario files** (task B, `scenario.*`, `src/tests/scenarios/*.scn`): `level L | rts seed S bots B | stop T`
      headers, timed / `wait`-relative lines, every console command, input verbs of `reference_player_test`,
      `assert_health / mana / count / alive / dead / pos / status / god`, failures name file and line; mcport
      `--scenario file` (exit 1 on a failure) and console `run file`; `scenario_test` runs all six examples twice
      and through a movie record / replay. Not done: `order` (no orders exist before round 15).
- [x] **Desync tooling** (task E): `net_state_checksum` unchanged in value plus 35 named parts
      (`net_state_checksum_parts`), parts exchanged on the first `MC_NET_SYNC` mismatch, both peers dump
      `desync_<n>_p<p>.mcs`, `tools/reference/diff_state.py` (field tables parsed from mc_types.h / mode.h) and
      `tick_log_diff.py`, `MC_TICK_LOG_PARTS=1`, `--replay-check N | state:<file>` (`replay_check.*`).
- [x] **Log file** (task D, `mcport/mclog.*`): `<save dir>/mcport.log` (+ `mcport.1.log`), four levels, feeds the
      console scrollback; Ctrl+F11 / `shot [name]` PNG screenshots (`mcport/screenshot.*`, composed output, headless).
- [x] **Movie format v3** (task A, `demo.*`): mvx version 3 with a mode header (mode + `ModeParams`), per-tick
      `{u16 len, bytes}` order blobs (`g_hook_demo_order_out / _in`), mode block + AI / terrain globals in the `gax`
      snapshot; `demo_open` starts the mode run, `demo_close` stops it; v1 / v2 / original movies unchanged.
- [x] Gate: `scenario_test`, `console_test`, `rts_headless_test` (seed 1, 3 bots, 2000 ticks twice in one process
  identical, dump parses, save / load round trip), plus `movie_v3_test`, `project_test`, `overlay_test`,
  `timectl_test`, `mclog_test`, `desync_test`, `replay_check_test`; **59/59 ctest**, all 49 earlier gates identical
  (references 0 divergences).
- Integration fixes: the level reset zeroes both Thing stacks (`free_list` / `active_list`; stale entries made
  checksums depend on what the process played before - false desyncs at exchange 0); TCP tests listen on
  127.0.0.1 (`MC_NET_BIND`, no Windows Firewall prompt).
- [ ] Left open: the debug keys, the console key and `log.level` / `net.desync_dump` as `mcport.ini` keys (constants
      for now; the `k_desc` lines are in port_timectl.md / port_console.md / port_desync.md); a god-mode key (console
      `god` only); overlays cover only the 4:3 frame when composed widescreen (native RGBA overlay layer = round 16);
      the damage overlay diffs health itself until round 13's `g_hook_damage_applied`; the pause menu labels rts
      saves "level 257"; the headless human of an rts run has no input (no mode win rule before round 12); the free
      camera cannot look straight down (horizon-shift projection; true pitch = round 16's renderer).

### Round 11 - the big world (world module, 32-bit positions, generator on N)

Steps B1-B7 and B9 of `phase4_code_world.md` section 7; the RTS renderer (B8) moves to round 16.

**Split over three sessions** (briefing `docs/port/BRIEFING_round11.md`, branch `round11`).
**Session 1 DONE 2026-10-08** (65/65 ctest, every reference set byte-identical incl. movie 0 full, Hidden Worlds,
1995 controls, scripted players): `WCoord` int32 positions (wrap on store; exactly u16 at 256), original-layout
images (`mc_image.h`) for movies / saves / references / checksum, `world.h` (descriptor, pointer maps, accessors,
int64 distances, `math_atan2_w`, high halves of start positions / camera log, `world_widen_tiled`), and the call-site
sweep by five agents (reports `docs/analysis/port_world_{terrain,things,creatures,persist,render}.md`): terrain
passes / features / spatial / effects / creatures / AI quadrants / projectiles / player / debug packets (wide
teleport in byte 9) / sound / save-state `WRLD` chunk / movie v3 wide snapshots / checksum / renderers (`kWin =
min(N/2, 256)`, draw distance up to 255 at N >= 512) / HUD / map screen (window around the player) / debug overlays.
`mcport rts --size 512..4096` runs a **tiled** world (the template map repeated) until the generator lands; two
runs at 1024 give identical checksums. Sizes are powers of two only (768 dropped: masks everywhere).
**Session 2 DONE 2026-10-08** (66/66 ctest, references identical; report `docs/analysis/port_world_gen.md`):
`rts --size N` builds a generated, symmetric world with features, triggers and portals (B4 + B5 below).
**Session 3 DONE 2026-10-08 - ROUND 11 COMPLETE** (69/69 ctest, every reference set identical; reports
`docs/analysis/port_world_{session3,test,refwide}.md`): B6 wake near any wizard (offline and online mode runs);
`NetGameRules` carries the `ModeParams`, computer wizards in a network mode run, `mode_start_net_run`,
`mcport rts --net N`; `world_net_test` (two processes, 1024, 2 humans + 3 bots, 2000 ticks in sync); two checksum
fixes (combat-music timer, free-slot flag bit 0 - per-instance state); `world_test` (all 140 levels at 256 hash-identical
to the pre-round-11 code `bf77e98`, 512 / 2048 / 4096 probes, rts 1024 determinism); `MC_REF_WORLD` + ctest
`reference_world` (divergences explained by the old boundary); dd 127 / 255 at 1024 in `world_render_test`. Scaling
decisions (session3 report section 3): AI castle site = own seat, then 64-cell quadrants around the wizard; a
generated world gives every seat spells 0 / 3 / Castle, all allowed (template 59 left bots castle-less); the
original's half-world separation line removed for N > 256; perception / spell ranges and the nearest-anywhere
searches kept (bot hunt radius = round 17). **Gate met**: two headless `mcport rts --net 2 --seed 1 --size 1024
--bots 3` ran 4000 ticks with no desync and identical final checksums; the free camera wraps the world (Ctrl x4).
Follow-up after the user's play (same day, session3 report section 6): spell urns graded by distance, stone circles
(60 % with a trigger; lone stones rare), palm / oak woods (oak = Hidden Worlds' tree, port SpriteDesc 285 / port
sprite slot), taller peaks, Hidden Worlds' snow classification + snow tiles (render-only art overlay `world_art_apply`).
User note: bots felt passive (few spells, template aggression, distance) - round 17.
**Done (2026-10-09)**: burnt oaks - `scenery_tree_s1_update` maps the oak (SpriteDesc 285) to the burnt oak
`MC_SPRITE_DESC_OAK_BURNT` (286, the burnt palm 0xe2's extents); `world_art_apply` loads Hidden Worlds' burnt
broadleaf (tmaps1-0 chunk 299) as port sprite `MC_SPRITE_OAK_BURNT`, burnt palm without Hidden Worlds.

- [x] B1 world descriptor + accessors (`world.h`: `world_cell`, `world_cell_of`, `world_cell_step`, `world_cell_x/y`,
      `world_cx`, `world_signed` (= the original's `(int16_t)` at 256), `world_dist_sq` int64), replacing `mc_cell`,
      the `cell_xy` clones, `(uint8_t)` neighbour arithmetic and 0x10000 strides; maps are pointers (the static
      arrays at 256, a heap block otherwise, `world_set_size`); `WRLD` chunk in savestates, wide `gax` / `max` movie
      snapshots (session 1).
- [x] B2 int32 positions: `WCoord` for `Pos` / `Thing.x/.y` / `Thing.home` (wraps on every store); `start_pos` and
      `PosLogEntry` keep their u16 image fields plus high halves in `g_world_hi`; `ThingInit` stays u16 cells (fine
      to 65535); `math_atan2_w`; `pos_dist_sq_*` and the nearest searches int64 (session 1). Range constants kept
      as they are - the scaling decisions are listed under session 3.
- [x] B3 original-layout images (`mc_image.h`: `ThingImage` / `GameStateImage`, `state_to_image` / `_from_image`,
      `state_image_read` for the original's over-reads); movies, save states, checksum and `reference_test` go
      through them; every gate re-validated (session 1: 65/65, all reference sets identical).
- [x] B4 generator on N (session 2, `docs/analysis/port_world_gen.md`): int32 torus diamond-square with log2 N
      levels, the original's amplitudes up to step 128 and a capped "continent" amplitude above (`--continent`),
      symmetrised by a variance-corrected group average, sea level by percentile (`--water`, default 30 %), rivers
      scaled by area and sourced in orbits with a visited-list tracer, then the original's passes
      (`terrain_build_passes`, shared with `terrain_build`). `terrain_build` itself stays the 256 level builder.
- [x] B5 **seeded feature placer v1** (session 2, `worldgen.{h,cpp}`, `port_world_gen.md`): seats by symmetry orbit
      (auto: 1 none, 2 point, 3-4 rot4, 5-8 dihedral; `--symmetry`), plateaus, springs in bands, towns, graded lairs,
      wild creatures, groves, trigger areas (8 payload types, all fire for bots - switches now fire for computer
      wizards in a mode run) and linked portal pairs, as THING_INIT records (budget 1999); `WorldParams` (8 bytes in
      `ModeParams`, 0 = default) carried by movies / save states / the mode checksum; extended pool (>= 16384, 32768 from 2048; scenery in extra records beyond the 1999; dd 255 in wide worlds) for
      generated worlds. **Not done** (listed in the report): lava thresholds, flood-fill connectivity (a carpet flies;
      walkers' reachability only matters once units have orders - round 15), day length / blight (rounds 13 / 21),
      lobby exchange (round 12), re-arming / visible triggers and payload weights as parameters. Original text:
      seeded feature placer v1 (`worldgen.cpp`, new): symmetry (point / mirror on the torus) chosen from
      the player count; spawns first, each on a flattened 24x24 plateau; mana springs in distance bands (safe /
      contested / rich), wild lairs Poisson-disc graded 1-5 by distance; towns, trees, water and lava
      thresholds, flood-fill connectivity with carve-or-reroll; `WorldParams` = seed, size, players, symmetry,
      roughness, water, spring density, wild density, lair grade curve, day length, blight on / off; exchanged in
      the lobby like `NetGameRules`. Generation runs with the extended pool.
      **Trigger areas and teleport portals** (user request 2026-10-08), built on the original's own mechanisms:
      - *Triggers*: class-11 switch Things (32 types) fire `switch_activate(dis_id)`, which spawns every level
        record with that `DisId` (ENGINE.md 2.3). The placer scatters switches with a hidden payload group, rolled
        from a weighted table by distance band: mana balls / a mana shower, a creature ambush (graded like the
        lairs), an earthquake, a volcano, a meteor shower / lightning storm, a crater field, rarely an elite or a
        spell urn. Payload records go into the synthesised `LevelData.things` (1999 records - budget them, or add a
        mode-side record table consulted by `switch_activate` for ids above the level file's). Check first which
        switch types fire for whom: they must fire for **any** wizard (AI bots included) and deterministically,
        never by the local player only (same trap as the wake rule); one-shot vs re-arming per switch (re-arm
        timer under the mode flag), optional visible marker (rune / shimmer) as a `WorldParams` choice.
      - *Portals*: effect 0x22 Teleport carries its destination cell (`Thing.home`, ENGINE.md 2.6); the placer
        places linked pairs (both ends, so they work both ways) with a minimum separation (e.g. a quarter of the
        world), never inside a spawn plateau, a few per player; destination wrap and arrival through the world
        accessors (round 11 session 1: the teleport gate's default 0x8000 / 128-cell destination is a 256-world
        constant to replace). Bots must learn to use them in round 17 (portal graph as a shortcut in pathing).
      - Both carried by the world seed / `WorldParams` (counts, payload weights, re-arm on / off), so every peer and
        every replay places the same triggers; a scenario per payload type (fly over, assert the spawn). Keep the
      payload table data-driven: round 20 adds MC2's trigger types in a second pass.
- [x] B6 deterministic wake-near-any-wizard under the mode flag (session 3: nearest wizard of every record in play,
      also in a network mode run; `world_net_test`). Session 1: castle-site quadrants scale with the world (session 3:
      replaced in a wide mode world by own seat + 64-cell quadrants), sound cull int64.
- [x] B7 `render_ext` window `kWin = min(N/2, 256)`, vertex cache sizing, camera no longer truncated to u16, draw
      distance up to 255 at N >= 512 (the ini / pause-menu maximum is still 127); radar via the accessors; the map
      screen shows a window around the player at the original scale (whole world = the round 16 minimap);
      interpolated camera keeps the whole coordinate (session 1).
- [x] B9 (session 3: `world_test` - 140 levels incl. Hidden Worlds hash-identical to `bf77e98`, golden file
      `src/tests/data/world_levels_256.txt`; `reference_world`; `world_render_test` dd 127 / 255; `world_net_test`)
      `world_test`: 256 through the new code == old arrays for all 69 levels; 512 / 1024 seam walks, spiral
      queries across the seam, two-instance determinism for 2000 ticks, save round trip, overflow probes at 500
      cells; `render_ext_test` on 1024 at dd 127 and 255; `MC_REF_WORLD=1024` campaign references diverge only
      where a Thing crosses the old boundary. Partly done in session 1: five per-subsystem tests on a tiled 1024
      world (`world_{terrain,things,creatures,persist,render}_test`: seam walks, spiral / area queries across both
      seams, 300-600-cell distances and angles, save / movie round trips, pixel-identical tiled frames, dd 200).
- Gate (**met** 2026-10-08, session 3): `mcport <dir> rts --seed 1 --size 1024 --bots 3` runs, two instances stay in sync, the free camera
  flies the whole world. Effort 25-35 days (the largest round; split: accessors / positions / generator /
  placer / renderer+tests).

### Round 12 - launching the mode: menu, setup screen, lobby, match rules v1

- [ ] **Main-menu urn** bottom right (SpriteDesc 0x4d -> tmaps chunk 116, palette-remapped with
      `palette_find_nearest`), rectangle hit test before the `mmmask.dat` lookup, drawn only when the mode is
      enabled in settings (keeps the 33 front-end screens identical; `--faithful` hides it). The same icon in
      the multiplayer setup selects the mode for a network game.
- [ ] **Setup screen** (front-end state 3 / 11 free, lobby as template; 640-wide virtual UI): seed (random /
      typed), world size 512 / 768 / 1024 / 2048, players and teams, bots count / personalities / difficulty,
      generator sliders (roughness, water, springs, wild density), day length, blight, win conditions, match
      length; a minimap preview rendered by the radar painter from the generated heightmap (generation takes
      well under a second); "copy seed" to the clipboard.
- [ ] **Match rules v1** in the mode block: teams (`same_side()` replacing the ~25 `owner != owner` tests under
      the mode flag), respawn timer at the castle, elimination on castle loss, desecration channel, win by
      elimination / mana dominance / spring hold, timer; result screen with stats (kills, mana, towns, elites).
- [ ] **Bots v1** = the existing wizard AI (`is_computer`) with the base-game castle loop, so matches are
      playable end to end before round 17; AI `rand()` seed hashed / replaced by the mode RNG.
- [ ] Lobby: `WorldParams` + mode rules exchange, join-in-progress stays off (round 19), host migration kept.
- Gate: a human vs 3 bots match on 1024 from the urn to the result screen; the same match from the network
  lobby between two processes stays in sync (`net_test` mode part). Effort 8-12 days.

### Round 13 - the RPG layer: plates, damage numbers, elites, drops

Cheap, visible, and it exercises the overlay path everything else uses.

- [ ] `g_hook_damage_applied(victim, amount, attacker)` at the six intake sites (all port code, null default).
- [ ] **Health plates** over any damaged Thing (health < max) in `render_ext`'s per-Thing emit: bar + border
      colour by rarity (white / blue / yellow / gold), name label for elites; **floating damage numbers**
      (render-only, from the hook, ring buffer, 25-tick life, colour by source). First version in the 640-wide
      2D layer; moved to the RGBA overlay in round 16.
- [ ] **Wild spawner**: lairs placed by round 11 spawn their graded monsters on a timer with a cap per lair
      (DK portal rate); sleep by day (round 11's day cycle) once the clock exists.
- [ ] **Rarity roll** at spawn: Normal / Champion pack (2-4, +damage, +speed) / Named Elite (2-3 affixes, name
      from a syllable table seeded by the mode RNG, bigger `mana` field = bigger drop,
      `thing_set_sprite_double`) / Boss (lair grade 5 only). Affixes reuse effects: Fire-breathing, Swift,
      Armoured (damage/2), Mana-burning, Lightning-enchanted (bolt on death), Volcanic (small volcano on death),
      Teleporting, Splitting, Regenerating (troll body), Aura. Shown on the plate.
- [ ] **Drop tables**: elite kills guarantee a drop; treasure classes `Elite_Tn -> {mana orb, scroll (charged
      casts), spell upgrade token, new spell urn}`, quality roll with a hidden pity counter, drop beam + sound +
      "+N mana" number; spell pickups reuse the base game's phase-1 spell pickup Things; "NoDrop shrinks with
      wizards nearby".
- [ ] **Spell levels** side struct (per player per spell, 1-3): scales `damage`, `mana_total`, duration at
      launch; upgrade tokens apply to an equipped spell; the book shows the level.
- [ ] **Wizard levels** (WC3 curve, 10 levels, creep XP decays to zero by level 5): +health, +regen, +1 essence
      cap per level; respawn time scales.
- Gate: scenario `elite_drop.scn` (spawn elite, kill it, assert drop and plate), screenshots of plates and
  numbers at 1080p and 4K, determinism replay. Effort 8-10 days.

### Round 14 - economy and towns

Built on the village system (`phase4_code_gameplay.md` 4.2-4.3): villages already have inhabitants, capacity,
periodic mana, villager emission, builders founding new villages, claim by player, collapse.

- [ ] **Resources** per player in the mode block: mana (existing), food, lumber, ore; `mana_totals_update`'s
      pass extended, no new full scans. HUD strip in first person, panel in the overview.
- [ ] **Buildings** as class-4 extension types with footprints in a new `buildings_ext.tab` (same byte format as
      `building.tab`): town centre (claims the town, sets the influence radius), farm (food), lumber yard
      (lumber from forest cells), mine (ore from rock / mountain cells), shrine (claims a spring, mana regen
      aura), lair (round 15), hive / quarry / swamp pool / spire (unlock monster types), tower (MC2 fire /
      lightning tower turret on the castle wall). Build = re-stamp like the castle (`castle_footprint_clear` +
      the raise effect with a synthetic footprint); roof colour by building type = new paint kinds (palette
      variants of the castle roof tiles, or MC2's building tiles).
- [ ] **Fixed plots**: each town centre offers 6-8 plots on flat ground around it (Halo Wars / Tooth and Tail);
      building order = packet (plot id, type); villagers build it over N ticks (the village build state).
- [ ] **Gatherers**: villager body with a carry state: pick nearest resource node (forest / rock cells, goats, mana
      springs) within the influence radius, walk, take, return, deposit; auto-assigned by building type; killable
      (owned, so enemies can raid them). A per-town "focus" toggle instead of manual assignment.
- [ ] **Organic growth**: the base loop (full town emits villagers, builders found villages) stays; towns inside a
      player's influence are claimed by the town centre; a town levels up (re-stamp bigger footprint) when food
      and population thresholds are met; new villages pick the owner's influence first.
- [ ] **Upkeep**: castle mana trickle above two essence-in-play thresholds; starvation (food < 0) makes monsters
      disloyal (DK happiness, one bar, two remedies).
- [ ] **Build menu** in first person (radial on the D-pad / number keys, aims at the nearest own plot) and in the
      overview (round 16).
- Gate: scenario `economy.scn` (found town, build farm + mine, assert stock after N ticks), bot-free 1-player
  match reaches a level-3 town in 10 minutes, determinism replay. Effort 12-16 days.

### Round 15 - units and orders

- [ ] **Lair**: summon queue (type, cost in essence + resources, time), rally flag, spawn with `owner` = wizard,
      `filter_cls 0xff`, forced awake; unlock list driven by town buildings; extra lairs / captured wild lairs
      raise the rate.
- [ ] **Unit handler** (class 5, extension types; one handler, per-type `UnitDesc`): orders idle / guard / move /
      attack-move / attack target / follow wizard / return; reuse `creature_attack_target`, `follow_leader`
      (flocking), `villager_walk_to_wizard`; retreat below 40 % health to the lair; enemy search over
      `creature_lists` and `player_list` with `same_side()`; rock-paper-scissors class bonuses (melee / ranged /
      flyer, 1.5x). Pathing: steer-and-detour plus a coarse flow field on a 1/8-cell grid for ground units
      (flyers need none; water / lava / walls are the levers).
- [ ] **Essence**: pool per player, cost per tier, return on death after a timer, *convert* channel by an enemy
      wizard at the corpse (Sacrifice), essence pickups in the wild.
- [ ] **Orders over the wire**: packet-level orders while they fit (`cmd 0x20`: order, type filter, cell), the
      variable-length blob exchange (`net_exchange_var`) for selections; both recorded by movie v3; latency
      masking (local marker + sound at once, execution when the packet lands).
- [ ] **First-person command set**: four orders on D-pad / 1-4 (rally to me, attack-move to reticle, hold, return
      to lair), LB / Tab-hold cycles the type filter, Battlezone two-press for a specific target (reticle Thing).
- [ ] **Veterans**: unit levels 1-10, trained to 4 at the lair, 5+ from kills; names at 5; plates show level.
- Gate: scenario `units.scn` (summon 6 bees, attack-move to an enemy castle, assert damage), bot castles fall to
  ordered units, two-process lockstep with orders in sync for 5000 ticks. Effort 14-18 days.

### Round 16 - the RTS overview camera and native UI

- [ ] **Oblique renderer** (`render_ortho.cpp`, step B8): camera pitch 45-60 degrees at variable height over the
      torus, list builder over `render_ext_raster`'s `ExtTri` / `ExtSprite`, LOD at far zoom, fog of war
      (explored / visible by own Things), threaded like `render_ext`; the faithful renderer untouched.
- [ ] **Picking**: cursor -> ground cell (height-field ray march) and -> Thing (projected anchors), drag box.
- [ ] **RGBA overlay layer** in `ComposeOutput` / `present_composed` at display resolution with an atlas font
      (TTF rendered once at startup or a prebuilt SDF atlas): plates, damage numbers, selection rings, order
      markers, resource strip, build radial, minimap (radar painter at N), unit cards, tooltips. Debug overlays
      move here too; the fade multiplies it, debug text is excluded on purpose.
- [ ] **Controls**: mouse + keys (drag select, double-click all of type, 4 groups, rally flags, Shift queue),
      controller (Halo Wars scheme: tap / double-tap / paint select, hold RB + face for groups, stick radial for
      building). Toggle key `M` / Back; the wizard keeps flying its last heading in the overview (intent-based
      steering).
- [ ] Overview spell list: Beyond Sight, Teleport / Castle Port, Heal on units, nothing offensive.
- Gate: pixel test of the oblique renderer on a hand-checked frame, 1080p / 4K timings (target < 8 ms), an
  order issued from the overview lands as the same packet as from first person. Effort 14-18 days.

### Round 17 - wizard bots

Three layers (`phase4_research_games.md` 6.3), all deterministic over the mode RNG, run as a per-tick pass after
`player_commands_process` emitting the same packets a human would.

- [ ] **Strategic**: DK-style prioritised *processes* (claim 2 springs, found town, build farm, build lair,
      summon wave, creep elite, raid weakest neighbour's gatherers, defend, desecrate) fed by periodic *checks*
      and pre-empted by *events* (castle hit, wizard low, spring lost); a build table per personality.
- [ ] **Tactical**: influence maps on a 1/8-cell grid (friendly, enemy, tension, vulnerability) updated every
      12 ticks; choose where raids, defence and the avatar go.
- [ ] **Unit layer**: squad FSM (idle at lair -> move -> engage -> retreat) with a utility target picker.
- [ ] **Avatar**: the existing `ai_wizard` flight / dodge / cast layer kept, driven by the strategic goal (fly
      to the spring, creep the elite, go home when hurt, join the push); allowed spells extended past 0x11.
- [ ] **Avatar baseline: 1996 AI plus picks from the 1995 engine** (decided 2026-10-08; the 1995 behaviour is
      already in the port behind `engine1995()`, see `docs/analysis/port_hidden_engine1995.md`). Keep the 1996
      `ai_wizard` as the base (final release, covered by the most references incl. scripted human input) and
      take, each as a `GameplayRules` flag for the mode (save states / movies / network carry it, base and
      Hidden Worlds levels stay exact):
      - **Speed Up that works**: 1995 `ai_approach_target` / idle / return-home leave the target speed alone
        while Speed Up runs; 1996 resets it to 0x50 at once, wasting the cast (a 1996 bug).
      - **Hover momentum** (optional, judge in bot-vs-bot tests): the 1995 hover tails of modes 1/3/6/7/8 do
        not zero the target speed.
      - **Spell 22 pickup fix**: 1995 binds class 12 record 68 to the phase-2 pickup handler; in 1996 that
        pickup can never be collected.
      - **Aimed Fire Wall AI** together with the homing Fire Wall spell (round 18): 1995 `ai_spell_ready` /
        `ai_cast_spell` aimed cast, cooldown 4, the rung after meteor in both attack-spell choices,
        countering and heavy-threat scoring of projectile 0x10. They only make sense together.
      - Not taken: the 1995 "both fire buttons, no movement = own castle loses a level" rule (a debug
        leftover, removed in 1996), the looser 1995 castle-upgrade test (1996's extra checks are better).
- [ ] **Personalities** (8 named wizards, MC2's roster names are a good start: Nyphur, Rahn, Jark, Belix,
      Elyssia, Yragore, Prish): favourite monster, aggression, expansion appetite, raid cadence, chat lines.
      Difficulty = gather multiplier + decision interval, hidden in fiction.
- [ ] **Balance harness**: headless bot-vs-bot matches (`mcport rts --bots 8 --headless`) with JSON summaries;
      a script runs N seeds and reports win rates, match length, army sizes; this is how tuning rounds work.
- Gate: 8 bots finish a 1024 match in under 45 minutes of game time on every tested seed, no bot idles, win
  rates within 35-65 % for equal personalities, deterministic across two instances. Effort 14-18 days.

### Round 18 - new spells, spell levels, loadout

From `phase4_code_gameplay.md` 5.2 (14 sketches) and MC2's list (`phase4_research_mc2.md` section 2). Each is an
extension-table handler plus a `kSpells`-style row; spell Things are class 12 with extension types.

- [ ] Tier 1 (reuse only): **Thunderstorm** (storm cloud 0x26 spawning lightning at random enemies in range, white
      smoke rain), **Chain lightning**, **Meteor shower**, **Firestorm** (fire pillar + lava blobs), **Summon
      swarm** (skeleton-army body parameterised: bees / vultures, owner = caster), **Earth wall**
      (`level_build_wall` toward the aim point), **Sinkhole / Raise land** (crater 0xb / ridge 0x33), **Mana beacon**
      (hoard marker + magnet pull), **Mind control** (possession projectile with `filter_cls 5`, claim slot ->
      owner), **Raise dead**.
- [ ] Tier 2 (MC2 ports): **Whirlwind / Tornado** (moving kraken-grip effect lifting creatures), **Tremor**,
      **Gravity Well** (projectile and creature pull; destroys buildings), **Castle Port** (cycle own / rival
      castles), **Ransack** (tenth of a castle's mana dropped at your castle), **Mana Magnet / Mana Lock**,
      **Fire / Lightning Tower** (round 14 building), **Summon Army** (bees / cymmerians / wyverns once MC2
      creatures load, round 20), **Alliance** (one species in a radius fights for you), **Magic Mine**, **Fool's
      Mana**, **Blizzard** (slow slot on units), **Mass heal / Regeneration aura**.
- [ ] **Homing Fire Wall** from Hidden Worlds (already ported behind `world_hidden()`,
      `docs/analysis/hidden_worlds_re.md` section 4): a homing shot (MoveDesc index 2, cone 0x100) leaving an
      explosion trail, ending in a small meteor-style shock ring (`% 7`). Offer it as the mode's Fire Wall or as
      an extra spell, with the 1995 aimed Fire Wall AI from round 17.
- [ ] Spell levels 1-3 applied by the round-13 side struct; loadout of 6 at match start; discovery drops biased to
      the loadout; the book shows locked / level.
- Gate: one scenario per spell (cast, assert effect Things / damage), AI allowed-spell table extended,
  determinism replay. Effort 10-14 days.

### Round 19 - online

- [ ] **Relay / dedicated host**: relay mode in `TcpTransport` (the name server forwards data frames so only the
      relay needs a public port) and a headless `mcport <dir> relay` / dedicated host process; lobby list service
      on the name server; `net.cpp` untouched (NetBIOS semantics stay).
- [ ] **Input delay ring** (apply tick N at N + d, d from measured RTT, AoE's adaptive turn) so the exchange
      overlaps the simulation; non-blocking receive on a worker thread (NetContext is thread-local) so the window
      stays alive during stalls; UDP transport with redundant inputs as a second `NetTransport` once TCP relay
      works.
- [ ] **Rejoin / late join / spectators**: snapshot (GameState serialised + mode block + maps) + buffered
      packets since; receive-only role for spectators and for a replay viewer.
- [ ] **Replays**: every match writes a movie v3 (seed + params + command stream); replay viewer = the demo
      mode's free camera plus the overview; desync dumps as in round 10.
- [ ] Chat, ping display, drop handling (host migration exists), anti-cheat limited to "all peers simulate".
- Gate: a 4-player match over the relay across NAT, 150 ms artificial latency, 2 % loss, no desync for 30
  minutes; a late joiner and a spectator attach mid-match. Effort 12-16 days.

### Round 20 - Magic Carpet 2 assets and dungeons (stretch)

- [ ] `mctools.level` / `mcdata/level.c` MC2 variant (26,116-byte levels, 20-byte entities, class 14 / 15); sprite
      banks, tmaps and palettes load as they are; palette remap tables between MC1's and MC2's palettes.
- [ ] New wild / summonable creatures from MC2 sprites: Wyvern (elite air), Manticore (cavalry), Cymmerian
      (raider with a bug swarm on death), Devil, Spider (web stun), Sentinel (static turret), Troglodyte
      (artillery), Hydra (boss), Zombie (anti-caster swarm), Mana Worm (walking resource), Moon Dweller; each a
      `UnitDesc` over the existing creature bodies (archer / dragon / worm / genie are the behaviour donors).
- [ ] MC2 effects: tornado vortex, towers, structures rising ("Loretower is revealed" = the ridge raiser).
- [ ] **Dungeons**: v1 = walled canyon regions on the surface (MC2 wall / path / canyon class-10 things, breakable
      by Gravity Well / Earthquake, sentinels and spiders inside, a boss lair and a guaranteed urn) - no engine
      change. v2 (only if v1 is popular) = true interiors as separate 256 worlds with the cavern sky and palette,
      entered by a portal Thing; needs the world module to hold several world instances and Things to carry a
      realm id - a deep change, decide after v1.
- [ ] **Trigger / feature pass 2** (user, 2026-10-08): once MC2's data loads, a second pass over the world placer's
      trigger areas and portals (round 11 B5) for MC2's additional trigger / switch types and level features
      (inventory them from the MC2 level format and `phase4_research_mc2.md` first): new payload kinds in the
      weighted tables, any new activation rules (still firing for any wizard, deterministic), MC2-style portals /
      structures, scenarios per new type.
- Gate: MC2 creatures appear in wild lairs and in the lair's unlock list, pixel check of remapped sprites.
  Effort 10-14 days (+15-20 for dungeons v2).

### Round 21 - retention and polish

- [ ] Match flow: countdown, phase announcements (dawn / dusk / blight rings), end-of-match screen with graphs
      (mana over time, army size, kills), MVP, replay save prompt.
- [ ] Offline progression: ascension tiers (wild affixes up, bots faster) unlocked by wins; seed-of-the-day.
- [ ] Balance passes with the bot harness; tutorial scenario files; controller pass; accessibility (plate size,
      colour-blind palettes for rarity).
- [ ] Settings UI for the mode in the pause menu; key rebinding (Phase 3 leftover) becomes necessary here.

### Round 22 - GPU renderer (end of Phase 4; added 2026-10-08)

Why: not needed for frame rate today - the Release build runs **165-190 fps on level 10 at draw distance 127**
(user, 2026-10-08; the 35-45 fps first reported was the unoptimised Debug build). It is planned last because the
load only grows (1024-cell worlds, dd 255, armies, plates / numbers, the round-16 overview and RGBA overlay at
display resolution), and the user expects to want it eventually. Until then everything stays on the CPU path.

- [ ] **Measure first** (Release and Debug, 1080p and native, dd 80 / 127 / 255, a busy late-match 1024 world):
      per frame `render_ext` list build and rasterise, overview renderer, HUD / overlay, compose / upscale, 8-bit ->
      RGBA conversion, SDL texture upload / present, sim ticks (F11 profile line, `MC_REXT_PERF`, `tick_profile`).
      If one CPU stage dominates, fixing it may be enough - decide the scope of the round from the table.
- [ ] **D3D11 renderer** (`mcport/gpu/`, default choice: Direct3D 11 directly, since the platform already runs on
      SDL's d3d11 backend and the port is Windows-only; SDL2's renderer has no custom shaders): terrain as a
      height-field mesh (vertex buffer per chunk of 32x32 cells, rebuilt only when cells change - castle raise,
      craters, retexture), the texture atlas and the shade / blend tables as textures, **an 8-bit look kept on the
      GPU** (palette index + shade -> palette lookup in the pixel shader, point sampling, the original's fog /
      haze curve), sprites as camera-facing quads from the sprite cache (palette-index textures), water / sky /
      reflections as in `render_ext`. Display lists come from the same per-frame Thing pass `render_ext` uses
      (interpolated positions, segment layout, mana-ball colours), so nothing new reads game state.
- [ ] **Composition on the GPU**: the 640-wide HUD / menus / overlays uploaded as one palette-index texture and
      blended over the 3D view, upscaling and the palette effects (fades, flashes) in the shader; the round-16
      overview renderer and RGBA overlay layer ported to GPU passes on the same device.
- [ ] **Settings / fallback**: `render.backend = gpu | cpu` (`mcport.ini`, pause menu); the CPU renderers stay as the
      fallback and for headless runs / tests; the faithful renderer and every pixel reference are untouched
      (render-only, default settings unchanged); vsync and an fps cap.
- [ ] Tests: a GPU-vs-CPU image comparison on fixed cameras (level 38, a 1024 world at the seam, the overview;
      tolerance metric, PNGs looked at), headless skip when no D3D11 device; determinism unaffected (checksums with
      both backends identical); frame-time numbers in the report.
- Gate: 1080p and 4K at dd 127 **and** dd 255 hold the display refresh rate in Release in a busy 1024 match;
  screenshots of both backends side by side. Effort 10-14 days.

---

## 4. Effort summary and order

| round | theme | days | depends on |
|---|---|---|---|
| 10 | debug suite + mode skeleton + movie v3 (**done 2026-10-08**) | 8-10 | - |
| 11 | world module, int32 positions, generator, placer (**sessions 1-2 of 3 done 2026-10-08**) | 25-35 | 10 (headless runs, tests) |
| 12 | menu urn, setup screen, lobby, rules v1, bots v1 | 8-12 | 11 |
| 13 | plates, numbers, elites, drops, levels | 8-10 | 10 (hook, overlay); 11 (lairs) |
| 14 | resources, buildings, gatherers, growth, upkeep | 12-16 | 12 |
| 15 | lair, units, orders, essence, first-person commands | 14-18 | 14 |
| 16 | overview renderer, picking, RGBA overlay, controls | 14-18 | 15 (orders), 11 |
| 17 | bots (strategic / tactical / unit / avatar) + balance harness | 14-18 | 15, 16 (minimap data) |
| 18 | new spells, levels, loadout | 10-14 | 13 |
| 19 | relay, input delay, rejoin, spectators, replays | 12-16 | 15 |
| 20 | MC2 assets, dungeons v1 | 10-14 | 13, 15 |
| 21 | match flow, progression, balance, polish | open | all |
| 22 | GPU renderer (D3D11), GPU composition, frame-time profile | 10-14 | 16 (overview + overlay to port) |
| | **total to a playable online mode (10-19)** | **125-167** | |

Rounds 13 and 18 can run alongside 14-16 (different files); 19 can start after 15. Rounds 11 and 12 are the
critical path. Round 22 touches only the renderers / platform and can be pulled forward if profiling ever shows
the CPU path is the limit.

---

## 5. Decisions for the user (defaults chosen so work can start)

1. **Mode name**: "Conquest" is a working title.
2. **World sizes**: 512 / 1024 / 2048 / 4096, default 1024 (16x area). Powers of two only (round 11: masks
   everywhere; 768 dropped), all through `world.h`.
3. **Economy**: essence (returning souls) for armies, four resources for towns, mana stays the win resource.
   Simpler alternative if it tests badly: mana-only costs with the WC3 upkeep curve.
4. **First-person orders**: four fixed orders + type filter; no free cursor in first person.
5. **Native overlay font**: a bundled open-licence TTF rendered to an atlas at startup (round 16).
6. **MC2 data**: loaded from the user's GOG copy via `MC_MC2_DIR`; nothing from it is shipped.
7. **Dungeons**: surface canyons first; true interiors only after that proves fun.
8. **GPU renderer**: in Phase 4 as its last round, 22 (user, 2026-10-08). Default API Direct3D 11 (Windows-only port,
   SDL already on d3d11); alternatives OpenGL 3.3 through SDL (portable) or SDL3's GPU API (an SDL2 -> SDL3 move).

---

## 6. Next: round 12 (launching the mode)

Round 11 is complete on branch `round11` (sessions 1-3; the user merges into main). Round 12 starts from section 4's
round 12 list. What round 11 hands over: `mode_start_net_run` (mode_level.h) is the call the lobby makes after its
setup exchange (replacing `rts --net`); `NetGameRules.mode_params` carries the setup; bots die to woken monsters before
their castle and never respawn without one (round 13's respawn rule); bots roam the whole world for mana (round 17).
Tests that load several levels in one process must reset the carried-over globals (`port_world_test.md`).
Agents build with `--parallel 2 -- -p:CL_MPCount=1` and run `ctest -j 2`.
