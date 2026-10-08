// Engine glue: initialisation of the renderer side, default camera, the per-tick driver. The
// simulation side (level loading, handler registration) is sim.cpp so it can be linked without the
// renderer.
#include "engine.h"
#include "sim.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "tables.h"
#include "terrain.h"
#include "thing.h"
#include "player.h"
#include "sprites.h"
#include "demo.h"
#include "hud.h"
#include "ui_draw.h"
#include "world_set.h"
#include "worldgen.h"
#include "mcfile.h"
#include "rnc.h"
#include "gen/terrain_tables.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

const char *engine_game_dir() { return sim_game_dir(); }

// port: the renderer / HUD side of a data-set switch (world_set_select, Hidden Worlds): everything
// engine_init loaded from a set-specific file, including the shade / blend tables (HIDDEN.EXE loads
// data/dtables.dat, built from its palette, instead of data/tables.dat).
static void engine_world_set_changed(int /*set*/) {
    world_art_apply(false);                     // the overlay belongs to the base set's atlas
    const char *dir = sim_game_dir();
    if (!tables_load_palette(dir))  std::fprintf(stderr, "engine: palette missing for the data set\n");
    if (!tables_load_textures(dir)) std::fprintf(stderr, "engine: texture atlas missing for the data set\n");
    if (!tables_load_sky(dir))      std::fprintf(stderr, "engine: sky missing for the data set\n");
    tables_load_or_generate(dir);               // after palette + atlas: a missing file is regenerated from them
    sprites_shutdown();
    if (!sprites_init(dir))         std::fprintf(stderr, "engine: tmaps missing for the data set\n");
    ui_draw_reload_set_tables();
}

bool engine_init(const char *game_dir) {
    if (!sim_init(game_dir)) return false;
    if (!tables_load_palette(game_dir))  { std::fprintf(stderr, "engine: data/palette.dat missing in %s\n", game_dir); return false; }
    if (!tables_load_textures(game_dir)) { std::fprintf(stderr, "engine: data/block%d.dat missing\n", g_state->texture_block_size); return false; }
    if (!tables_load_or_generate(game_dir)) { std::fprintf(stderr, "engine: tables.dat load/generate failed\n"); return false; }
    if (!tables_load_sky(game_dir))      { std::fprintf(stderr, "engine: data/sky.dat missing\n"); return false; }
    if (!sprites_init(game_dir))         { std::fprintf(stderr, "engine: sprite cache init failed\n"); return false; }
    sim_register_gameplay();
    render_things_install();
    ui_draw_init(game_dir);                     // fonts, HUD sprites, pointers (missing files: HUD stays blank)
    g_hook_frame_state = hud_tick_state;
    g_hook_world_set_changed = engine_world_set_changed;
    g_hook_level_art = world_art_apply;
    g_hook_demo_textures_reload = texture_load_needed;
    g_hook_demo_textures_mark = texture_mark_needed;      // texture_mark_needed_4c130 when a recording starts
    return true;
}

void engine_shutdown() { world_art_apply(false); ui_draw_shutdown(); sprites_shutdown(); mc_globals_shutdown(); }

// ---- port (Phase 4): the art of a generated Conquest world (engine.h) ----------------------------------------
namespace {
struct WorldArt {
    bool     on = false;
    uint8_t *base = nullptr;        // the data set's atlas while the patched copy is shown
    uint8_t *patched = nullptr;     // g_texture_atlas while on
    uint8_t  avg[0x180] = {};       // the tables' per-texture average colours before
};
WorldArt s_art;

std::string hidden_path(const char *rel) { return std::string(world_set_hidden_dir()) + "/" + rel; }

// Nearest colour of PAL (6-bit) for every entry of FROM (weights 2 / 3 / 1 as render_ext's colour cube).
void palette_remap(const uint8_t *from, const uint8_t *pal, uint8_t out[256]) {
    for (int i = 0; i < 256; i++) {
        int best = 1 << 30, bi = 0;
        for (int j = 0; j < 256; j++) {
            const int dr = pal[j * 3] - from[i * 3], dg = pal[j * 3 + 1] - from[i * 3 + 1], db = pal[j * 3 + 2] - from[i * 3 + 2];
            const int d = 2 * dr * dr + 3 * dg * dg + db * db;
            if (d < best) { best = d; bi = j; }
        }
        out[i] = (uint8_t)bi;
    }
}

void fill_texture_table(const uint8_t *atlas, int B) {
    const int cols = 256 / B;
    for (int t = 0; t < 256; t++) g_texture_table[t] = atlas + (size_t)(t / cols) * B * 256 + (size_t)(t % cols) * B;
}

// Hidden Worlds' broadleaf tree: tmaps1-0 chunk 140, pixels remapped (0 stays transparent).
bool load_oak(const uint8_t remap[256]) {
    mc_blob tab{}, dat{};
    if (!mc_read_file(hidden_path("data/tmaps1-0.tab").c_str(), &tab)) return false;
    bool ok = false;
    if (tab.len >= 141 * 10 && mc_read_file(hidden_path("data/tmaps1-0.dat").c_str(), &dat)) {
        const uint8_t *e = tab.data + 140 * 10;
        const uint32_t size = (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24;
        const uint32_t off = (uint32_t)e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24;
        if (off < dat.len) {
            size_t len = 0;
            int err = 0;
            uint8_t *raw = rnc_is_rnc(dat.data + off, dat.len - off)
                ? rnc_unpack_alloc(dat.data + off, dat.len - off, &len, &err) : nullptr;
            if (raw && len >= 6 && len >= size) {
                const size_t w = (size_t)raw[2] | (size_t)raw[3] << 8, h = (size_t)raw[4] | (size_t)raw[5] << 8;
                if (6 + w * h <= len) {
                    raw[0] &= 0xfe;                     // never animated
                    for (size_t i = 0; i < w * h; i++) if (raw[6 + i]) raw[6 + i] = remap[raw[6 + i]];
                    ok = sprite_port_set(MC_SPRITE_OAK, raw, 6 + w * h);
                }
            }
            std::free(raw);
        }
        mc_blob_free(&dat);
    }
    mc_blob_free(&tab);
    return ok;
}
}

bool world_art_active() { return s_art.on; }

void world_art_apply(bool on) {
    if (s_art.on) {                             // off first (also before a fresh on)
        const int B = (int8_t)g_state->texture_block_size;
        if (g_texture_atlas == s_art.patched) {
            g_texture_atlas = s_art.base;
            fill_texture_table(g_texture_atlas, B);
            std::memcpy(g_tex_avg_colour(), s_art.avg, sizeof s_art.avg);
            std::free(s_art.patched);
        } else {
            std::free(s_art.base);              // the atlas was reloaded meanwhile (it freed the patched copy)
        }
        s_art = WorldArt{};
        sprite_port_set(MC_SPRITE_OAK, nullptr, 0);
        g_sprite_desc[MC_SPRITE_DESC_OAK].base_sprite = g_sprite_desc[0x53].base_sprite;   // the palm
    }
    if (!on || world_set() != 0 || !world_set_hidden_available() || !g_texture_atlas) return;
    uint8_t pal1[768];
    if (mc_load_rnc_into(hidden_path("data/pal1-0.dat").c_str(), pal1, sizeof pal1) != (long)sizeof pal1) return;
    uint8_t remap[256];
    palette_remap(pal1, g_palette6, remap);
    const int B = (int8_t)g_state->texture_block_size;
    if (B != 16 && B != 32) return;
    mc_blob blk;
    if (!mc_read_unpacked(hidden_path(B == 32 ? "data/blk1-1.dat" : "data/blk1-0.dat").c_str(), &blk)) return;
    // the same allocation as tables_load_textures (file or full geometry + 32 rows of slack)
    const size_t geom = (size_t)B * B * 256 + 32u * 256u;
    const size_t alloc = g_texture_atlas_size > geom ? g_texture_atlas_size : geom;
    uint8_t *patched = static_cast<uint8_t *>(std::malloc(alloc));
    if (!patched) { mc_blob_free(&blk); return; }
    std::memcpy(patched, g_texture_atlas, alloc);
    const int cols = 256 / B;
    int tiles = 0;
    for (int t = 0; t < 0x94; t++) {
        const int8_t *c = g_corner_class_tuples + t * 4;
        if (c[0] < 0 || (c[0] != 6 && c[1] != 6 && c[2] != 6 && c[3] != 6)) continue;
        const size_t o = (size_t)(t / cols) * B * 256 + (size_t)(t % cols) * B;
        if (o + (size_t)(B - 1) * 256 + B > blk.len) continue;
        for (int y = 0; y < B; y++)
            for (int x = 0; x < B; x++) patched[o + (size_t)y * 256 + x] = remap[blk.data[o + (size_t)y * 256 + x]];
        tiles++;
    }
    mc_blob_free(&blk);
    s_art.on = true;
    s_art.base = g_texture_atlas;
    s_art.patched = patched;
    std::memcpy(s_art.avg, g_tex_avg_colour(), sizeof s_art.avg);
    g_texture_atlas = patched;
    fill_texture_table(patched, B);
    texture_average_colours(g_palette6, g_tex_avg_colour());
    if (load_oak(remap)) g_sprite_desc[MC_SPRITE_DESC_OAK].base_sprite = MC_SPRITE_OAK;
    std::fprintf(stderr, "world art: %d snow tiles%s from Hidden Worlds\n", tiles,
                 g_sprite_desc[MC_SPRITE_DESC_OAK].base_sprite == MC_SPRITE_OAK ? " and the oak" : "");
}

bool engine_load_level(int index) { return sim_load_level(index); }
bool engine_load_snapshot(const char *gam_rel, const char *map_rel) { return sim_load_snapshot(gam_rel, map_rel); }

Camera engine_default_camera() {
    Camera c{};
    // First player-class THING_INIT of the level, else the map centre.
    int cx = 128, cy = 128;
    for (const ThingInit &t : g_state->level.things) {
        if (t.cls == 3) { cx = t.x; cy = t.y; break; }
    }
    // The demo snapshot (movie/gam00000.dat) has the player at cell centre, z = 0x100, and the
    // position-history entries carry zoom 0x80, which is therefore the default focal scale.
    c.cam_x = cx * 256 + 128;
    c.cam_y = cy * 256 + 128;
    c.yaw = 0;
    int h = g_map_height[mc_cell(cx, cy)] * 0x20;
    c.cam_z = h + 0x180;
    c.pitch = 0;
    c.roll = 0;
    c.zoom = 0x80;
    return c;
}

// The per-tick part of game_tick_update_32e80 that is ported: texture / sprite animation, then the
// simulation (command packets from the movie when one is playing), then the water phase.
bool engine_tick() {
    bool more = true;
    if (!(g_cfg->paused & 1)) texture_anim_update();
    if (demo_playing()) more = demo_step();
    else game_tick_sim();
    g_anim_tick = g_state->players[g_state->local_player & 7].tick;
    return more;
}
