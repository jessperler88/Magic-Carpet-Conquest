# Round 11 session 2: the generated world (B4 generator on N, B5 feature placer)

Integrator session, 2026-10-08, branch `round11`. New: `src/mcengine/worldgen.{h,cpp}`, `src/tests/world_gen_test.*`.
Changed: `terrain_gen.cpp` / `terrain.h` (B4 primitives, `terrain_build_passes`), `sim.cpp` (`g_hook_level_world`),
`mode.h` (`WorldParams` in `ModeParams`, `MODE_PARAM_TILED` / `_GENERATE`, `kGeneratedWorldSlots`), `mode_level.cpp`,
`thing.cpp` (pool minimum), `scenery.cpp` (switches fire for computer wizards in a mode run), `scenario.cpp` / `rts_run.cpp`
/ `main.cpp` (options), `world_persist_test.cpp` (WRLD block size with the extended pool). 66/66 ctest; every reference
set identical (nothing here runs for the campaign or a template mode level).

## 1. How a generated world is built

`sim.cpp level_start_from_state` asks `g_hook_level_world` before `terrain_build`; `mode_level.cpp` installs it and
answers for a mode level with `mode_params_generated(params)` (world_size > 256 unless `--tiled`, or `--generate` at
256). `worldgen_build(params, &g_state->level)`:

1. `world_set_size(N)`; `g_rng16` / `g_state->rng` seeded from the template's `GenMap.seed` as `terrain_build` does.
2. **Height field** (`terrain_fractal_world`): torus diamond-square over int32, levels N/2 .. 1 in the original's
   square-then-diamond order, the original's noise shape (`avg - a + r % (2a+1) - gnarl + r % (2gnarl+1)`, one draw
   for both terms) with `a = step * 32` up to step 128 (exactly the original's per-level amplitudes) and
   `a = 4096 * continent %` above (default 50 %). So the local relief is the campaign's and the levels above 128 cells
   add continents without flattening it. Own 64-bit LCG seeded from the match seed (not `g_rng16`, as the original's fractal).
3. **Symmetrise** (worldgen.cpp): the average over the symmetry group (no seams, unlike copying a fundamental domain),
   divided by its expected standard deviation `sqrt(sum_ab rho(d_ab)) / g` with `rho(d) = max(0, 1 - d / L)`,
   L = min(256, N/2). Without that correction the group's fixed points (centre / corners), where all images coincide,
   keep the full variance and everything else is flattened by 1/sqrt(g): the first 1024 map was one big mountain island.
   The ratio is computed every 4 cells and averaged over the orbit, so the result stays exactly symmetric.
4. **Sea level** (`terrain_normalise_world`): the value below which `water %` of the cells lie (4096-bin histogram)
   maps to 0, the maximum to 0xc4. Default 30 %. The original's rule (sea = field < 0) drowned whole worlds once the
   continent levels shift the field (seed 11 at 1024 was all sea) and is not offered.
5. **Seats**: the orbit of one point under the group (2 / 4 / 8 seats), best of 160 seeded candidates by
   (min seat distance in N/32 buckets) * 1000 - water cells in the plateau; without symmetry farthest-point sampling
   over 192 candidates. `voronoi` = half the smallest seat distance = the unit of the distance bands.
   **Plateaus**: 24 x 24 cells at one height for all seats (mean of their averages, clamped 0x28..0x90), a 12-cell ring
   blended into the terrain.
6. **Rivers** (`terrain_rivers_*_world`): `GenMap.river * (N/256)^2 / g` sources, each traced from all its orbit points
   (mostly symmetric rivers), `GenMap.sourc` minimum height, 1000 * area tries, plateaus + 2 cells protected
   (`g_map_type = 0`: never entered). The tracer keeps a visited list instead of the original's two whole-map sweeps
   per river (a 4096 world has 16M cells and ~10000 rivers).
7. `terrain_build_passes(gen)`: the original's tail (flatten water quads .. light map) with the template's thresholds,
   unchanged (`terrain_build` now calls it too; references identical).
8. **Features** -> `level->things` (THING_INIT records, budget 1999), cleared first. Everything is placed in orbits, so
   every seat gets the same environment (payload groups and packs replay the same LCG state per image):

| feature | records | where |
|---|---|---|
| start | cls 3 model 4 + p ("Flyer1..8": `world_set_start_pos`) | seat centre; seats beyond the players stay empty |
| portal pair | 2 x cls 10 model 0x22, parent / child = destination y / x | one end at 40-80 % of the seat region, the other >= N/4 away at >= 60 %; destination 3 cells beside the other gate (arriving on it would bounce back) |
| town | cls 10 model 0x2d, DisId 0xffff, parent 3..6 (size parent + 0x10) | 60-95 %, built by `terrain_generate_features` like the levels' villager castles |
| spring | 8 / 14 / 20 mana balls (cls 10 model 0x27) within 3 cells | bands safe 30-55 %, contested 65-95 %, rich 95-160 % |
| trigger area | cls 11 type 0 (hidden, inside, one-shot), swi_sz 2, swi_id = DisId; payload records with that DisId | 40-150 %, payload from a weighted table by band (below) |
| lair | a pack of 1-10 creatures by grade 1..5 (grade from distance) | 35-160 %, 20 cells apart (Poisson-disc by rejection) |
| wild | single creatures, grade 1..4 by distance | 45-200 %, `30 * area * wild %` orbits, at most a third of the free records |
| grove | 5-12 trees, now and then a standing stone | 25-200 %, what is left (12 per 256^2 area) |

Packs by grade: 1 bees / vultures / emus, 2 crabs / worms / archers, 3 trolls / skeletons / griffons, 4 dragons /
griffons / trolls, 5 wyverns / a genie / dragons. Krakens are not placed (water only).

**Trigger payloads** (`kPayloads`, weights safe / contested / rich): mana 6/4/3 (8-20 balls), ambush 2/4/4 (two packs,
grade 1-6 by band), earthquake 1/2/2 (effect 15), volcano 0/1/2 (effect 9, 0-6 cells off), storm 1/2/2 (lightning 23
+ meteors 17), craters 1/1/1 (3 x effect 11), elite 0/0/1 (a wyvern / genie / dragon; round 13 adds affixes),
urn 0/1/1 (a spell the template allows player 0 but does not give at the start). Round 20 adds MC2's types to the table.

Counts at the defaults (3 springs, 3 lairs, 3 triggers, 1 portal pair per seat): 1024 / 4 seats 1672 records, 2048 /
8 seats 1664 (groves get what is left), 512 / 2 seats 658, 256 / 2 seats 332. Generation: 256 ~5 ms, 512 ~25 ms,
1024 ~0.15 s, 2048 ~1 s (Release).

## 2. Rules changed for a mode run (all behind `mode_active()`, campaign untouched)

- **Switches fire for computer wizards** (`scenery.cpp switch_test_player`): the original skips every player Thing
  whose type != 0, so only human flyers (local or remote) fire switches - AI wizards never. In a mode run type 1 fires
  them too. Deterministic either way (no local-player test). The re-arm test (`switch_test_any_player`) already
  looks at every player record.
- **Extended pool**: a generated world always runs with at least 8192 slots (16384 / 32768 since 3b below)
  (`thing_pool_wanted_slots`; from the mode params, so every peer and replay agrees; movies record the size anyway).
  1652 records spawned into the faithful 1000 slots left no slot for the wizards.
- Teleport gates need no change: `effect_teleport_update` moves every player Thing (human or AI) that touches the gate
  while facing it (within 0xaa); the destination is the record's cell, any world size.

## 3. world_gen_test (exit 0, ~12 s)

1. 1024 / 4 players (rot4), 512 / 2 (point), 256 generated / 2 (mirror), 2048 / 8 (dihedral): size, seats >= players,
   records <= 1999, every player spawned within 12 cells of its seat, all seats' plateaus one height (>= 0x28, land),
   towns alive (effect 0x2d) = placed, every portal end a gate Thing whose destination is beside its partner, every
   trigger a switch Thing with its payload records waiting. Heights equal at all images: 90-95 % of the cells (the
   original's passes are not symmetric: in-place sweeps, neighbour scan order, the texture / light RNG).
2. Six 1024 worlds (seeds 11-16, 8 triggers per seat): a god-mode computer wizard flown into one trigger area of each
   payload type fires it - all eight types (ambush 5 records -> 53 Things, mana 13, storm 9, craters ~1000 debris Things,
   ...); wizard 2 placed in front of a gate looking at it comes out beside the other end (5 portals, all through).
3. Determinism: the same params give identical maps and records; 600 ticks twice give identical `net_state_checksum`
   sequences; another seed another world. `world_persist_test` records a v3 movie of a generated 1024 run and replays it
   (the playback regenerates the world from the movie's ModeParams): identical checksums every tick.
4. `world_gen_<size>.png` (`MC_WORLD_GEN_OUT`, default %TEMP%/mc_world_gen_test): water blue, land shaded by height /
   light, white seats, cyan springs, orange-red lairs (redder = higher grade), magenta triggers, yellow portals,
   brown towns, green groves. Looked at; in-game screenshots (`rts --size 1024` scenario with `shot`) show the
   plateau, rivers and lakes, a lair pack, a grove, a portal gate.

## 3b. Follow-up after the user's first play (same day)

User: "on that map size we need at least 16k things cap as its fairly sparse when it comes to features like trees etc.
also 255 view distance needs to be available in the options when on conquest mode big genned maps, 127 max obviously
stays the cap for base game and hidden worlds."

- **Pool**: `mode_generated_world_slots(N)` = 16384 up to 1024 cells, 32768 (the i16 ceiling) above.
- **Extra records** (`thing.h level_extra_records_set`): scenery and wild creatures no longer squeeze into the level
  file's 1999 records; `switch_activate(0)` counts (creature_count, sprite marks) and spawns them after
  `LevelData.things`, then empties the list (the level reset too). Per 256^2 cells: 12 groves of 6-14 trees, 40 lone
  trees / standing stones / dolmens, 40 wild creatures (x wild %), scaled together to a third of the pool. The level
  file keeps the seats, portals, towns, springs, lairs and trigger areas + payloads (360 records at 1024 / 4 seats).
  Start of a 1024 world: ~1960 trees, 325 stones, 4999 live Things in 16384 slots; 2048 / 8 seats: 6712 trees,
  16761 live in 32768.
- **Draw distance**: ini range 8..255; the pause menu's maximum is 255 in a wide world (`world_wide()`), 127 otherwise;
  `render_ext` already clamps to `kWin - 1` (127 at 256 cells), so the campaign and Hidden Worlds stay at 127 whatever
  the ini says (the menu then shows "(127 in this world)").

## 4. Open points / for session 3 and later

- The level file's 1999 records now only hold the gameplay features; 4096 worlds with many trigger payloads could
  still need payloads in a mode-side table (round 14's buildings will need one anyway).
- Springs are mana-ball blobs for now; round 14 makes them claimable territory (the layout keeps them as features:
  `worldgen_last_layout()`, not saved - regenerate from the params, or add a MODE-chunk copy when a later round reads it
  after a load).
- Trigger re-arm, visible markers (switch types 9 / 2) and per-payload weights are code constants, not `WorldParams`
  yet (8 bytes are full: a v2 ModeState can grow it).
- Day length / blight from the B5 list are not generator inputs; they belong to rounds 13 / 21 (flags or ModeState).
- The default 1024 world has one continent-scale highland; `--continent` lowers it (0 = default 50 %).
- Session 3 list unchanged: wake-near-any-wizard (creatures far from the local player sleep - in a 1024 world most
  lairs never wake), `MC_REF_WORLD`, two-process sync at 1024, `draw_distance` max 127 -> 255, fixed-range constants.
