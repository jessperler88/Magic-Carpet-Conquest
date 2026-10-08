// Simulation-side glue without the renderer: globals, data files, handler registration of the stable
// subsystems, level loading (level_load_and_init_3d3b0 without the front end, sound and network) and
// the demo recorder's snapshot pair.
#include "sim.h"
#include "text.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "terrain.h"
#include "thing.h"
#include "spatial.h"
#include "level_features.h"
#include "constructors.h"
#include "player.h"
#include "demo.h"
#include "mode.h"
#include "world_set.h"
#include "world.h"
#include "worldgen.h"
#include "mcfile.h"
#include "rnc.h"
#include <cstdio>
#include <cstring>
#include <string>

static std::string s_game_dir;

const char *sim_game_dir() { return s_game_dir.c_str(); }

// port: the simulation's data from set-specific files after a data-set switch (world_set.h): the
// sprite extents sprite_table_init_sizes_4bd10 completes from tmaps (from the exe table again) and the
// castle footprints (building.*).
static void sim_world_set_changed(int /*set*/) {
    std::memcpy(g_sprite_desc, g_sprite_desc_data, sizeof g_sprite_desc);
    sprite_table_init_sizes(s_game_dir.c_str());
    level_features_load_data(s_game_dir.c_str());
}

bool sim_init(const char *game_dir) {
    s_game_dir = game_dir;
    mc_globals_init();
    world_set_init(game_dir, nullptr);                  // port: base data set; Hidden Worlds dir = <game>/hidden
    g_hook_world_set_sim = sim_world_set_changed;
    if (!sprite_table_init_sizes(game_dir)) { std::fprintf(stderr, "sim: data/tmaps.dat missing in %s\n", game_dir); return false; }
    if (!level_features_load_data(game_dir)) { std::fprintf(stderr, "sim: data/building.* or search.dat missing\n"); return false; }
    text_load(game_dir, g_cfg->language & 3);           // the on-screen notices (missing file: empty strings)
    // Bind the translated Thing handlers to the class tables (one call per subsystem) and connect
    // the hooks between subsystems.
    level_features_register_handlers();
    constructors_register_handlers();
    player_register_handlers();
    g_hook_castle_stamp_footprint = castle_stamp_footprint;
    g_hook_thing_set_castle_extents = thing_set_castle_extents;
    g_hook_player_note_ridge_distance = player_note_ridge_distance;
    return true;
}

bool (*g_hook_level_source)(int index, LevelData *out) = nullptr;

namespace {
// level_load_file_3d160: the per-level reset.
void level_reset_state() {
    {   // the Config part (lists rebuilt per tick; stale heads of a previous level reach player_spawn)
        reinterpret_cast<uint8_t *>(g_state)[0x244] = 0;
        uint8_t *cf = reinterpret_cast<uint8_t *>(g_cfg);
        g_cfg->fade_stage = 0;
        std::memset(cf + 0x5d, 0, 0x10);
        g_cfg->substeps = 0;
        g_cfg->palette_effect = 0;
        std::memset(cf + 0xb8, 0, 0xe);
        std::memset(cf + 0x8e1a, 0, 4);
        std::memset(g_cfg->creature_lists, 0, sizeof g_cfg->creature_lists);
        g_cfg->player_list = g_cfg->mana_ball_list = g_cfg->wizard_list = g_cfg->projectile_list = 0;
        g_cfg->flags &= 0x3fff;
        g_cfg->paused &= ~1;
    }
    std::memset(&g_state->padc, 0, 0x28 - 0xc);   // +0xc..+0x27: serials, volcano_thing, volcano_smoke
    g_state->free_top = 0;
    // Port: both Thing stacks cleared (the original leaves the previous level's entries above the tops; nothing
    // reads them, but net_state_checksum hashes the whole arrays, so a second level in one process - or peers with
    // different histories - would differ from tick 0). A fresh process has them zero, as the references do.
    std::memset(g_state->free_list, 0, sizeof g_state->free_list);
    std::memset(g_state->active_list, 0, sizeof g_state->active_list);
    std::memset(g_state->things, 0, sizeof g_state->things);
    g_state->creature_count = 0;
    std::memset(g_state->spells_present, 0, sizeof g_state->spells_present);
    std::memset(g_state->start_pos, 0, sizeof g_state->start_pos);
    std::memset(&g_world_hi, 0, sizeof g_world_hi);
    world_set_size(256);                         // round 11: every level starts as the original's world (world.h)
    level_extra_records_clear();                 // round 11: a generated world's records beyond the level file's
    std::memset(g_cell_things, 0, g_world.cells * sizeof(uint16_t));
    std::memset(g_map_type, 0, g_world.cells);
    std::memset(g_map_height, 0, g_world.cells);
    std::memset(g_map_light, 0, g_world.cells);
    std::memset(g_map_flags, 0, g_world.cells);
    std::memset(g_work_buf, 0, 64000);
}

// level_load_and_init_3d3b0 from "Generate map" on, with g_state->level filled.
void level_start_from_state(int index) {
    // port (round 11): a network mode run plays humans + bots (mode_build_level's player_count), not the session's count
    if (!(g_cfg->flags & 0x10) || mode_active()) g_state->player_count = g_state->level.player_count;
    g_cfg->level = (uint16_t)index;
    // port (Phase 4 round 11): a mode level with a generated world is built by worldgen.cpp (world size, maps and
    // the placed THING_INIT records); the campaign and template mode levels go through terrain_build
    const bool generated = g_hook_level_world && g_hook_level_world();
    if (!generated) terrain_build(g_state->level.gen);
    if (g_hook_level_art) g_hook_level_art(generated);     // port: render-only art of a generated world (engine.h)
    thing_pool_reset();
    terrain_generate_features();                 // "Generate features"
    std::memset(&g_pos_scratch, 0, sizeof g_pos_scratch);
    models_initialise();                         // "Initialise Models"
    g_state->active_top = -1;
    switch_activate(0, true);
    players_init_records();
    if (mode_active()) mode_level_start();       // port (Phase 4): the mode block's level start
    if (g_hook_mana_totals_update)
        g_hook_mana_totals_update(thing_at(g_state->players[g_state->local_player & 7].thing));
}
} // namespace

bool sim_load_level_data(const LevelData &data, int index) {
    level_reset_state();
    std::memcpy(&g_state->level, &data, sizeof(LevelData));
    level_start_from_state(index);
    return true;
}

// level_load_file_3d160 (the per-level reset) + level_load_levels_dat_3bfc0 + "Generate map".
bool sim_load_level(int index) {
    if (g_hook_level_source) {
        static LevelData s_src;
        if (g_hook_level_source(index, &s_src)) { world_set_select(0); return sim_load_level_data(s_src, index); }
    }
    // port: Hidden Worlds campaign levels (world_set.h) load DDLEVELS with the set-1 data files.
    if (!world_set_select(world_set_for_level(index))) return false;
    const int file_index = world_level_file_index(index);
    char path[1024];
    mc_blob tab, dat;
    mc_path_join(path, sizeof path, s_game_dir.c_str(), "levels/levels.tab");
    if (!mc_read_file(path, &tab)) return false;
    mc_path_join(path, sizeof path, s_game_dir.c_str(), "levels/levels.dat");
    if (!mc_read_file(path, &dat)) { mc_blob_free(&tab); return false; }
    bool ok = false;
    size_t n = tab.len / 4;
    if (file_index >= 0 && (size_t)file_index + 1 < n) {
        uint32_t a, b;
        std::memcpy(&a, tab.data + file_index * 4, 4);
        std::memcpy(&b, tab.data + (file_index + 1) * 4, 4);
        if (b <= a) b = (uint32_t)dat.len;
        if (a < dat.len && b <= dat.len) {
            level_reset_state();
            long got = rnc_unpack(dat.data + a, b - a, reinterpret_cast<uint8_t *>(&g_state->level), sizeof(LevelData));
            if (got == (long)sizeof(LevelData)) {
                level_start_from_state(index);
                ok = true;
            } else {
                std::fprintf(stderr, "sim: level %d unpack returned %ld\n", index, got);
            }
        }
    }
    mc_blob_free(&tab);
    mc_blob_free(&dat);
    return ok;
}

// The demo recorder's snapshot pair (demo_load_state_3c200 / demo_load_terrain_3c360): raw GameState
// + the five maps and the generator state block.
bool sim_load_snapshot(const char *gam_rel, const char *map_rel) {
    char path[1024];
    mc_blob gam, map;
    mc_path_join(path, sizeof path, s_game_dir.c_str(), gam_rel);
    if (!mc_read_file(path, &gam)) return false;
    mc_path_join(path, sizeof path, s_game_dir.c_str(), map_rel);
    if (!mc_read_file(path, &map)) { mc_blob_free(&gam); return false; }
    bool ok = gam.len == MC_STATE_IMAGE_SIZE && map.len >= 0x60000;
    if (ok) {
        std::memcpy(g_map_type, map.data, 0x10000);
        std::memcpy(g_map_height, map.data + 0x10000, 0x10000);
        std::memcpy(g_map_light, map.data + 0x20000, 0x10000);
        std::memcpy(g_map_flags, map.data + 0x30000, 0x10000);
        std::memcpy(g_cell_things, map.data + 0x40000, 0x20000);
        // The 0x12c2-byte tail is the compact corner-class texture table (0xb58b0), which the
        // run-time retexture functions need.
        if (map.len >= 0x60000 + sizeof g_corner_tex_table)
            std::memcpy(g_corner_tex_table, map.data + 0x60000, sizeof g_corner_tex_table);
        ok = thing_load_snapshot_image(gam.data, g_state);
        thing_pool_ext_reset();     // a 1000-slot image: the wanted pool size, empty extension (thing.h)
    }
    mc_blob_free(&gam);
    mc_blob_free(&map);
    return ok;
}
