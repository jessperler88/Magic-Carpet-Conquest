# world_test: the 256 world through the round 11 code equals the pre-round-11 code (round 11 session 3, task F)

Files: `src/tests/world_test.cpp`, `src/tests/world_test.cmake`, `src/tests/data/world_levels_256.txt` (golden, 148
lines, 22 KB). Build dir `build_F`. Result: **all 140 levels identical to commit `bf77e98`** (70 levels.dat entries + 70
Hidden Worlds DDLEVELS entries), every wide probe passes, `world_test` exit 0 in ~44 s (Release), zero warnings.
No round 11 bug found.

## What was translated

Nothing (a test task). No engine source was changed and no shared-file change is requested.

## What the test checks (and its numbers)

1. **Golden comparison** (~3 s). For every level of `levels.dat` (0..69) and, when the Hidden Worlds data is installed
   (`MC_HIDDEN_DIR`, `<game>/hidden`, or `<repo>/extracted/gog_cd/CARPET` - the same lookup as `world_set_test`; skipped
   otherwise), every DDLEVELS entry (100..169): FNV-1a 32 of the five maps right after `sim_load_level(L)` (`g_map_type`,
   `g_map_height`, `g_map_light`, `g_map_flags` over `g_world.cells` = 0x10000, `g_cell_things` as 0x20000 bytes), of the
   GameState image (`state_to_image`, 0x38d03 bytes) right after the load and after 1, 100 and 500 `game_tick_sim()`
   ticks with idle input (`g_hook_player_local_input = nullptr`), the pool size and a hash of the pool extension
   (`thing_to_image` of slots >= 1000; empty in all 140 runs - every level stays at 1000 slots in 500 ticks). Each line
   must equal the golden line. **Result: 140 / 140 identical** (the first differing field would be printed per level,
   and a count per field). It runs before `mode_level_register()` (the campaign path only).
2. **512 and 2048 tiled worlds** (level 38, `world_widen_tiled`; the session 1 tests all use 1024). An archer (class 5
   type 4) walked with `creature_move_step` east and south (220 steps) across the old 255 -> 256 boundary, across the
   new seam N*256 -> 0 and (2048) between two middle tiles follows the 256 track mod 65536 step by step (x, y, z, yaw,
   health), no step longer than a cell, ends one tile further (unwrapped), stays in its cell list every step: 10 walks,
   all identical. Spatial searches (the spiral cell walks of `spatial.cpp`): a searcher just west of the seam / of x =
   0x10000 / (2048) of x = 0x30000 finds the victim 0x80 east across it with `thing_find_collision`,
   `thing_find_collision_other_owner`, `thing_area_damage` slots 0 and 1, `thing_find_mana_ball_touching`,
   `thing_exists_near_pos` (and not at the 256-world alias +65536), `cell_kill_things` - and never the decoy at the
   victim's 256-world alias: 5 cases, all correct.
3. **A 4096 world** (level 38 tiled, 16.7 M cells; terrain at tile (13, 9) equals tile (0, 0) at 64 points).
   Along x at 500, 1000, 2000, 2047, 2100 and 3000 cells (2100 / 3000 = the torus' short way west, 1996 / 1096 cells):
   `world_signed`, `pos_dist_sq_xyz` (exact int64 incl. a z difference), `pos_dist_xyz` (= floor sqrt), `pos_dist_sq_xy`,
   `pos_dist_xy`, `pos_angle_to` (0x200 / 0x600). 21 difference vectors (8 directions, those within half the world):
   `world_dist_sq` exact, `world_isqrt` the floor root, `math_atan2_w` within 1 unit of the true angle (worst 1 of 2048;
   tolerance 2), `pos_angle_to` / `pos_dist_xy` of the same points agree; the 256 world's `(int16_t)` alias of every
   one of the 21 vectors points more than 8 units elsewhere (so the probes do distinguish).
4. **`rts --size 1024 --bots 3`** (seed 1, `MODE_PARAM_DEBUG` as mcport sets it, a generated world through
   `mode_start_run` + `sim_load_level(MODE_LEVEL_INDEX)`), 2000 ticks, twice in one process: identical per-tick
   `net_state_checksum` sequences and map hashes; 4768 live Things at the end (4525 outside the first 256 cells), 4
   wizards; the checksum changes on 1999 of 2000 ticks. ~41 s of the test's 44 s.

`MC_WORLD_TEST_WRITE=<file>` writes the current code's golden lines (same format) for a diff; `MC_WORLD_GOLDEN=<file>`
reads another golden file.

## The golden file and the determinism of the old code

The briefing asked to check the old code is deterministic across two runs in one process first. It is not, as is:
running the 140 levels twice (and in reverse order) in one process gave different hashes for some levels. Causes,
found by diffing the raw GameState per checkpoint and bisecting the level order:

| what carries over from the previous level | effect | reset |
|---|---|---|
| GameState+0x2409.. (the player record backup of the previous level, written at every level load) | every level's state hash after the load (e.g. level 0 after level 169, 110 after 109) | GameState restored to its bytes after `sim_init` |
| `Config.tick` and the `cfg_tick_bit` bytes (Config+0x5e..) | the knock-back shake term of `player_log_position` | Config restored likewise |
| `g_dummy_player_block` (thing.cpp; DAT_000b6e90 in the exe's BSS): Things without a player write into it, and `player_log_position` reads `knock_speed` from it for a player whose Thing is still 0 | level 17's camera-log pitch at tick 1 after Hidden Worlds level 157 left `knock_speed` 0x50 in it | zeroed |
| `g_ai_rand_seed` (ai_wizard.cpp, DAT_0009e5c8, initial 1; the original never resets it) | the AI wizards' choices: level 109 three times in a row gave three t500 hashes | = 1 |
| `g_ai_human_wizard`, `g_projectile_null_hit_index`, `g_terrain_nearly_flat` (the other mutable gameplay globals) | none seen, reset for safety | = 0 |

With these resets before every level the old code gives the same 140 lines (a) twice in one process, (b) in reverse
order, and (c) in a fresh process per level (`MC_DUMP_LIST=<L>` per run), so the golden file does not depend on order.
`world_test` applies the same resets. (This is the original's behaviour too - a campaign played in one session carries
the AI RNG, the dummy block and the record backup from level to level; nothing to fix.)

Format (one line per level, `#` comments allowed, CRLF tolerated):

    <L> type <h> height <h> light <h> flags <h> cells <h> state <h> t1 <h> t100 <h> t500 <h> pool <n> ext <h>

## Regenerating the golden file

From Git Bash; `S` = any scratch directory outside the repository (MSBuild warns MSB8029 if it is under %TEMP%;
harmless). The dumper is a throwaway `mc_unit_test` in the worktree only; it is reproduced below.

```sh
S=<scratch>/round11_F
cd "C:/Magic Carpet" && git worktree add "$S/old" bf77e98
# write $S/old/src/tests/world_dump_test.cpp (below) and
#       $S/old/src/tests/world_dump_test.cmake:  mc_unit_test(world_dump_test tests/world_dump_test.cpp ${MC_SIM_ALL})
cd "$S/old/src" && VCPKG_ROOT=C:/vcpkg cmake --preset msvc-x64 -B "$S/build_old" -DMC_GAME_DIR="C:/Magic Carpet/MagicCarpet/magic"
cmake --build "$S/build_old" --config Release --target world_dump_test --parallel 2 -- -p:CL_MPCount=1
MC_HIDDEN_DIR="C:/Magic Carpet/extracted/gog_cd/CARPET" "$S/build_old/Release/world_dump_test.exe" \
    "C:/Magic Carpet/MagicCarpet/magic" "C:/Magic Carpet/src/tests/data/world_levels_256.txt"
cd "C:/Magic Carpet" && git worktree remove --force "$S/old"
```

(`-DMC_GAME_DIR` is needed because the worktree has no `MagicCarpet/` folder. Without `MC_HIDDEN_DIR` the file has
only levels 0..69 and `world_test` then checks only those.) The whole dump takes ~3 s.

```cpp
// Throwaway (never committed): the golden file of world_test from the pre-round-11 code (commit bf77e98).
// world_dump_test <game dir> <out file>; MC_HIDDEN_DIR = the Hidden Worlds data (levels 100..169).
#define _CRT_SECURE_NO_WARNINGS
#include "sim.h"
#include "thing.h"
#include "player.h"
#include "world_set.h"
#include "ai_wizard.h"
#include "projectiles.h"
#include "terrain_paint.h"
#include "mc_globals.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 3 || !sim_init(argv[1])) return 1;
    sim_register_gameplay();
    g_hook_player_local_input = nullptr;
    const char *hd = std::getenv("MC_HIDDEN_DIR");
    if (hd) world_set_init(argv[1], hd);
    const std::vector<uint8_t> state0(reinterpret_cast<uint8_t *>(g_state), reinterpret_cast<uint8_t *>(g_state) + sizeof(GameState));
    const std::vector<uint8_t> cfg0(reinterpret_cast<uint8_t *>(g_cfg), reinterpret_cast<uint8_t *>(g_cfg) + sizeof(Config));
    std::vector<int> levels;
    for (int L = 0; L < 70; L++) levels.push_back(L);
    if (hd && world_set_hidden_available())
        for (int k = 0; k < HW_LEVEL_COUNT; k++) levels.push_back(HW_LEVEL_BASE + k);
    FILE *out = std::fopen(argv[2], "wb");
    if (!out) return 1;
    std::fprintf(out, /* the 8 '#' header lines of world_levels_256.txt, verbatim */ "...");
    for (int L : levels) {
        std::memcpy(g_state, state0.data(), state0.size());
        std::memcpy(g_cfg, cfg0.data(), cfg0.size());
        std::memset(g_dummy_player_block, 0, sizeof g_dummy_player_block);
        g_ai_rand_seed = 1;
        g_ai_human_wizard = 0;
        g_projectile_null_hit_index = 0;
        g_terrain_nearly_flat = 0;
        if (!sim_load_level(L)) { std::fprintf(out, "%d none\n", L); continue; }
        const uint32_t ty = fnv(g_map_type, MC_MAP_CELLS), he = fnv(g_map_height, MC_MAP_CELLS),
                       li = fnv(g_map_light, MC_MAP_CELLS), fl = fnv(g_map_flags, MC_MAP_CELLS),
                       ct = fnv(g_cell_things, MC_MAP_CELLS * 2), s0 = fnv(g_state, sizeof(GameState));
        uint32_t t[3];
        int n = 0;
        const int at[3] = {1, 100, 500};
        for (int k = 0; k < 3; k++) {
            while (n < at[k]) { game_tick_sim(); n++; }
            t[k] = fnv(g_state, sizeof(GameState));
        }
        uint32_t ext = 2166136261u;
        for (int i = MC_THING_SLOTS; i < thing_pool_slots(); i++) ext = fnv(thing_at((unsigned)i), sizeof(Thing), ext);
        std::fprintf(out, "%d type %08x height %08x light %08x flags %08x cells %08x state %08x t1 %08x t100 %08x t500 %08x pool %d ext %08x\n",
                     L, ty, he, li, fl, ct, s0, t[0], t[1], t[2], thing_pool_slots(), ext);
    }
    std::fclose(out);
    return 0;
}
```

(Copy the 8 header lines of the current golden file into the `fprintf` as string literals, each ending in `\n`; this
exact program, with them, reproduced the committed file byte for byte. In the old code `Thing` is the original's
0xa4-byte layout, so hashing `g_state` raw is the image.)

To compare the current code instead: `MC_WORLD_TEST_WRITE=new.txt build_F/Release/world_test.exe <game>` and diff
`new.txt` with the golden file minus its `#` lines.

## Deviations / open points

- The golden comparison covers 500 ticks per level with idle input (the human wizard does nothing; the computer
  wizards, creatures and spawners run). Longer runs and real input are covered by the per-tick references
  (`reference_test`, movie 0 + 8 levels) - together they pin the 256 world.
- The `ext` column is the hash of an empty extension in all 140 lines (no level fills 1000 slots in 500 idle ticks);
  it is there so a run that does would be compared too.
- `golden()` re-calls `world_set_init(game, hidden)` - harmless for the later parts (each loads its own level).

## What later work must know

- A test that loads several campaign levels in one process and compares hashes must reset the five globals above
  (or load each level in a fresh process): `g_ai_rand_seed` in particular is a cross-level dependency of the AI.
- `world_test` takes ~44 s in Release, ~41 s of it the two 2000-tick 1024 runs; lower `ticks` in `rts_twice()` if the
  gate gets too slow.
