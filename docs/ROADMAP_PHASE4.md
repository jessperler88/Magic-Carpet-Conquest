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
9. **GPU renderer**: not needed. The CPU `render_ext` does 4K at dd 127 in 6 ms on 24 threads; the overview
   renderer reuses its rasteriser and thread pool. A GPU path stays a Phase 5 option for filtered textures.
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
Session 2: B4 + B5 (generator on N, feature placer, `WorldParams`). Session 3: B6 wake-near-any-wizard, `world_test`
/ `MC_REF_WORLD`, two-process sync at 1024, the gate, plus the open design points the sweep listed (fixed-range
constants that do not scale: sound cull, tether, teleport gate 128 cells, spiral search 15 rings; world-wide
searches such as skeletons -> nearest enemy castle; castle-site quadrants growing with N; rivers / source tries
scaling with area; `draw_distance` max 127 in config / pause menu).

- [ ] B1 world descriptor + accessors (`world.h`: `world_cell`, `world_cell_of`, `world_cell_step`, `cell_x/y`,
      `world_diff`, `world_dist_sq` int64), replacing `mc_cell`, the three `cell_xy` clones, `(uint8_t)`
      neighbour arithmetic and 0x10000 strides in 14 files; maps allocated at level start outside GameState
      (faithful: the existing static arrays); `WRLD` chunk in savestates / `max%05d.dat`.
- [ ] B2 int32 `Pos` / `Thing.x/.y` / `start_pos` / `PosLogEntry` / `Thing.home` / `ThingInit`; int32 `math_atan2`;
      all 74 `pos_dist_*` and the nearest-search `best_d` sites to int64; AI / sound range constants reviewed.
- [ ] B3 GameState serialise / deserialise to the original layout (precedent `thing_relink_snapshot`);
      `reference_test`'s field table re-pointed; every gate re-validated.
- [ ] B4 generator on N: int32 fractal scratch (or seeded coarse lattice, which doubles as a "continent scale"
      slider), k levels, river / 1000-tries constants scaled, local passes through the accessors;
      `terrain_build(const GenMap&, const WorldParams&)`.
- [ ] B5 **seeded feature placer v1** (`worldgen.cpp`, new): symmetry (point / mirror on the torus) chosen from
      the player count; spawns first, each on a flattened 24x24 plateau; mana springs in distance bands (safe /
      contested / rich), wild lairs Poisson-disc graded 1-5 by distance; towns, trees, water and lava
      thresholds, flood-fill connectivity with carve-or-reroll; `WorldParams` = seed, size, players, symmetry,
      roughness, water, spring density, wild density, lair grade curve, day length, blight on / off; exchanged in
      the lobby like `NetGameRules`. Generation runs with the extended pool.
- [ ] B6 deterministic wake-near-any-wizard under the mode flag; `ai_wizard` quadrant logic generalised; sound
      cull with `world_diff`.
- [ ] B7 `render_ext` window `kWin = min(N/2, 256)`, vertex cache sizing, camera no longer truncated to u16;
      radar / map screen scale for N (the map screen shows the whole world or a window).
- [ ] B9 `world_test`: 256 through the new code == old arrays for all 69 levels; 512 / 1024 seam walks, spiral
      queries across the seam, two-instance determinism for 2000 ticks, save round trip, overflow probes at 500
      cells; `render_ext_test` on 1024 at dd 127 and 255; `MC_REF_WORLD=1024` campaign references diverge only
      where a Thing crosses the old boundary.
- Gate: `mcport <dir> rts --seed 1 --size 1024 --bots 3` runs, two instances stay in sync, the free camera
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
- Gate: MC2 creatures appear in wild lairs and in the lair's unlock list, pixel check of remapped sprites.
  Effort 10-14 days (+15-20 for dungeons v2).

### Round 21 - retention and polish

- [ ] Match flow: countdown, phase announcements (dawn / dusk / blight rings), end-of-match screen with graphs
      (mana over time, army size, kills), MVP, replay save prompt.
- [ ] Offline progression: ascension tiers (wild affixes up, bots faster) unlocked by wins; seed-of-the-day.
- [ ] Balance passes with the bot harness; tutorial scenario files; controller pass; accessibility (plate size,
      colour-blind palettes for rarity).
- [ ] Settings UI for the mode in the pause menu; key rebinding (Phase 3 leftover) becomes necessary here.

---

## 4. Effort summary and order

| round | theme | days | depends on |
|---|---|---|---|
| 10 | debug suite + mode skeleton + movie v3 (**done 2026-10-08**) | 8-10 | - |
| 11 | world module, int32 positions, generator, placer | 25-35 | 10 (headless runs, tests) |
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
| | **total to a playable online mode (10-19)** | **125-167** | |

Rounds 13 and 18 can run alongside 14-16 (different files); 19 can start after 15. Rounds 11 and 12 are the
critical path.

---

## 5. Decisions for the user (defaults chosen so work can start)

1. **Mode name**: "Conquest" is a working title.
2. **World sizes**: 512 / 768 / 1024 / 2048, default 1024 (16x area). Powers of two use masks, 768 uses modulo;
   all go through `world_diff`.
3. **Economy**: essence (returning souls) for armies, four resources for towns, mana stays the win resource.
   Simpler alternative if it tests badly: mana-only costs with the WC3 upkeep curve.
4. **First-person orders**: four fixed orders + type filter; no free cursor in first person.
5. **Native overlay font**: a bundled open-licence TTF rendered to an atlas at startup (round 16).
6. **MC2 data**: loaded from the user's GOG copy via `MC_MC2_DIR`; nothing from it is shipped.
7. **Dungeons**: surface canyons first; true interiors only after that proves fun.
8. **GPU renderer**: not in Phase 4.

---

## 6. Next session: round 11 briefing (round 10 briefing below is done)

Write `docs/port/BRIEFING_round10.md` from section 3 / round 10 with five tasks: (A) mode skeleton + headless
`rts` run + JSON dumps + movie v3, (B) console + scenario files + `scenario_test`, (C) projection export +
display pointer + inspector + overlays, (D) time control + debug camera + screenshot + log, (E) desync parts +
diff tool + replay-check. Integrator scaffolding first: `GameplayRules.mode`, `mode.h` state block with its
savestate chunk and checksum part, `sim_load_level_data`, and the `rts` subcommand stub in `main.cpp`.
