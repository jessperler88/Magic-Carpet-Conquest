// Per-tick comparison of the port against the original game (task E of round 4).
//
// The reference is extracted/reference/movie0/tick%05d.gam: the raw GameState of the retail carpet.exe
// (patched copy, tools/reference/patch_carpet.py) taken in DOSBox while it plays movie 0, after
// thing_update_all and before sound_update_494b0 of every tick 413..1013 and every 10th tick up to
// 8960, plus tick%05d.map (demo_save_terrain layout) every 100 ticks. See docs/analysis/port_reference.md.
//
// This test plays the same movie in the port (sim_init, sim_register_gameplay, sim_prepare_movie,
// demo_open, demo_step - as sim_test) and, at every tick that has a dump, converts the dump's
// pointers to the port's index form (thing_relink_snapshot, as for the shipped snapshot) and compares:
//   * every Thing slot that is live in either state: the first tick and the first differing field of
//     each slot (list links next / cell_next / cell_prev reported separately), per-class counts;
//   * GameState.rng, each PlayerRec (first differing offset), the maps at the terrain ticks.
// Output: a per-tick summary line, then the per-slot first divergences and per-class first ticks.
// Optional CSV of the per-slot table: argv[4].
//
// argv: [1] game dir  [2] reference dir (default MC_REFERENCE_DIR)  [3] last tick (default: all)  [4] csv
// Exit code: 1 on any divergence or unported handler (the movie-0 gate), 0 otherwise or when the
// reference is missing (SKIP).
//
// Level mode (round 5, tools/reference/run_level.py): when <reference dir>/index.json names a "level",
// the reference is a recording of that campaign level made by the original from its first ticks with
// an idle local player (<ref>/movie/mvi<movie>.dat + the snapshot pair of tick 2) and the dumps of
// its playback (tick00002..). The port then does what the original's playback run does: 320x200 game
// logic, sim_load_level(level), demo_open(<ref>, movie), demo_step per tick. Afterwards the level
// generation itself is checked: a fresh sim_load_level(level) + one game_tick_sim() (idle local
// input) against tick00001.gam, the original's state after the first tick of the generated level.
//
// 1995 references (tools/reference/run_level_hw.py, docs/analysis/port_reference_hw.md): HIDDEN.EXE /
// the CD's CARPET.EXE dump 0x38d09-byte states (6 bytes after spells_present, zero in the dumps); the
// first 0x38d03 bytes are compared, a 0x38d09-byte snapshot is played from a truncated scratch copy, and
// index.json "port_level" (100 + DDLEVELS entry) is the level the port loads. Suite mode runs them only
// in a directory without 1996 references (extracted/reference/hw_gen, cd95_gen).
//
// Wide world (round 11 session 3, docs/analysis/port_world_refwide.md): MC_REF_WORLD=N (512 .. 4096, a power of
// two) widens the world with world_widen_tiled(N) right after the snapshot pair is loaded (the first tick of the
// playback; the generation check: after sim_load_level) and plays on in the tiled N-cell world. The original's
// cell-list order is restored after the widening (world_widen_tiled relinks in slot order; MC_REF_WORLD_RELINK=slot
// keeps that), so up to the first interaction across the old 255 -> 256 boundary the low halves the comparison
// sees must stay identical. The maps are compared in tile (0,0), the cell-list heads folded over the tiles. Extra
// output: the first tick a Thing leaves tile (0,0) and, at the first divergent tick, where the diverging Things are.
// Unset: the program behaves exactly as without it.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "sim.h"
#include "thing.h"
#include "player.h"
#include "demo.h"
#include "hud.h"
#include "projectiles.h"
#include "mc_globals.h"
#include "world.h"
#include "mcfile.h"
#include "crash_handler.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef MC_REFERENCE_DIR
#define MC_REFERENCE_DIR MC_REPO_DIR "/extracted/reference/movie0"
#endif

static const char *kClassName[14] = { "-", "c1", "scenery", "player", "c4", "creature", "c6", "c7", "c8",
                                      "projectile", "effect", "switch", "spell", "c13" };
static const char *cls_name(int c) { return c >= 0 && c < 14 ? kClassName[c] : "?"; }

// Thing field table (mc_types.h).
struct Field { int off, size; const char *name; };
static const Field kFields[] = {
    {0x00, 4, "next"}, {0x04, 4, "rng"}, {0x08, 4, "max_health"}, {0x0c, 4, "health"}, {0x10, 4, "flags"},
    {0x14, 2, "cell_next"}, {0x16, 2, "cell_prev"}, {0x18, 2, "owner"}, {0x1a, 2, "aux"}, {0x1c, 2, "prop_flags"},
    {0x1e, 2, "yaw"}, {0x20, 2, "pitch"}, {0x22, 2, "target_yaw"}, {0x24, 2, "target_pitch"}, {0x26, 2, "killer"},
    {0x28, 2, "last_attacker"}, {0x2a, 2, "caster"}, {0x2c, 2, "damage"}, {0x2e, 2, "z_vel"}, {0x30, 2, "cast_ticks"},
    {0x32, 2, "duration"}, {0x34, 2, "parent"}, {0x36, 2, "child"}, {0x38, 2, "speed"}, {0x3a, 1, "timer_a"},
    {0x3b, 1, "timer_b"}, {0x3c, 1, "spell_flags"}, {0x3d, 1, "burst"}, {0x3e, 1, "unk3e"}, {0x3f, 1, "tick"},
    {0x40, 1, "cls"}, {0x41, 1, "type"}, {0x42, 1, "filter_cls"}, {0x43, 1, "filter_type"}, {0x44, 1, "impact_cls"},
    {0x45, 1, "impact_type"}, {0x46, 1, "state"}, {0x47, 1, "castle_size"}, {0x48, 2, "x"}, {0x4a, 2, "y"},
    {0x4c, 2, "z"}, {0x4e, 2, "ext_z0"}, {0x50, 2, "ext_x"}, {0x52, 2, "ext_y"}, {0x54, 2, "ext_h"},
    {0x56, 2, "sprite"}, {0x58, 1, "frame"}, {0x59, 1, "draw_type"},
    {0x5a, 4, "dmg0.amount"}, {0x5e, 2, "dmg0.attacker"}, {0x60, 4, "dmg1.amount"}, {0x64, 2, "dmg1.attacker"},
    {0x66, 4, "dmg2.amount"}, {0x6a, 2, "dmg2.attacker"}, {0x6c, 4, "dmg3.amount"}, {0x70, 2, "dmg3.attacker"},
    {0x72, 4, "dmg4.amount"}, {0x76, 2, "dmg4.attacker"}, {0x78, 4, "dmg5.amount"}, {0x7c, 2, "dmg5.attacker"},
    {0x7e, 2, "speed_cur"}, {0x80, 2, "speed_base"}, {0x82, 2, "turn_rate"}, {0x84, 4, "mana_cost"},
    {0x88, 4, "mana_total"}, {0x8c, 4, "mana"}, {0x90, 2, "mana_owner"}, {0x92, 2, "target"}, {0x94, 2, "unk94"},
    {0x96, 2, "home.x"}, {0x98, 2, "home.y"}, {0x9a, 2, "home.z"}, {0x9c, 4, "desc"}, {0xa0, 4, "player"},
};
static const Field *field_at(int off) {
    for (const Field &f : kFields) if (off >= f.off && off < f.off + f.size) return &f;
    return nullptr;
}
static long field_value(const uint8_t *p, const Field &f) {
    if (f.size == 1) return p[f.off];
    if (f.size == 2) { uint16_t v; std::memcpy(&v, p + f.off, 2); return v; }
    int32_t v; std::memcpy(&v, p + f.off, 4); return v;
}
static bool is_link(int off) { return off < 4 || (off >= 0x14 && off < 0x18); }

struct SlotDiv {
    int tick = 0;              // first tick at which the slot differs (0 = never)
    int ref_cls = 0, ref_type = 0, ref_state = 0, port_cls = 0, port_type = 0, port_state = 0;
    const Field *field = nullptr;
    long ref_val = 0, port_val = 0;
    int nbytes = 0;
    int link_tick = 0;         // first tick at which only the list links differ
};

static constexpr size_t kState1995Size = 0x38d09;   // GameState dump of the 1995 CARPET.EXE / HIDDEN.EXE
static bool g_ref_1995 = false;                      // a dump of that size was loaded
// The 1995 executables leave bytes of an older string behind the NUL of PlayerRec.name ("Zanzamar\0rah"):
// per instance, outside the checksum; ignored for their references.
static bool ignore_player_byte(const uint8_t *ref_rec, int k) {
    const int name = (int)offsetof(PlayerRec, name);
    if (!g_ref_1995 || k < name || k >= name + (int)sizeof(PlayerRec::name)) return false;
    return std::memchr(ref_rec + name, 0, (size_t)(k - name)) != nullptr;
}

// Round 11: both sides are compared as the original's byte image (mc_image.h): the reference as dumped
// (relinked to index form), the port's state converted with state_to_image.
static GameStateImage s_port_img;
static int g_ref_world = 0;                 // MC_REF_WORLD (below); 0 = the original's world (the gate)
static int edge_cells(int32_t x, int32_t y);
static const GameStateImage &port_image() { state_to_image(*g_state, &s_port_img); return s_port_img; }

static bool load_ref(const std::string &dir, int tick, GameStateImage *out) {
    char name[64];
    std::snprintf(name, sizeof name, "tick%05d.gam", tick);
    std::string path = dir + "/" + name;
    mc_blob b;
    if (!mc_read_file(path.c_str(), &b)) return false;
    // Hidden Worlds (tools/reference/run_level_hw.py): the 1995 executables dump 6 more bytes after
    // spells_present (0x38d09); the common 0x38d03 bytes are compared.
    bool ok = b.len == MC_STATE_IMAGE_SIZE || b.len == kState1995Size;
    if (ok) {
        std::memcpy(out, b.data, MC_STATE_IMAGE_SIZE);
        if (b.len == kState1995Size) g_ref_1995 = true;
        ok = thing_relink_snapshot(out);
    }
    mc_blob_free(&b);
    return ok;
}

// Minimal index.json reader: the integer value of "key" (or def).
static int index_int(const std::string &dir, const char *key, int def) {
    mc_blob b;
    std::string path = dir + "/index.json";
    if (!mc_read_file(path.c_str(), &b)) return def;
    std::string s(reinterpret_cast<const char *>(b.data), b.len);
    mc_blob_free(&b);
    std::string k = std::string("\"") + key + "\":";
    size_t at = s.find(k);
    if (at == std::string::npos) return def;
    at += k.size();
    while (at < s.size() && s[at] == ' ') at++;
    if (at >= s.size() || !(s[at] == '-' || (s[at] >= '0' && s[at] <= '9'))) return def;
    return std::atoi(s.c_str() + at);
}

// Brief comparison of a reference state against the port's (list links ignored): differing slots, the
// first one, player records, RNG. Used by the level-generation check; MC_REF_GEN_TRACE lists every field.
static int compare_states_brief(const GameStateImage &ref, const char *what) {
    int slots = 0, first_slot = 0;
    const Field *first_field = nullptr;
    long rv = 0, pv = 0;
    int nref = 0, nport = 0;
    const bool trace = std::getenv("MC_REF_GEN_TRACE") != nullptr;
    const GameStateImage &port = port_image();
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const uint8_t *pr = reinterpret_cast<const uint8_t *>(&ref.things[i]);
        const uint8_t *po = reinterpret_cast<const uint8_t *>(&port.things[i]);
        if (ref.things[i].cls) nref++;
        if (port.things[i].cls) nport++;
        int first = -1;
        for (int b = 0; b < (int)sizeof(ThingImage); b++) if (pr[b] != po[b] && !is_link(b)) { first = b; break; }
        if (first < 0) continue;
        if (!slots++) {
            first_slot = i; first_field = field_at(first);
            rv = first_field ? field_value(pr, *first_field) : pr[first];
            pv = first_field ? field_value(po, *first_field) : po[first];
        }
        if (trace) {
            std::printf("  gen slot %d (ref %d/%d/%d port %d/%d/%d):", i, pr[0x40], pr[0x41], pr[0x46], po[0x40], po[0x41],
                        po[0x46]);
            if (g_ref_world) {
                const Thing *t = thing_at((unsigned)i);
                std::printf(" [cell %d,%d edge %d]", (int32_t)t->x >> 8, (int32_t)t->y >> 8, edge_cells(t->x, t->y));
                for (unsigned k : {(unsigned)ref.things[i].target, (unsigned)t->target})
                    if (k && k < (unsigned)thing_pool_slots()) {
                        const Thing *o = thing_at(k);
                        std::printf(" [target %u cell %d,%d edge %d]", k, (int32_t)o->x >> 8, (int32_t)o->y >> 8,
                                    edge_cells(o->x, o->y));
                    }
            }
            for (const Field &f : kFields)
                if (!is_link(f.off) && std::memcmp(pr + f.off, po + f.off, f.size))
                    std::printf(" %s %ld/%ld", f.name, field_value(pr, f), field_value(po, f));
            std::printf("\n");
        }
    }
    char pl[9] = {};
    for (int p = 0; p < 8; p++) {
        const uint8_t *a = reinterpret_cast<const uint8_t *>(&ref.players[p]);
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&g_state->players[p]);
        pl[p] = '=';
        for (int k = 0; k < (int)sizeof(PlayerRec); k++) {
            if (a[k] == b[k] || ignore_player_byte(a, k)) continue;
            if (trace) std::printf("  gen player %d +%#x (P+%#x) %02x/%02x\n", p, k, k - 0x44f, a[k], b[k]);
            pl[p] = 'x';
        }
    }
    std::printf("%s: %d slots differ (things port %d / ref %d), rng %s (%08x/%08x), players %s", what, slots, nport,
                nref, ref.rng == g_state->rng ? "same" : "DIFFERS", (unsigned)ref.rng, (unsigned)g_state->rng, pl);
    if (slots)
        std::printf("; first slot %d (ref %s %d/%d) %s %ld -> %ld", first_slot, cls_name(ref.things[first_slot].cls),
                    ref.things[first_slot].type, ref.things[first_slot].state, first_field ? first_field->name : "?", rv, pv);
    std::printf("\n");
    return slots + (ref.rng != g_state->rng) + (std::strchr(pl, 'x') ? 1 : 0);
}

// The Config / GameState part of level_load_file_3d160 that sim_load_level does not do (0x3d160:
// mem_set of GameState+0x244, Config+0x17, +0x5d..+0x6c, +0x96, +0x98, +0xb8..+0xc5, +0x8e1a, the
// per-class list heads +0x8e1e..+0x8e7d, flags &= 0xfffe3fff). Same as game.cpp level_reset_config
// (task D); needed here because the level-generation check loads a second level in one process and
// player_spawn walks Config.player_list (stale from the playback otherwise). Requested for sim_load_level.
static void level_reset_config_local() {
    uint8_t *st = reinterpret_cast<uint8_t *>(g_state);
    uint8_t *cf = reinterpret_cast<uint8_t *>(g_cfg);
    st[0x244] = 0;
    g_cfg->fade_stage = 0;
    std::memset(cf + 0x5d, 0, 0x10);
    g_cfg->substeps = 0;
    g_cfg->palette_effect = 0;
    std::memset(cf + 0xb8, 0, 0xe);
    std::memset(cf + 0x8e1a, 0, 4);
    std::memset(g_cfg->creature_lists, 0, sizeof g_cfg->creature_lists);
    g_cfg->player_list = 0;
    g_cfg->mana_ball_list = 0;
    g_cfg->wizard_list = 0;
    g_cfg->projectile_list = 0;
    g_cfg->flags &= 0x3fff;
    g_cfg->paused &= ~1;
}

// ---- MC_REF_WORLD (round 11 session 3) ---------------------------------------------------------------------
static bool g_ref_world_slot_order = false; // MC_REF_WORLD_RELINK=slot: keep world_widen_tiled's slot-order lists
static int g_ref_widened_tick = -1;         // PlayerRec.tick when the world was widened
// MC_REF_WORLD_GATE=K (cells, with MC_REF_WORLD): the wide-world gate (ctest reference_world). Instead of "no
// divergence" the run passes when the RNG never differs, every slot's first divergence is within K cells of the
// old 255 -> 256 boundary (the Thing, its target or the reference's target) and every differing map cell is too.
static int g_ref_world_gate = -1;
static int g_ref_map_far = 0;               // map cells differing farther than the gate from the boundary

static int ref_world_from_env() {
    const char *e = std::getenv("MC_REF_WORLD");
    if (!e || !*e) return 0;
    const int n = std::atoi(e);
    if (n <= 256 || n > MC_WORLD_MAX || (n & (n - 1))) {
        std::printf("MC_REF_WORLD=%s ignored (512 .. %d, a power of two)\n", e, MC_WORLD_MAX);
        return 0;
    }
    return n;
}

// The port's map index of the original's cell i (tile (0,0) of a tiled world; i itself at 256).
static uint32_t port_cell(int i) { return world_cell(i & 0xff, i >> 8); }

// The original's cell-list head of cell i folded over the tiles: the head of the one tile whose list is not empty
// (0 if none); -1 when Things of that 256-cell are in more than one tile (only possible after a boundary crossing).
static int folded_head(int i) {
    if (!world_wide()) return g_cell_things[i];
    const int tiles = g_world.size >> 8;
    int head = 0, lists = 0;
    for (int ty = 0; ty < tiles; ty++)
        for (int tx = 0; tx < tiles; tx++) {
            const uint16_t h = g_cell_things[world_cell((i & 0xff) + tx * 256, (i >> 8) + ty * 256)];
            if (h) { head = h; lists++; }
        }
    return lists > 1 ? -1 : head;
}

// Widen the loaded 256 world (g_hook_demo_textures_reload in this program: called by the playback right after
// the snapshot pair is loaded, before the first tick's Thing updates). world_widen_tiled relinks every cell list
// in slot order; the original's order (heads + links of the snapshot) is put back, so a cell's Things are
// visited as in the original.
static void ref_widen() {
    if (!g_ref_world || world_wide()) return;
    const int slots = thing_pool_slots();
    std::vector<uint16_t> heads(g_cell_things, g_cell_things + MC_MAP_CELLS);
    std::vector<uint16_t> nxt((size_t)slots), prv((size_t)slots);
    std::vector<uint8_t> linked((size_t)slots);
    for (int i = 0; i < slots; i++) {
        const Thing *t = thing_at((unsigned)i);
        nxt[(size_t)i] = t->cell_next; prv[(size_t)i] = t->cell_prev; linked[(size_t)i] = (t->flags & 4) ? 1 : 0;
    }
    if (!world_widen_tiled(g_ref_world)) { std::printf("MC_REF_WORLD: world_widen_tiled(%d) failed\n", g_ref_world); std::exit(2); }
    int moved = 0;
    if (!g_ref_world_slot_order) {
        for (int i = 0; i < MC_MAP_CELLS; i++) g_cell_things[port_cell(i)] = heads[(size_t)i];
        for (int i = 0; i < slots; i++) {
            Thing *t = thing_at((unsigned)i);
            if (t->cell_next != nxt[(size_t)i] || t->cell_prev != prv[(size_t)i]) moved++;
            t->cell_next = nxt[(size_t)i]; t->cell_prev = prv[(size_t)i];
            if (linked[(size_t)i]) t->flags |= 4u; else t->flags &= ~4u;
        }
    }
    g_ref_widened_tick = g_state ? (int)g_state->players[g_state->local_player & 7].tick : -1;
    std::printf("MC_REF_WORLD: widened to %d cells (tiled) at tick %d; cell lists %s (%d slots relinked back)\n",
                g_world.size, g_ref_widened_tick, g_ref_world_slot_order ? "in slot order" : "in the original's order",
                moved);
}

// Cells (outside tile (0,0)) whose terrain differs from the same cell of tile (0,0): terrain changed by a Thing that
// had left tile (0,0).
static int maps_off_tile0() {
    if (!world_wide()) return 0;
    int n = 0;
    for (uint32_t c = 0; c < g_world.cells; c++) {
        const int cx = world_cell_x(c), cy = world_cell_y(c);
        if (cx < 256 && cy < 256) continue;
        const uint32_t s = world_cell(cx & 0xff, cy & 0xff);
        if (g_map_type[c] != g_map_type[s] || g_map_height[c] != g_map_height[s] || g_map_light[c] != g_map_light[s] ||
            g_map_flags[c] != g_map_flags[s]) n++;
    }
    return n;
}

// Distance in cells of a coordinate pair to the nearest line x = 0 / y = 0 of the 256 torus (the old boundary).
static int edge_cells(int32_t x, int32_t y) {
    const int cx = (x >> 8) & 0xff, cy = (y >> 8) & 0xff;
    const int ex = cx < 255 - cx ? cx : 255 - cx, ey = cy < 255 - cy ? cy : 255 - cy;
    return ex < ey ? ex : ey;
}

// MC_REF_WORLD_GATE: a Thing at the old boundary (within the gate's cells of it, by its low halves) or outside tile
// (0,0) (all its neighbours of the 256 world are in another tile); a pair whose 256-torus difference is not the
// tiled world's difference (their shortest way in the original crosses the old boundary).
static bool boundary_thing(const Thing *t) {
    return edge_cells(t->x, t->y) <= g_ref_world_gate || (int32_t)t->x >= 0x10000 || (int32_t)t->y >= 0x10000;
}
static bool boundary_pair(const Thing *a, const Thing *b) {
    const int32_t dx = (int32_t)b->x - (int32_t)a->x, dy = (int32_t)b->y - (int32_t)a->y;
    return (int16_t)(uint16_t)dx != world_signed(dx) || (int16_t)(uint16_t)dy != world_signed(dy);
}

static int compare_maps(const std::string &dir, int tick, int *cell_diff) {
    char name[64];
    std::snprintf(name, sizeof name, "tick%05d.map", tick);
    std::string path = dir + "/" + name;
    mc_blob b;
    if (!mc_read_file(path.c_str(), &b)) return -1;
    int diff = 0, far_edge = 0;
    *cell_diff = 0;
    if (b.len >= 0x60000) {
        const uint8_t *m = b.data;
        for (int i = 0; i < MC_MAP_CELLS; i++) {
            const uint32_t p = port_cell(i);
            const int diff_before = diff;
            bool d = false;
            if (m[i] != g_map_type[p] || m[0x10000 + i] != g_map_height[p] || m[0x20000 + i] != g_map_light[p] ||
                m[0x30000 + i] != g_map_flags[p]) { diff++; d = true; }
            uint16_t c; std::memcpy(&c, m + 0x40000 + i * 2, 2);
            if ((int)c != folded_head(i)) { (*cell_diff)++; d = true; }
            // the terrain only: a cell-list head follows the Things, whose divergences the slots report
            if (d && g_ref_world && diff_before != diff) {
                const int e = edge_cells((i & 0xff) << 8, (i >> 8) << 8);
                if (e > far_edge) far_edge = e;
                if (g_ref_world_gate >= 0 && e > g_ref_world_gate) {
                    g_ref_map_far++;
                    std::printf("      MC_REF_WORLD: cell %d,%d (edge %d): type %d/%d height %d/%d light %d/%d flags %d/%d\n",
                                i & 0xff, i >> 8, e, m[i], g_map_type[p], m[0x10000 + i], g_map_height[p], m[0x20000 + i],
                                g_map_light[p], m[0x30000 + i], g_map_flags[p]);
                }
            }
        }
    }
    mc_blob_free(&b);
    if (g_ref_world && (diff || *cell_diff))
        std::printf("      MC_REF_WORLD: the differing terrain cells reach %d cells from the old boundary\n", far_edge);
    return diff;
}

// Round 7 (task C): MC_REF_THING_SLOTS=n runs the comparison with an extended Thing pool (forced over the
// movie's own size after demo_open). The references never fill the original's 1000 slots except where they
// say so (level 49), so with the free stack's bottom in the extension the states must stay byte identical.
static void ref_force_pool() {
    if (const char *e = std::getenv("MC_REF_THING_SLOTS")) thing_pool_force_slots(std::atoi(e));
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    std::string ref_dir = argc > 2 ? argv[2] : MC_REFERENCE_DIR;
    int last_tick = argc > 3 ? std::atoi(argv[3]) : 1 << 30;
    const char *csv = argc > 4 ? argv[4] : nullptr;
    // Suite mode: a directory of run_level.py references (extracted/reference, extracted/reference/gen):
    // runs this program once per levelNN/ that has an index.json and prints its summary lines.
    if (argc > 2 && index_int(ref_dir, "level", -1) < 0) {
        int found = 0, failed = 0;
        // levelNN/ of run_level.py and (round 6) the scripted-player recordings of run_player.py: every
        // subdirectory whose index.json names a level, in name order.
        std::vector<std::string> subs, subs1995;
        std::error_code ec;
        for (const auto &e : std::filesystem::directory_iterator(ref_dir, ec)) {
            if (!e.is_directory()) continue;
            std::string sub = "/" + e.path().filename().string();
            if (index_int(ref_dir + sub, "level", -1) < 0) continue;
            // References of the 1995 executables (run_level_hw.py: index.json state_size 0x38d09) run only
            // in a directory that holds nothing else (extracted/reference/hw_gen, cd95_gen): next to the
            // 1996 references (extracted/reference) they are not part of that gate.
            if (index_int(ref_dir + sub, "state_size", (int)MC_STATE_IMAGE_SIZE) == (int)kState1995Size)
                subs1995.push_back(sub);
            else
                subs.push_back(sub);
        }
        if (subs.empty()) subs.swap(subs1995);
        else if (!subs1995.empty())
            std::printf("note: %zu reference(s) of the 1995 executables skipped (run them as their own suite or "
                        "one by one)\n", subs1995.size());
        std::sort(subs.begin(), subs.end());
        for (const std::string &sub : subs) {
            found++;
            std::string cmd = std::string("\"\"") + argv[0] + "\" \"" + game_dir + "\" \"" + ref_dir + sub + "\" " +
                              std::to_string(last_tick) + "\"";
#ifdef _WIN32
            FILE *pp = _popen(cmd.c_str(), "r");     // cmd.exe /c strips the outer pair of quotes
#else
            FILE *pp = popen(cmd.substr(1, cmd.size() - 2).c_str(), "r");
#endif
            if (!pp) { failed++; continue; }
            char line[1024];
            while (std::fgets(line, sizeof line, pp))
                if (!std::strncmp(line, "LEVEL", 5) || std::strstr(line, "level generation") || !std::strncmp(line, "SKIP", 4))
                    std::fputs(line, stdout);
#ifdef _WIN32
            int rc = _pclose(pp);
#else
            int rc = pclose(pp);
#endif
            if (rc != 0) failed++;
        }
        if (found) {
            std::printf("%s: %d of %d level references diverge\n", failed ? "FAIL" : "OK", failed, found);
            return failed ? 1 : 0;
        }
    }
    if (!sim_init(game_dir)) { std::printf("sim_init failed\n"); return 2; }
    sim_register_gameplay();
    g_ref_world = ref_world_from_env();
    if (g_ref_world) {
        const char *rl = std::getenv("MC_REF_WORLD_RELINK");
        g_ref_world_slot_order = rl && !std::strcmp(rl, "slot");
        // reference_test links no renderer: the texture reload hook of the playback start is free
        if (g_hook_demo_textures_reload) { std::printf("MC_REF_WORLD: the snapshot hook is taken\n"); return 2; }
        g_hook_demo_textures_reload = ref_widen;
        if (const char *gt = std::getenv("MC_REF_WORLD_GATE")) g_ref_world_gate = std::atoi(gt);
        std::printf("MC_REF_WORLD=%d: the world is tiled to %d x %d cells after the snapshot load\n", g_ref_world,
                    g_ref_world, g_ref_world);
    }

    // Level mode: index.json of a run_level.py reference names the level and the recording.
    const int level = index_int(ref_dir, "level", -1);
    const int movie = level >= 0 ? index_int(ref_dir, "movie", 20000 + level) : 0;
    // Hidden Worlds references (run_level_hw.py --exe hidden): "level" is the DDLEVELS entry k given to
    // HIDDEN.EXE's -level, "port_level" the port's campaign index (100 + k, world_set.h) that loads it.
    const int load_level = level >= 0 ? index_int(ref_dir, "port_level", level) : level;
    const int first_tick = level >= 0 ? index_int(ref_dir, "first_tick", 2) : 413;
    // gen_only (run_level.py --gen-only): only the level-generation check (tick00001.gam + the snapshot pair).
    const bool gen_only = level >= 0 && index_int(ref_dir, "gen_only", 0) != 0;
    static GameStateImage ref;   // 0x38d03 bytes
    if (!load_ref(ref_dir, gen_only ? 1 : first_tick, &ref)) {
        // Not every machine has the dumps (they come from DOSBox); ctest passes with a notice.
        std::printf("SKIP: reference %s/tick%05d.gam missing or unusable; run tools/reference/run_reference.py "
                    "(movie 0) or run_level.py (levels)\n", ref_dir.c_str(), first_tick);
        return 0;
    }
    if (level >= 0)
        std::printf("level mode: level %d%s, recording movie/mvi%05d.dat, first dump tick %d\n", level,
                    load_level != level ? (" (port level " + std::to_string(load_level) + ")").c_str() : "", movie,
                    first_tick);

    // A projectile that hit nothing stores (NULL - &things[0]) / 0xa4 (signed, low 16 bits) in its
    // impact effect's target: depends on where the reference run's pool lay (first seen at tick 1196).
    g_projectile_null_hit_index = (uint16_t)((int32_t)(0u - g_snapshot_things_base) / (int32_t)sizeof(ThingImage));
    std::printf("reference run: things[0] at 0x%08x, null-hit index 0x%04x\n", g_snapshot_things_base,
                (unsigned)g_projectile_null_hit_index);
    // The reference runs start with `-roll` (patched: flags |= 0x124 to play, 0x100 to record): 0x100 (front end
    // skipped, "custom") also switches game_check_level_won_3db20 off (`test flags, 0x110`), so mirror it.
    // Round 6: a recording may quick-save (command 10, movie/gam10000.dat) and quick-load (0xb); the port
    // writes the quick save into a scratch directory next to the test (demo_set_record_dir), the original
    // into its movie directory.
    const std::string qs_dir = (std::filesystem::temp_directory_path() / "mc_reference_test_quicksave").string();
    std::error_code qs_ec;
    std::filesystem::create_directories(std::filesystem::path(qs_dir) / "movie", qs_ec);
    std::filesystem::remove(std::filesystem::path(qs_dir) / "movie" / "gam10000.dat", qs_ec);
    demo_set_record_dir(qs_dir.c_str());
    // A 1995 recording's snapshot gam<movie>.dat is 0x38d09 bytes, which the port's demo_load_state does not
    // take: its 0x38d03-byte head (the 6 extra bytes were zero in every 1995 dump so far) goes into a scratch
    // movie directory together with the recording and the terrain snapshot.
    std::string movie_dir = ref_dir;
    if (level >= 0 && !gen_only) {
        char gname[64];
        std::snprintf(gname, sizeof gname, "/movie/gam%05d.dat", movie);
        mc_blob g{};
        if (mc_read_file((ref_dir + gname).c_str(), &g) && g.len == kState1995Size) {
            namespace fs = std::filesystem;
            const fs::path tmp = fs::temp_directory_path() / "mc_reference_test_1995";
            std::error_code ec;
            fs::create_directories(tmp / "movie", ec);
            for (const char *k : {"mvi", "map"}) {
                char n[64];
                std::snprintf(n, sizeof n, "%s%05d.dat", k, movie);
                fs::copy_file(fs::path(ref_dir) / "movie" / n, tmp / "movie" / n, fs::copy_options::overwrite_existing, ec);
            }
            if (FILE *fp = std::fopen((tmp.string() + gname).c_str(), "wb")) {
                std::fwrite(g.data, 1, MC_STATE_IMAGE_SIZE, fp);
                std::fclose(fp);
            }
            int extra = 0;
            for (size_t i = MC_STATE_IMAGE_SIZE; i < g.len; i++) extra |= g.data[i];
            std::printf("1995 snapshot: playing from %s (6 extra bytes %s)\n", tmp.string().c_str(),
                        extra ? "NON-ZERO" : "zero");
            movie_dir = tmp.string();
        }
        mc_blob_free(&g);
    }
    auto start_playback = [&]() -> int {
        g_cfg->flags = 0x100; g_cfg->paused = 0;
        sim_prepare_movie();
        // The original's play run: `-level L` generates the level, then the first tick loads the snapshot.
        level_reset_config_local();
        ref_force_pool();
        if (!sim_load_level(load_level)) { std::printf("sim_load_level(%d) failed\n", load_level); return 2; }
        if (!gen_only && !demo_open(movie_dir.c_str(), movie)) {
            std::printf("%s/movie/mvi%05d.dat missing\n", ref_dir.c_str(), movie);
            return 2;
        }
        ref_force_pool();
        return 0;
    };
    // A quick load sets the level clock (PlayerRec.tick) back, and the original's dumps are named by it: a
    // tick that is played twice holds the state of its last pass. A first playback finds, per step, whether
    // its tick comes again later; only the last pass of a tick is compared.
    std::vector<char> step_is_last;
    int step_limit = 1 << 30;
    bool has_quickload = false;
    if (level >= 0 && !gen_only) {
        char rel[64];
        std::snprintf(rel, sizeof rel, "/movie/mvi%05d.dat", movie);
        mc_blob mv;
        if (mc_read_file((ref_dir + rel).c_str(), &mv)) {
            for (size_t i = 0; i + 10 <= mv.len; i += 10) if (mv.data[i] == 0xb) has_quickload = true;
            mc_blob_free(&mv);
        }
    }
    if (has_quickload) {
        g_cfg->flags = 0x100; g_cfg->paused = 0;
        if (int rc = start_playback()) return rc;
        std::vector<int> seq;
        bool go = true;
        for (int steps = 0; go && steps < 1000000; steps++) {
            if (steps > 0) hud_tick_state(g_state->local_player);
            go = demo_step();
            seq.push_back((int)g_state->players[g_state->local_player & 7].tick);
        }
        demo_close();
        // run_player.py found where the original's playback really ended (the last dump it wrote, after
        // `loads` quick loads): the original stops a few ticks after a quick load. Steps past that point
        // have no dump of their pass and are not compared.
        const int end_tick = index_int(ref_dir, "playback_end_tick", 0);
        const int end_loads = index_int(ref_dir, "playback_end_loads", -1);
        if (end_tick > 0 && end_loads >= 0) {
            int loads = 0;
            for (size_t i = 0; i < seq.size(); i++) {
                if (i > 0 && seq[i] < seq[i - 1]) loads++;
                if (loads == end_loads && seq[i] == end_tick) {
                    seq.resize(i + 1);
                    std::printf("the original's playback ended at tick %d after %d quick load(s): step %zu\n", end_tick,
                                end_loads, i);
                    break;
                }
            }
        }
        step_limit = (int)seq.size();
        step_is_last.assign(seq.size(), 1);
        std::vector<char> seen(1 << 20, 0);
        for (size_t i = seq.size(); i-- > 0;) {
            int t = seq[i] & ((1 << 20) - 1);
            if (seen[(size_t)t]) step_is_last[i] = 0;
            seen[(size_t)t] = 1;
        }
        std::printf("quick load in the recording: %zu steps, %zu of them play a tick that is played again later "
                    "(not compared)\n", seq.size(), (size_t)std::count(step_is_last.begin(), step_is_last.end(), 0));
        std::filesystem::remove(std::filesystem::path(qs_dir) / "movie" / "gam10000.dat", qs_ec);
    }
    g_cfg->flags = 0x100; g_cfg->paused = 0;
    sim_prepare_movie();
    if (level >= 0) {
        if (int rc = start_playback()) return rc;
    } else {
        // As `carpet -roll 1 -level 38`: the original generates level 38 before the first tick loads the
        // snapshot, which leaves the terrain RNG g_rng16 (DAT_0012dfb0, in neither GameState nor the map
        // dump) at 0x2fea; the run-time retexturing draws from it (task F, round 5: map-flag rotation bits
        // differ from tick 800 without it - the state comparison does not see the flags, the render does).
        level_reset_config_local();
        ref_force_pool();
        sim_load_level(38);
        if (!demo_open(game_dir, 0)) { std::printf("movie 0 missing\n"); return 2; }
        ref_force_pool();
    }
    thing_dispatch_reset_stats();

    std::vector<SlotDiv> div(MC_THING_SLOTS);
    int first_rng_tick = 0, first_player_tick[8] = {}, first_player_off[8] = {};
    int compared = 0, first_count_tick = 0, first_map_tick = 0;
    int class_first[14] = {};
    std::printf("tick  same/live  diff(links only)  by class [diff/live]                     rng  players\n");
    bool more = !gen_only;
    int pool_full_tick = 0;     // round 7: the first tick in which the port's pool had no free slot
    int wide_cross_tick = 0, wide_cross_slot = 0;   // MC_REF_WORLD: the first Thing outside tile (0,0)
    bool wide_diag_done = false;                    // MC_REF_WORLD: the first divergent tick described
    std::vector<int> wide_div_slots;
    int wide_unexplained = 0;                       // MC_REF_WORLD_GATE: divergences away from the old boundary
    for (int steps = 0; more && steps < 1000000; steps++) {
        // render_frame_1fab0's state writes of the previous tick (hud_tick_state). The original renders
        // after sound_update, where the reference exe dumps, so dump N holds them up to tick N - 1.
        if (steps > 0) hud_tick_state(g_state->local_player);
        const uint32_t pool_fail0 = g_thing_alloc_failures;
        more = demo_step();
        int tick = (int)g_state->players[g_state->local_player & 7].tick;
        if (!pool_full_tick && (g_thing_alloc_failures != pool_fail0 || thing_free_count() == 0)) pool_full_tick = tick;
        if (g_ref_world && world_wide() && !wide_cross_tick)
            for (int i = 1; i < thing_pool_slots(); i++) {
                const Thing *t = thing_at((unsigned)i);
                if (t->cls && ((int32_t)t->x >= 0x10000 || (int32_t)t->y >= 0x10000)) {
                    wide_cross_tick = tick; wide_cross_slot = i;
                    std::printf("MC_REF_WORLD: tick %d: slot %d (%s %d/%d) is the first Thing outside tile (0,0), at "
                                "cell %d,%d\n", tick, i, cls_name(t->cls), t->type, t->state, (int32_t)t->x >> 8,
                                (int32_t)t->y >> 8);
                    break;
                }
            }
        if (std::getenv("MC_REF_STEPS")) std::printf("  step %d tick %d\n", steps, tick);
        // MC_REF_CELLCHECK=1: walk every cell list (g_cell_things heads, Thing.cell_next) and report a list
        // that does not end within the pool size (a cycle) - used to explain the original's hang after a
        // quick load (the cell heads are not part of the quick save).
        if (std::getenv("MC_REF_CELLCHECK")) {
            int cyc = 0, first_cell = -1;
            for (uint32_t c = 0; c < g_world.cells; c++) {
                unsigned i = g_cell_things[c];
                int n = 0;
                while (i != 0 && i < MC_THING_SLOTS && n <= MC_THING_SLOTS) { i = g_state->things[i].cell_next; n++; }
                if (n > MC_THING_SLOTS) { if (!cyc++) first_cell = (int)c; }
            }
            if (cyc) std::printf("  step %d tick %d: %d cell lists do not terminate (first cell %d,%d)\n", steps, tick, cyc,
                                 world_cell_x((uint32_t)first_cell), world_cell_y((uint32_t)first_cell));
        }
        if (tick > last_tick && !has_quickload) break;
        if (steps >= step_limit) break;
        if ((size_t)steps < step_is_last.size() && !step_is_last[(size_t)steps]) continue;
        if (!load_ref(ref_dir, tick, &ref)) continue;
        compared++;
        const GameStateImage &port = port_image();
        int live = 0, same = 0, links_only = 0;
        int cdiff[14] = {}, clive[14] = {};
        int nref = 0, nport = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            const ThingImage &r = ref.things[i], &o = port.things[i];
            if (r.cls) nref++;
            if (o.cls) nport++;
            if (r.cls == 0 && o.cls == 0) continue;
            live++;
            int kc = r.cls ? r.cls : o.cls;
            if (kc < 14) clive[kc]++;
            const uint8_t *pr = reinterpret_cast<const uint8_t *>(&r), *po = reinterpret_cast<const uint8_t *>(&o);
            int first = -1, nb = 0;
            bool link = false;
            for (int b = 0; b < (int)sizeof(ThingImage); b++) {
                if (pr[b] == po[b]) continue;
                if (is_link(b)) { link = true; continue; }
                if (first < 0) first = b;
                nb++;
            }
            if (first < 0) {
                same++;
                if (link) { links_only++; if (!div[i].link_tick) div[i].link_tick = tick; }
                continue;
            }
            if (kc < 14) { cdiff[kc]++; if (!class_first[kc]) class_first[kc] = tick; }
            if (g_ref_world && !div[i].tick) wide_div_slots.push_back(i);
            SlotDiv &d = div[i];
            if (!d.tick) {
                d.tick = tick;
                d.ref_cls = r.cls; d.ref_type = r.type; d.ref_state = r.state;
                d.port_cls = o.cls; d.port_type = o.type; d.port_state = o.state;
                d.field = field_at(first);
                d.ref_val = d.field ? field_value(pr, *d.field) : pr[first];
                d.port_val = d.field ? field_value(po, *d.field) : po[first];
                d.nbytes = nb;
            }
        }
        // MC_REF_TRACE="slot,slot,..." (MC_REF_TRACE_FROM=tick): every differing field of these slots per
        // tick, and every differing byte of the owning player record (P block offsets) for class-3 slots.
        if (const char *tr = std::getenv("MC_REF_TRACE")) {
            const char *from = std::getenv("MC_REF_TRACE_FROM");
            if (tick >= (from ? std::atoi(from) : 0)) {
                for (const char *p = tr; *p; ) {
                    int i = std::atoi(p);
                    if (i > 0 && i < MC_THING_SLOTS) {
                        const uint8_t *pr = reinterpret_cast<const uint8_t *>(&ref.things[i]);
                        const uint8_t *po = reinterpret_cast<const uint8_t *>(&port.things[i]);
                        std::printf("  trace %d slot %d (ref %d/%d/%d port %d/%d/%d):", tick, i, pr[0x40], pr[0x41], pr[0x46],
                                    po[0x40], po[0x41], po[0x46]);
                        for (const Field &f : kFields) {
                            if (is_link(f.off) || std::memcmp(pr + f.off, po + f.off, f.size) == 0) continue;
                            std::printf(" %s %ld/%ld", f.name, field_value(pr, f), field_value(po, f));
                        }
                        std::printf("\n");
                    }
                    while (*p && *p != ',') p++;
                    if (*p) p++;
                }
            }
        }
        // MC_REF_PTRACE=player (MC_REF_TRACE_FROM=tick): the differing bytes of that PlayerRec ("P" = P block offset).
        if (const char *pt = std::getenv("MC_REF_PTRACE")) {
            const char *from = std::getenv("MC_REF_TRACE_FROM");
            const int p = std::atoi(pt) & 7;
            if (tick >= (from ? std::atoi(from) : 0)) {
                const uint8_t *a = reinterpret_cast<const uint8_t *>(&ref.players[p]);
                const uint8_t *b = reinterpret_cast<const uint8_t *>(&g_state->players[p]);
                int shown = 0;
                for (int k = 0; k < (int)sizeof(PlayerRec) && shown < 16; k++) {
                    if (a[k] == b[k]) continue;
                    if (!shown++) std::printf("  ptrace %d player %d:", tick, p);
                    if (k >= 0x44f) std::printf(" P+%03x %02x/%02x", k - 0x44f, a[k], b[k]);
                    else std::printf(" +%03x %02x/%02x", k, a[k], b[k]);
                }
                if (shown) std::printf("\n");
            }
        }
        if (nref != nport && !first_count_tick) first_count_tick = tick;
        bool rng_same = ref.rng == g_state->rng;
        if (!rng_same && !first_rng_tick) first_rng_tick = tick;
        char pl[64] = {}; int pn = 0;
        for (int p = 0; p < 8; p++) {
            const uint8_t *a = reinterpret_cast<const uint8_t *>(&ref.players[p]);
            const uint8_t *b = reinterpret_cast<const uint8_t *>(&g_state->players[p]);
            int off = -1;
            for (int k = 0; k < (int)sizeof(PlayerRec); k++) if (a[k] != b[k] && !ignore_player_byte(a, k)) { off = k; break; }
            pl[pn++] = off < 0 ? '=' : 'x';
            if (off >= 0 && !first_player_tick[p]) { first_player_tick[p] = tick; first_player_off[p] = off; }
        }
        // MC_REF_WORLD: where the Things of the first divergent tick are (full port coordinates, distance to the old
        // 255 -> 256 boundary in cells, their target), the local wizard, and how many Things have left tile (0,0).
        // Then (MC_REF_WORLD_WHERE=n lines, default 40) every slot at its first divergence the same way.
        auto where = [](const Thing *t) {
            std::printf("cell %d.%02x,%d.%02x (edge %d)", (int32_t)t->x >> 8, (unsigned)((int32_t)t->x & 0xff),
                        (int32_t)t->y >> 8, (unsigned)((int32_t)t->y & 0xff), edge_cells(t->x, t->y));
        };
        if (g_ref_world && !wide_diag_done && (!wide_div_slots.empty() || !rng_same || nref != nport || std::strchr(pl, 'x'))) {
            wide_diag_done = true;
            int off_tile = 0;
            for (int i = 1; i < thing_pool_slots(); i++) {
                const Thing *t = thing_at((unsigned)i);
                if (t->cls && ((int32_t)t->x >= 0x10000 || (int32_t)t->y >= 0x10000)) off_tile++;
            }
            std::printf("MC_REF_WORLD: first divergent tick %d: %zu slot(s), rng %s, players %s, things %d/%d; %d Things "
                        "outside tile (0,0); terrain changed outside tile (0,0) in %d cells\n", tick, wide_div_slots.size(),
                        rng_same ? "same" : "DIFFERS", pl, nport, nref, off_tile, maps_off_tile0());
            const Thing *w = thing_at(thing_wrap(g_state->players[g_state->local_player & 7].thing));
            std::printf("  local wizard (slot %u) at ", (unsigned)g_state->players[g_state->local_player & 7].thing);
            where(w);
            std::printf("\n");
        }
        if (g_ref_world) {
            static int where_left = -1;
            if (where_left < 0) where_left = std::getenv("MC_REF_WORLD_WHERE") ? std::atoi(std::getenv("MC_REF_WORLD_WHERE")) : 40;
            for (int i : wide_div_slots) {
                const ThingImage &r = ref.things[i];
                const Thing *t = thing_at((unsigned)i);
                // MC_REF_WORLD_GATE=K: a divergence is a boundary effect when the Thing, its target or the reference's
                // target is within K cells of the old boundary (low halves: a Thing that crossed counts as at it)
                if (g_ref_world_gate >= 0) {
                    // a spell sits at (0,0): its owner (the wizard) stands for it
                    const bool spell = r.cls == 12 || t->cls == 12;
                    const Thing *self = spell ? (t->owner && t->owner < (unsigned)thing_pool_slots() ? thing_at(t->owner) : nullptr) : t;
                    bool at_edge = self && boundary_thing(self);
                    for (unsigned k : {(unsigned)t->owner, (unsigned)t->target, (unsigned)r.target}) {
                        if (!k || k >= (unsigned)thing_pool_slots() || (int)k == i) continue;
                        const Thing *o = thing_at(k);
                        if (o->cls == 12) continue;
                        if (boundary_thing(o) || (self && boundary_pair(self, o))) at_edge = true;
                    }
                    if (!at_edge) {
                        wide_unexplained++;
                        std::printf("  UNEXPLAINED %d slot %d (%s %d/%d) at cell %d,%d (edge %d): not within %d cells of the "
                                    "old boundary\n", tick, i, cls_name(r.cls), r.type, r.state, (int32_t)t->x >> 8,
                                    (int32_t)t->y >> 8, edge_cells(t->x, t->y), g_ref_world_gate);
                    }
                }
                if (where_left <= 0) continue;
                where_left--;
                std::printf("  where %d slot %d ref %s %d/%d port %s %d/%d at ", tick, i, cls_name(r.cls), r.type, r.state,
                            cls_name(t->cls), t->type, t->state);
                where(t);
                if (t->target && t->target < (unsigned)thing_pool_slots()) {
                    std::printf("; target %u at ", (unsigned)t->target);
                    where(thing_at(t->target));
                }
                if (r.target != t->target && r.target && r.target < (unsigned)thing_pool_slots()) {
                    std::printf("; ref target %u at ", (unsigned)r.target);
                    where(thing_at(r.target));
                }
                std::printf("\n");
            }
            wide_div_slots.clear();
        }
        bool print = compared <= 20 || tick % 100 == 0 || !more;
        if (print) {
            std::printf("%5d %4d/%4d %4d(%3d)  ", tick, same, live, live - same, links_only);
            for (int k = 1; k < 14; k++) if (clive[k]) std::printf("%s %d/%d ", cls_name(k), cdiff[k], clive[k]);
            std::printf(" %s%08x %s things %d/%d\n", rng_same ? "rng=" : "rng!", (unsigned)ref.rng, pl, nport, nref);
        }
        if (tick % 100 == 0) {
            int cells = 0;
            int maps = compare_maps(ref_dir, tick, &cells);
            if ((maps > 0 || cells > 0) && !first_map_tick) first_map_tick = tick;
            if (maps >= 0) std::printf("      maps: %d cells differ (type/height/light/flags), %d cell-list heads differ\n", maps, cells);
        }
    }
    demo_close();
    std::printf("\ncompared %d ticks; first tick with a different RNG: %d; first tick with a different live count: %d\n",
                compared, first_rng_tick, first_count_tick);
    for (int p = 0; p < 4; p++)
        std::printf("player %d record: first differs at tick %d, offset %#x\n", p, first_player_tick[p], first_player_off[p]);
    std::printf("first diverging tick per class (reference class):");
    for (int k = 1; k < 14; k++) if (class_first[k]) std::printf(" %s %d", cls_name(k), class_first[k]);
    std::printf("\n\nper slot, sorted by tick (slot ref cls/type/state -> port cls/type/state, field ref -> port, bytes):\n");
    std::vector<int> order;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (div[i].tick) order.push_back(i);
    for (size_t a = 1; a < order.size(); a++)
        for (size_t b = a; b > 0 && div[order[b]].tick < div[order[b - 1]].tick; b--) std::swap(order[b], order[b - 1]);
    int shown = 0;
    for (int i : order) {
        const SlotDiv &d = div[i];
        if (shown++ < 150)
            std::printf("  %5d slot %3d %-10s %2d/%3d -> %-10s %2d/%3d  %-14s %ld -> %ld  (%d bytes)\n", d.tick, i,
                        cls_name(d.ref_cls), d.ref_type, d.ref_state, cls_name(d.port_cls), d.port_type, d.port_state,
                        d.field ? d.field->name : "?", d.ref_val, d.port_val, d.nbytes);
    }
    std::printf("%zu slots diverge in total (%d shown)\n", order.size(), shown < 150 ? shown : 150);
    // Field histogram of the first divergences.
    std::printf("first differing field, counted over slots:");
    for (const Field &f : kFields) {
        int n = 0;
        for (int i : order) if (div[i].field == &f) n++;
        if (n) std::printf(" %s %d", f.name, n);
    }
    std::printf("\n");
    if (csv) {
        FILE *fp = std::fopen(csv, "w");
        if (fp) {
            std::fprintf(fp, "slot,tick,ref_cls,ref_type,ref_state,port_cls,port_type,port_state,field,ref_val,port_val,bytes,link_tick\n");
            for (int i = 1; i < MC_THING_SLOTS; i++) {
                const SlotDiv &d = div[i];
                std::fprintf(fp, "%d,%d,%d,%d,%d,%d,%d,%d,%s,%ld,%ld,%d,%d\n", i, d.tick, d.ref_cls, d.ref_type, d.ref_state,
                             d.port_cls, d.port_type, d.port_state, d.field ? d.field->name : "", d.ref_val, d.port_val,
                             d.nbytes, d.link_tick);
            }
            std::fclose(fp);
            std::printf("per-slot table written to %s\n", csv);
        }
    }
    std::printf("pool: %d slots, first without a free slot in tick %d (0 = never)\n", thing_pool_slots(), pool_full_tick);
    int missing = thing_dispatch_report(nullptr);
    std::printf("handlers dispatched without a port: %d\n", missing);
    if (missing) thing_dispatch_report(stdout);
    int gen_diff = -1;
    if (level >= 0) {
        // Level generation: fresh sim_load_level + the first tick (idle local input) against tick 1 of the
        // original's record run (dumped before the recording started).
        if (load_ref(ref_dir, 1, &ref)) {
            g_cfg->flags = 0x100;                    // the record run: -roll with flags |= 0x100
            sim_prepare_movie();
            level_reset_config_local();
            if (sim_load_level(load_level)) {
                ref_widen();                             // MC_REF_WORLD (nothing otherwise)
                game_tick_sim();
                gen_diff = compare_states_brief(ref, "level generation + tick 1");
                // The recording's map file is the terrain at tick 2 (one tick later; terrain rarely changes
                // that fast): per map, the cells that differ from the port's generated level.
                char rel[64];
                std::snprintf(rel, sizeof rel, "/movie/map%05d.dat", movie);
                mc_blob m{};
                if (const char *dump = std::getenv("MC_REF_GEN_DUMP")) {     // the port's four maps, raw
                    if (FILE *fp = std::fopen(dump, "wb")) {
                        // tile (0,0) of a wide world (port_cell): the same bytes at 256
                        std::vector<uint8_t> raw(0x10000);
                        for (const uint8_t *src : {g_map_type, g_map_height, g_map_light, g_map_flags}) {
                            for (int i = 0; i < 0x10000; i++) raw[(size_t)i] = src[port_cell(i)];
                            std::fwrite(raw.data(), 1, raw.size(), fp);
                        }
                        std::fclose(fp);
                    }
                }
                if (mc_read_file((ref_dir + rel).c_str(), &m) && m.len >= 0x40000) {
                    const uint8_t *maps[4] = { g_map_type, g_map_height, g_map_light, g_map_flags };
                    const char *mname[4] = { "type", "height", "light", "flags" };
                    std::printf("level generation maps vs the tick-2 snapshot:");
                    for (int k = 0; k < 4; k++) {
                        int n = 0, first = -1;
                        for (int i = 0; i < MC_MAP_CELLS; i++)
                            if (m.data[k * 0x10000 + i] != maps[k][port_cell(i)]) { if (first < 0) first = i; n++; }
                        std::printf(" %s %d", mname[k], n);
                        if (n) std::printf(" (first cell %d,%d: %d/%d)", first & 0xff, first >> 8, m.data[k * 0x10000 + first],
                                           maps[k][port_cell(first)]);
                    }
                    std::printf("\n");
                }
                mc_blob_free(&m);
            }
        } else {
            std::printf("level generation: no tick00001.gam\n");
        }
    }
    // Regression gate: since round 4 the port is byte-identical to the original over the whole movie.
    int diverged = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (div[i].tick) diverged++;
    for (int p = 0; p < 8; p++) if (first_player_tick[p]) diverged++;
    if (first_rng_tick || first_count_tick || first_map_tick) diverged++;
    if (first_map_tick) std::printf("maps first differ at tick %d\n", first_map_tick);
    std::printf("%s: %d divergence(s) from the original over %d ticks\n", diverged ? "FAIL" : "OK", diverged, compared);
    if (level >= 0) {
        int first = 0;
        auto take = [&](int t) { if (t && (!first || t < first)) first = t; };
        for (int i = 1; i < MC_THING_SLOTS; i++) take(div[i].tick);
        for (int p = 0; p < 8; p++) take(first_player_tick[p]);
        take(first_rng_tick);
        take(first_count_tick);
        take(first_map_tick);
        std::printf("LEVEL %d: first divergent tick %d (0 = none) over %d ticks; level generation %s", level, first,
                    compared, gen_diff < 0 ? "not checked" : gen_diff ? "differs" : "identical");
        if (g_ref_world)
            std::printf("; MC_REF_WORLD %d: first Thing outside tile (0,0) in tick %d (slot %d)", g_ref_world,
                        wide_cross_tick, wide_cross_slot);
        std::printf("\n");
    } else if (g_ref_world) {
        int first = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++) if (div[i].tick && (!first || div[i].tick < first)) first = div[i].tick;
        for (int p = 0; p < 8; p++) if (first_player_tick[p] && (!first || first_player_tick[p] < first)) first = first_player_tick[p];
        for (int t : {first_rng_tick, first_count_tick, first_map_tick}) if (t && (!first || t < first)) first = t;
        std::printf("MC_REF_WORLD %d: first divergent tick %d (0 = none) over %d ticks; first Thing outside tile (0,0) in "
                    "tick %d (slot %d)\n", g_ref_world, first, compared, wide_cross_tick, wide_cross_slot);
    }
    if (g_ref_world && g_ref_world_gate >= 0) {
        const bool pass = !first_rng_tick && !wide_unexplained && !g_ref_map_far && !missing && gen_diff <= 0;
        std::printf("%s: MC_REF_WORLD gate (boundary %d cells): %d divergence(s), %d slot(s) away from the old boundary, "
                    "%d map cell(s) away from it, RNG %s\n", pass ? "OK" : "FAIL", g_ref_world_gate, diverged,
                    wide_unexplained, g_ref_map_far, first_rng_tick ? "differs" : "identical");
        return pass ? 0 : 1;
    }
    return diverged || missing || gen_diff > 0 ? 1 : 0;
}
