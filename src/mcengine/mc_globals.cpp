#include "mc_globals.h"
#include "mc_math.h"
#include "settings.h"
#include "thing.h"
#include "world.h"
#include <cstdlib>
#include <cstring>

PortSettings g_settings;               // settings.h (port-only; default = the original's behaviour)

static bool          s_rules_forced = false;
static GameplayRules s_rules_forced_value;
GameplayRules gameplay_rules() {
    GameplayRules r;
    if (s_rules_forced) r = s_rules_forced_value;
    else r.possession_range_pct = g_settings.possession_range_pct;
    if (r.possession_range_pct < 100) r.possession_range_pct = 100;
    if (r.possession_range_pct > 200) r.possession_range_pct = 200;
    if (r.mode < 0 || r.mode > 255) r.mode = 0;
    return r;
}
void gameplay_force_rules(const GameplayRules *rules) {
    s_rules_forced = rules != nullptr;
    if (rules) s_rules_forced_value = *rules;
}
bool gameplay_rules_forced() { return s_rules_forced; }
int    g_thing_slots = MC_THING_SLOTS;  // thing.h: Thing pool size (round 7, task C; thing.cpp sizes it)
Thing *g_thing_ext   = nullptr;         // thing.h: slots >= MC_THING_SLOTS

GameState *g_state = nullptr;
Config    *g_cfg   = nullptr;

World   g_world;                        // world.h (round 11)
WorldHi g_world_hi;

namespace {
uint8_t  s_map_type256[MC_MAP_CELLS];
uint8_t  s_map_height256[MC_MAP_CELLS];
uint8_t  s_map_light256[MC_MAP_CELLS];
uint8_t  s_map_flags256[MC_MAP_CELLS];
uint16_t s_cell_things256[MC_MAP_CELLS];
uint8_t  *s_map_wide = nullptr;         // one block for a wider world: 4 byte maps + the cell heads
}
uint8_t  *g_map_type    = s_map_type256;
uint8_t  *g_map_height  = s_map_height256;
uint8_t  *g_map_light   = s_map_light256;
uint8_t  *g_map_flags   = s_map_flags256;
uint16_t *g_cell_things = s_cell_things256;

bool world_set_size(int n) {
    if (n < MC_WORLD_MIN || n > MC_WORLD_MAX || (n & (n - 1))) return false;
    if (n == g_world.size) return true;
    int shift = 0;
    while ((1 << shift) < n) shift++;
    const uint32_t cells = (uint32_t)n * (uint32_t)n;
    std::free(s_map_wide);
    s_map_wide = nullptr;
    if (n == 256) {
        g_map_type = s_map_type256; g_map_height = s_map_height256; g_map_light = s_map_light256;
        g_map_flags = s_map_flags256; g_cell_things = s_cell_things256;
    } else {
        s_map_wide = static_cast<uint8_t *>(std::calloc((size_t)cells, 6));
        if (!s_map_wide) { world_set_size(256); return false; }
        g_map_type = s_map_wide;
        g_map_height = s_map_wide + cells;
        g_map_light = s_map_wide + 2 * (size_t)cells;
        g_map_flags = s_map_wide + 3 * (size_t)cells;
        g_cell_things = reinterpret_cast<uint16_t *>(s_map_wide + 4 * (size_t)cells);
    }
    g_world.size = n;
    g_world.shift = shift;
    g_world.cell_mask = (uint32_t)n - 1;
    g_world.units = n * MC_CELL_UNITS;
    g_world.unit_mask = (uint32_t)g_world.units - 1;
    g_world.cells = cells;
    g_world_unit_mask = g_world.unit_mask;
    std::memset(g_map_type, 0, cells);
    std::memset(g_map_height, 0, cells);
    std::memset(g_map_light, 0, cells);
    std::memset(g_map_flags, 0, cells);
    std::memset(g_cell_things, 0, (size_t)cells * 2);
    return true;
}

uint32_t world_isqrt(int64_t v) {
    if (v <= 0xffffffffll) return mc_isqrt((uint32_t)v);
    uint64_t r = ((uint64_t)mc_isqrt((uint32_t)(v >> 32)) + 1) << 16;  // above the root: Newton decreases to floor
    for (;;) {
        const uint64_t q = (uint64_t)v / r;
        if (q >= r) return (uint32_t)r;
        r = (r + q) >> 1;
    }
}

uint8_t  g_work_buf[MC_WORK_SIZE];
uint8_t *g_frame2 = nullptr;

uint8_t  g_tables_image[MC_TABLES_SIZE];
uint8_t  g_palette6[768];
uint8_t *g_texture_atlas = nullptr;
size_t   g_texture_atlas_size = 0;
const uint8_t *g_texture_table[256];
uint8_t  g_sky[65536];
int32_t  g_uv_table[256];

uint16_t g_rng16 = 0;
uint16_t g_video_mode_flags = 8;

const QuadStep *mc_quad_steps() { return reinterpret_cast<const QuadStep *>(g_quad_steps_data); }
SpriteDesc g_sprite_desc[MC_SPRITE_DESC_COUNT];
const MoveDesc *mc_move_desc(unsigned i) {
    return reinterpret_cast<const MoveDesc *>(g_move_desc_data) + (i < 30 ? i : 0);
}

void mc_globals_init() {
    if (!g_state) g_state = static_cast<GameState *>(std::calloc(1, sizeof(GameState)));
    if (!g_cfg)   g_cfg   = static_cast<Config *>(std::calloc(1, sizeof(Config)));
    std::memset(g_map_type, 0, g_world.cells);
    std::memset(g_map_height, 0, g_world.cells);
    std::memset(g_map_light, 0, g_world.cells);
    std::memset(g_map_flags, 0, g_world.cells);
    std::memset(g_cell_things, 0, g_world.cells * sizeof(uint16_t));
    std::memset(g_work_buf, 0, sizeof g_work_buf);
    std::memcpy(g_uv_table, g_uv_table_data, sizeof g_uv_table);
    std::memcpy(g_sprite_desc, g_sprite_desc_data, sizeof g_sprite_desc);
    // config_parse_33750 defaults
    g_state->view_size = 0x28;
    g_state->opt_shadows = g_state->opt_hud_a = g_state->opt_hud_b = 1;
    g_state->opt_second_surface = g_state->opt_textured_sky = 1;   // = cfg->pentium on a Pentium
    g_state->texture_block_size = 0x20;
    for (auto &o : g_state->opt_allowed) o = 1;
    g_state->player_count = 2;
    g_cfg->pentium = 1;
    g_cfg->pit_reload = 0x2726;
}

void mc_globals_shutdown() {
    std::free(g_state); g_state = nullptr;
    std::free(g_cfg);   g_cfg = nullptr;
    std::free(g_texture_atlas); g_texture_atlas = nullptr; g_texture_atlas_size = 0;
}
