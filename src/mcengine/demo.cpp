// Movie playback (demo_record_playback_step_3c540 and helpers), translated from the disassembly.
#define _CRT_SECURE_NO_WARNINGS
#include "demo.h"
#include "player.h"
#include "terrain.h"
#include "thing.h"
#include "settings.h"
#include "mode.h"
#include "mc_math.h"
#include "mcfile.h"
#include "world.h"
#include "mc_image.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int  (*g_hook_demo_input_changed)() = nullptr;
void (*g_hook_demo_input_snapshot)() = nullptr;
void (*g_hook_demo_textures_reload)() = nullptr;

void (*g_hook_demo_textures_mark)() = nullptr;
bool (*g_hook_demo_quick_save)() = nullptr;
bool (*g_hook_demo_quick_load)() = nullptr;
bool (*g_hook_demo_mode_start)(const char *, uint32_t, const ModeParams &) = nullptr;
void (*g_hook_demo_mode_stop)() = nullptr;
void (*g_hook_demo_sim_globals_get)(uint32_t v[DEMO_SIM_GLOBALS]) = nullptr;
void (*g_hook_demo_sim_globals_set)(const uint32_t v[DEMO_SIM_GLOBALS]) = nullptr;
size_t (*g_hook_demo_order_out)(int, uint8_t *, size_t) = nullptr;
void   (*g_hook_demo_order_in)(int, const uint8_t *, size_t) = nullptr;

static std::string s_dir;          // game directory ("movie" is appended like the "%s/..." formats do)
static std::string s_rec_dir;      // port: where recordings / quick saves are written (never the game copy)
static mc_blob     s_file;         // the open recording (the original keeps a DOS handle in Config+9)
static size_t      s_pos = 0;
static std::FILE  *s_out = nullptr; // the recording being written (port: Config+9 holds 2 while it is open)
static long        s_written = 0;  // packets written to s_out
// Port round 7: recordings with a non-original Thing pool (demo.h DemoExtHeader): mvx / gax / max files.
static bool        s_ext = false;  // the movie being played / recorded is one of those
static bool        s_forced = false;   // demo_open set thing_pool_force_slots (cleared by demo_close)
static size_t      s_data_off = 0; // bytes before the first packet in s_file (the mvx header)
static bool        s_rules_forced = false;  // demo_open set gameplay_force_rules (cleared by demo_close)
static size_t      s_hdr_size = 0; // size of the mvx header of the movie demo_open chose
// Round 10 (task A): movie format v3 (demo.h) - a recording of a game-mode run.
static int         s_version = 0;  // 0 = mvi (the original's), else the mvx version of the movie played / recorded
static bool        s_mode_started = false;  // demo_open started the mode run of a v3 movie (demo_close stops it)
static long        s_rec_read = 0, s_rec_total = 0;     // v3: records read / in the file
static long count_v3_records(const uint8_t *p, size_t len, size_t off) {
    long n = 0;
    while (off + sizeof(CmdPacket) + 2 <= len) {
        const size_t blob = (size_t)p[off + sizeof(CmdPacket)] | ((size_t)p[off + sizeof(CmdPacket) + 1] << 8);
        off += sizeof(CmdPacket) + 2 + blob;
        if (off > len) break;
        n++;
    }
    return n;
}
static const char *fmt_mvi() { return s_ext ? "%s/mvx%05d.dat" : "%s/mvi%05d.dat"; }
static bool pool_extended() { return thing_pool_slots() != MC_THING_SLOTS; }
static bool rules_extended() { return !gameplay_rules_faithful(gameplay_rules()); }

// The pointer bases demo_save_state writes (see demo.h). Defaults: the bundled DOSBox 0.74-3 running
// carpet.exe under DOS/4GW (every reference dump of tools/reference has these).
static DemoPointerBases s_bases = { 0x000190d0u, 0x001fda10u, 0x0021de90u };

// Config+9: the file handle (0 = closed). The port stores 1 while s_file is open.
static uint32_t handle_get() { return g_cfg->demo_file; }
static void     handle_set(uint32_t h) { g_cfg->demo_file = h; }

static void movie_path_in(const std::string &dir, char *buf, size_t cap, const char *fmt, int number) {
    char rel[64];
    std::snprintf(rel, sizeof rel, fmt, "movie", number);       // "%s/gam%05d.dat" with DAT_000908f4 = "movie"
    mc_path_join(buf, cap, dir.c_str(), rel);
}
static bool file_exists(const char *path) {
    std::FILE *f = std::fopen(path, "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}
// Reading: the original has one movie directory for both. The port reads a file from the record
// directory when it is there (the quick save gam10000.dat written by command 10, a movie just
// recorded), otherwise from the playback directory of demo_open.
static void movie_path(char *buf, size_t cap, const char *fmt, int number) {
    if (!s_rec_dir.empty()) {
        movie_path_in(s_rec_dir, buf, cap, fmt, number);
        if (file_exists(buf)) return;
    }
    movie_path_in(s_dir, buf, cap, fmt, number);
}

void demo_set_record_dir(const char *dir) { s_rec_dir = dir ? dir : ""; }
void demo_set_pointer_bases(const DemoPointerBases &b) { s_bases = b; }
DemoPointerBases demo_pointer_bases() { return s_bases; }
bool demo_recording() { return s_out != nullptr && (g_cfg->flags & 2) != 0; }
long demo_packets_written() { return s_written; }

bool demo_open(const char *game_dir, int number) {
    s_dir = game_dir;
    demo_close();
    char path[1024];
    movie_path(path, sizeof path, "%s/mvi%05d.dat", number);
    std::FILE *f = std::fopen(path, "rb");
    s_ext = false;
    s_hdr_size = 0;
    s_version = 0;
    int slots = MC_THING_SLOTS;
    GameplayRules rules;                                        // the original's movies: faithful rules
    DemoModeHeader mh{};
    if (!f) {
        // port: a recording made with an extended pool (demo.h DemoExtHeader)
        movie_path(path, sizeof path, "%s/mvx%05d.dat", number);
        f = std::fopen(path, "rb");
        if (!f) return false;
        DemoExtHeader h{};
        DemoExtRules r{};
        bool ok = std::fread(&h, 1, sizeof h, f) == sizeof h && std::memcmp(h.magic, "MCPX", 4) == 0 &&
                  h.version >= 1 && h.version <= 4 &&
                  h.thing_slots >= (uint32_t)MC_THING_SLOTS + (h.version == 1 ? 1u : 0u) &&
                  h.thing_slots <= (uint32_t)MC_THING_SLOTS_MAX;
        if (ok && h.version >= 2) {
            ok = std::fread(&r, 1, sizeof r, f) == sizeof r && r.possession_range_pct >= 100 &&
                 r.possession_range_pct <= 200;
            rules.possession_range_pct = (int)r.possession_range_pct;
        }
        size_t mh_size = 0;
        if (ok && h.version >= 3) {                             // round 10: a game-mode run (demo.h)
            mh_size = demo_mode_header_size((int)h.version);
            uint8_t raw[sizeof mh] = {};
            ok = std::fread(raw, 1, mh_size, f) == mh_size && demo_mode_header_read(raw, (int)h.version, &mh) &&
                 mh.mode != GAME_MODE_ORIGINAL && mh.mode < GAME_MODE_COUNT && r.mode == mh.mode &&
                 mh.record_format == DEMO_RECORD_FORMAT && g_hook_demo_mode_start != nullptr;
            rules.mode = (int)mh.mode;
        }
        s_hdr_size = sizeof h + (h.version >= 2 ? sizeof r : 0) + mh_size;
        std::fclose(f);
        if (!ok) {
            std::fprintf(stderr, "movie %d: %s is not a recording this port can play\n", number, path);
            return false;
        }
        s_ext = true;
        s_version = (int)h.version;
        slots = (int)h.thing_slots;
    } else {
        std::fclose(f);
    }
    // port: the pool the recording was made with (the original's movies: 1000 slots); demo_load_state
    // sizes the pool when the snapshot loads, demo_close drops the override.
    if (slots != thing_pool_wanted_slots() || s_forced) { thing_pool_force_slots(slots); s_forced = true; }
    if (s_version >= 3) {
        // round 10: the recording's mode run (mode_start_run: the mode block, g_hook_level_source); its
        // state comes from the gax snapshot like every other part of the movie
        if (!g_hook_demo_mode_start(game_dir, mh.mode, mh.params)) {
            std::fprintf(stderr, "movie %d: mode %u could not be started\n", number, (unsigned)mh.mode);
            if (s_forced) { thing_pool_force_slots(0); s_forced = false; }
            s_ext = false;
            s_version = 0;
            return false;
        }
        s_mode_started = true;
    }
    gameplay_force_rules(&rules);                               // port: the recording's rules (demo_close drops them)
    s_rules_forced = true;
    g_cfg->movie = (uint16_t)number;
    g_cfg->flags = (uint16_t)((g_cfg->flags & ~2) | 4);
    return true;
}

bool demo_playing() { return (g_cfg->flags & 4) != 0; }

bool demo_step() {
    if (!demo_playing()) return false;
    game_tick_sim();
    return demo_playing();
}

long demo_packets_read() {
    if (s_version >= 3) return s_rec_read;
    return (long)((s_pos > s_data_off ? s_pos - s_data_off : 0) / sizeof(CmdPacket));
}
long demo_packets_total() {
    if (s_version >= 3) return s_rec_total;
    return (long)((s_file.len > s_data_off ? s_file.len - s_data_off : 0) / sizeof(CmdPacket));
}
bool demo_extended() { return s_ext; }
int demo_version() { return s_ext ? s_version : 0; }

// file_read_61a40(handle, buf, len): returns the number of bytes read.
static size_t movie_read(void *dst, size_t len) {
    size_t left = s_file.data ? s_file.len - s_pos : 0;
    if (len > left) len = left;
    if (len) std::memcpy(dst, s_file.data + s_pos, len);
    s_pos += len;
    return len;
}

// demo_close_3c7c0
void demo_close() {
    if (handle_get() != 0) {
        if (s_out) {                                            // file_close_61a10
            std::fclose(s_out);
            s_out = nullptr;
        }
        handle_set(0);
        g_cfg->flags &= ~6;
    }
    if (s_forced) { thing_pool_force_slots(0); s_forced = false; }   // port: back to the setting at the next level
    if (s_rules_forced) { gameplay_force_rules(nullptr); s_rules_forced = false; }
    // round 10: the mode run a v3 movie started ends with it
    if (s_mode_started) { s_mode_started = false; if (g_hook_demo_mode_stop) g_hook_demo_mode_stop(); }
    // port: the buffer is kept until the next demo_open so demo_packets_* stay valid
}

// Round 10: the tail of a v3 gax snapshot (demo.h): the mode block and the simulation globals outside
// GameState that a mode run depends on.
static void put_u32(std::vector<uint8_t> *v, uint32_t x) {
    for (int i = 0; i < 4; i++) v->push_back((uint8_t)(x >> (8 * i)));
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static std::vector<uint8_t> save_v3_tail() {
    std::vector<uint8_t> v;
    const size_t m = mode_serialised_size();
    put_u32(&v, (uint32_t)m);
    const size_t at = v.size();
    v.resize(at + m);
    mode_serialise(v.data() + at);
    uint32_t g[DEMO_SIM_GLOBALS] = {};
    if (g_hook_demo_sim_globals_get) g_hook_demo_sim_globals_get(g);
    put_u32(&v, DEMO_SIM_GLOBALS + 1);
    put_u32(&v, g_rng16);
    for (uint32_t x : g) put_u32(&v, x);
    return v;
}
// Bytes of a well-formed v3 tail at p (0 = malformed).
static size_t v3_tail_size(const uint8_t *p, size_t len) {
    if (len < 4) return 0;
    const size_t m = get_u32(p);
    if (m > len || len - 4 < m + 4) return 0;
    const size_t n = get_u32(p + 4 + m);
    if (n > (len - 8 - m) / 4) return 0;
    return 8 + m + 4 * n;
}

// Round 11: the world block of a gax snapshot of a world wider than 256 cells (demo.h):
//   "WRLD", u32 version 1, u32 size N, u32 slots, ThingHi[slots], WorldHi.
constexpr uint32_t DEMO_WRLD_VERSION = 1;
static size_t wrld_block_size(uint32_t slots) { return 16 + (size_t)slots * sizeof(ThingHi) + sizeof(WorldHi); }
static void save_wrld_block(std::vector<uint8_t> *v) {
    const uint32_t slots = (uint32_t)thing_pool_slots();
    for (char c : {'W', 'R', 'L', 'D'}) v->push_back((uint8_t)c);
    put_u32(v, DEMO_WRLD_VERSION);
    put_u32(v, (uint32_t)g_world.size);
    put_u32(v, slots);
    for (uint32_t i = 0; i < slots; i++) {
        const ThingHi hi = thing_hi(*thing_at(i));
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&hi);
        v->insert(v->end(), b, b + sizeof hi);
    }
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&g_world_hi);
    v->insert(v->end(), b, b + sizeof g_world_hi);
}
// The block at p (len bytes, exactly the block): its size N (0 = not a valid block for `slots`).
static uint32_t wrld_block_check(const uint8_t *p, size_t len, uint32_t slots) {
    if (len != wrld_block_size(slots) || std::memcmp(p, "WRLD", 4) != 0 || get_u32(p + 4) != DEMO_WRLD_VERSION ||
        get_u32(p + 12) != slots) return 0;
    const uint32_t n = get_u32(p + 8);
    return n > 256 && n <= (uint32_t)MC_WORLD_MAX && !(n & (n - 1)) ? n : 0;
}

static bool load_v3_tail(const uint8_t *p, size_t len) {
    if (len < 4) return false;
    const size_t m = get_u32(p);
    if (len < 4 + m + 4 || !mode_deserialise(p + 4, m)) return false;
    p += 4 + m;
    len -= 4 + m;
    const uint32_t n = get_u32(p);
    if (n < 1 || len < 4 + (size_t)n * 4) return false;
    g_rng16 = (uint16_t)get_u32(p + 4);
    uint32_t g[DEMO_SIM_GLOBALS] = {};
    for (uint32_t i = 0; i < n - 1 && i < (uint32_t)DEMO_SIM_GLOBALS; i++) g[i] = get_u32(p + 8 + 4 * i);
    if (g_hook_demo_sim_globals_set) g_hook_demo_sim_globals_set(g);
    return true;
}

// demo_load_state_3c200: movie/gam%05d.dat over the GameState, keeping the first dword and the
// option bytes +0x2195..+0x21b8, then demo_relink_state_pointers_3dc10 and models_initialise.
bool demo_load_state(int number) {
    // port: the full quick load (PortSettings::quicksave_full, savegame.h savestate_*)
    if (number == 10000 && g_settings.quicksave_full && g_hook_demo_quick_load) return g_hook_demo_quick_load();
    const bool ext = s_ext && number != 10000;                  // port: gax%05d.dat of an extended-pool movie
    // round 11: the original's GameState-only quick save cannot hold a world wider than 256 cells (and holds
    // no maps to come back to 256 with): refused there (the full quick save handles any world)
    if (!ext && world_wide()) return false;
    char path[1024];
    movie_path(path, sizeof path, ext ? "%s/gax%05d.dat" : "%s/gam%05d.dat", (int16_t)number);
    mc_blob gam;
    if (!mc_read_unpacked(path, &gam)) return false;            // demo_state_file_exists_3c310
    constexpr size_t kImage = MC_STATE_IMAGE_SIZE;
    if (ext ? gam.len < kImage + 4 : gam.len != kImage) { mc_blob_free(&gam); return false; }
    // file_load_rnc_3cbe0 over the state, keeping the first dword and the three 12-byte option blocks
    // 0x2195, 0x21a1, 0x21ad (round 11: the file is the original's image, mc_image.h converts it)
    bool ok;
    if (ext) {
        // port: the GameState is in the port's index form already; the extension's Things follow it
        static GameStateImage img;
        std::memcpy(&img, gam.data, kImage);
        std::memcpy(&img, g_state, 4);
        std::memcpy(reinterpret_cast<uint8_t *>(&img) + 0x2195, reinterpret_cast<const uint8_t *>(g_state) + 0x2195, 0x24);
        uint32_t slots = 0;
        std::memcpy(&slots, gam.data + kImage, 4);
        const size_t n = slots > (uint32_t)MC_THING_SLOTS ? slots - (uint32_t)MC_THING_SLOTS : 0;
        const size_t base = kImage + 4 + n * sizeof(ThingImage);
        ok = slots == (uint32_t)thing_pool_wanted_slots() && gam.len >= base;
        // round 11: the snapshot's end (+ the v3 tail), then nothing or the world block (demo.h)
        size_t end = base;
        if (ok && s_version >= 3) {
            const size_t tail = v3_tail_size(gam.data + base, gam.len - base);
            ok = tail != 0;
            end += tail;
        }
        uint32_t world = 256;
        const uint8_t *wrld = nullptr;
        if (ok && gam.len != end) {
            world = wrld_block_check(gam.data + end, gam.len - end, slots);
            // v3 snapshots of round 10 were read with ">= base": trailing bytes that are no block stay accepted
            if (world) wrld = gam.data + end;
            else { world = 256; ok = s_version >= 3; }
        }
        // the world size before any coordinate is stored (WCoord wraps at g_world.units)
        if (!world_set_size((int)world)) ok = false;
        std::vector<ThingHi> hi(wrld ? slots : 0);
        if (wrld) {
            std::memcpy(hi.data(), wrld + 16, hi.size() * sizeof(ThingHi));
            std::memcpy(&g_world_hi, wrld + 16 + hi.size() * sizeof(ThingHi), sizeof g_world_hi);
        } else {
            std::memset(&g_world_hi, 0, sizeof g_world_hi);
        }
        state_from_image(img, g_state, wrld ? hi.data() : nullptr);
        thing_pool_ext_reset();
        for (size_t i = 0; ok && i < n; i++) {
            ThingImage ti;
            std::memcpy(&ti, gam.data + kImage + 4 + i * sizeof(ThingImage), sizeof ti);
            thing_from_image(ti, thing_at((unsigned)(MC_THING_SLOTS + i)), wrld ? &hi[MC_THING_SLOTS + i] : nullptr);
        }
        if (ok && s_version >= 3) ok = load_v3_tail(gam.data + base, gam.len - base);
    } else {
        // demo_relink_state_pointers_3dc10 re-points the players' things at their records and rebases
        // the descriptor pointers; the port converts every pointer field to its index form.
        ok = thing_load_snapshot_image(gam.data, g_state, true);
        thing_pool_ext_reset();                                 // port: a 1000-slot image, empty extension
    }
    mc_blob_free(&gam);
    models_initialise();
    g_state->active_top = -1;
    // port: a full quick load without its hook still leaves consistent cell lists
    if (number == 10000 && g_settings.quicksave_full) demo_repair_cell_lists();
    return ok;
}

// demo_load_terrain_3c360: type, height, light, flags (0x10000 each), the cell -> thing index
// (0x20000) and the 0x12c2-byte corner-class texture table DAT_000b58b0.
bool demo_load_terrain(int number) {
    char path[1024];
    movie_path(path, sizeof path, (s_ext && number != 10000) ? "%s/max%05d.dat" : "%s/map%05d.dat", (int16_t)number);
    mc_blob map;
    if (!mc_read_file(path, &map)) return false;                // demo_terrain_file_exists_3c4f0
    size_t off = 0;
    auto rd = [&](void *dst, size_t len) {                      // file_read: short reads leave the rest untouched
        size_t n = off < map.len ? map.len - off : 0;
        if (n > len) n = len;
        if (n) std::memcpy(dst, map.data + off, n);
        off += n;
    };
    // round 11: a world wider than 256 cells (set by its gax snapshot, which demo_load_state reads first)
    // has the wide map format of demo.h: "MCWM", u32 N, then the maps at N*N
    const size_t cells = g_world.cells;
    if (world_wide()) {
        const bool ok = map.len == 8 + cells * 6 + 0x12c2 && std::memcmp(map.data, "MCWM", 4) == 0 &&
                        get_u32(map.data + 4) == (uint32_t)g_world.size;
        if (!ok) { mc_blob_free(&map); return false; }
        off = 8;
    }
    rd(g_map_type, cells);
    rd(g_map_height, cells);
    rd(g_map_light, cells);
    rd(g_map_flags, cells);
    rd(g_cell_things, cells * 2);
    rd(g_corner_tex_table, 0x12c2);
    mc_blob_free(&map);
    return true;
}

// The original's GameState holds pointers where the port holds indices / offsets (mc_types.h); the
// inverse of thing_relink_snapshot: Thing.next and the free / recyclable stacks -> &things[i],
// Thing.desc -> &g_move_desc[i], Thing.player -> GameState + offset (0 = the dummy block).
// demo_load_state_3c200 reads it back through thing_relink_snapshot, the original through
// demo_relink_state_pointers_3dc10 + models_initialise_354c0 (which rebuilds both stacks, so only
// Thing.next / .player and the rebased .desc matter to it).
void demo_state_to_original(const GameState *src, uint8_t *dst) {
    state_to_image(*src, reinterpret_cast<GameStateImage *>(dst));
    const uint32_t state_base = s_bases.state;
    const uint32_t things_base = state_base + (uint32_t)offsetof(GameStateImage, things);
    auto ptr = [&](int32_t idx) -> uint32_t {
        return (idx > 0 && idx < MC_THING_SLOTS) ? things_base + (uint32_t)idx * (uint32_t)sizeof(ThingImage) : 0u;
    };
    auto put = [&](size_t off, uint32_t v) { std::memcpy(dst + off, &v, 4); };
    for (int i = 0; i < 1000; i++) {
        put(offsetof(GameStateImage, free_list) + (size_t)i * 4, ptr(src->free_list[i]));
        put(offsetof(GameStateImage, active_list) + (size_t)i * 4, ptr(src->active_list[i]));
    }
    for (int i = 0; i < MC_THING_SLOTS; i++) {
        const Thing &t = src->things[i];
        const size_t base = offsetof(GameStateImage, things) + (size_t)i * sizeof(ThingImage);
        put(base + offsetof(ThingImage, next), ptr((int32_t)t.next));
        if (i != 0 && t.cls == 0) {          // a free slot: whatever the port left there
            put(base + offsetof(ThingImage, desc), t.desc ? s_bases.move_desc + t.desc * (uint32_t)sizeof(MoveDesc) : 0u);
            put(base + offsetof(ThingImage, player), t.player ? state_base + t.player : 0u);
            continue;
        }
        put(base + offsetof(ThingImage, desc), s_bases.move_desc + t.desc * (uint32_t)sizeof(MoveDesc));
        put(base + offsetof(ThingImage, player), t.player ? state_base + t.player : s_bases.dummy_block);
    }
}

// demo_save_state_3c2c0: file_save(movie/gam%05d.dat, g_state, 0x38d03); true when every byte was written.
bool demo_save_state(int number) {
    // port: the full quick save (PortSettings::quicksave_full, savegame.h savestate_*)
    if (number == 10000 && g_settings.quicksave_full && g_hook_demo_quick_save) return g_hook_demo_quick_save();
    if (s_rec_dir.empty()) return false;
    char path[1024];
    if (s_ext && number != 10000) {
        // port: an extended-pool recording - the port's form plus the extension's Things (demo.h)
        movie_path_in(s_rec_dir, path, sizeof path, "%s/gax%05d.dat", (int16_t)number);
        std::FILE *f = std::fopen(path, "wb");
        if (!f) return false;
        const uint32_t slots = (uint32_t)thing_pool_slots();
        const size_t n = slots - (uint32_t)MC_THING_SLOTS;
        static GameStateImage img;
        state_to_image(*g_state, &img);
        bool ok = std::fwrite(&img, 1, sizeof img, f) == sizeof img && std::fwrite(&slots, 1, 4, f) == 4;
        for (size_t i = 0; ok && i < n; i++) {
            ThingImage ti;
            thing_to_image(*thing_at((unsigned)(MC_THING_SLOTS + i)), &ti);
            ok = std::fwrite(&ti, 1, sizeof ti, f) == sizeof ti;
        }
        if (ok && s_version >= 3) {                             // round 10: the mode block + globals (demo.h)
            const std::vector<uint8_t> tail = save_v3_tail();
            ok = std::fwrite(tail.data(), 1, tail.size(), f) == tail.size();
        }
        if (ok && world_wide()) {                               // round 11: the world block (demo.h)
            std::vector<uint8_t> blk;
            save_wrld_block(&blk);
            ok = std::fwrite(blk.data(), 1, blk.size(), f) == blk.size();
        }
        ok = std::fclose(f) == 0 && ok;
        return ok;
    }
    if (world_wide()) return false;                             // round 11: see demo_load_state
    movie_path_in(s_rec_dir, path, sizeof path, "%s/gam%05d.dat", (int16_t)number);
    static uint8_t image[MC_STATE_IMAGE_SIZE];
    demo_state_to_original(g_state, image);
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    size_t n = std::fwrite(image, 1, sizeof image, f);
    std::fclose(f);
    return n == sizeof image;
}

// demo_save_terrain_3c430: the four maps, the cell heads and the corner-class table (0x612c2 bytes).
bool demo_save_terrain(int number) {
    if (s_rec_dir.empty()) return false;
    char path[1024];
    movie_path_in(s_rec_dir, path, sizeof path, (s_ext && number != 10000) ? "%s/max%05d.dat" : "%s/map%05d.dat", (int16_t)number);
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t cells = g_world.cells;                         // 0x10000: the original's file
    if (world_wide()) {                                         // round 11: the wide map format (demo.h)
        const uint32_t n = (uint32_t)g_world.size;
        std::fwrite("MCWM", 1, 4, f);
        std::fwrite(&n, 1, 4, f);
    }
    std::fwrite(g_map_type, 1, cells, f);
    std::fwrite(g_map_height, 1, cells, f);
    std::fwrite(g_map_light, 1, cells, f);
    std::fwrite(g_map_flags, 1, cells, f);
    std::fwrite(g_cell_things, 1, cells * 2, f);
    std::fwrite(g_corner_tex_table, 1, 0x12c2, f);
    std::fclose(f);
    return true;
}

// The recording half of demo_record_playback_step_3c540 (0x3c6a7..0x3c7ac).
static void demo_record_step(CmdPacket *pkt) {
    const int index = (int)(pkt - g_state->commands);
    if (handle_get() == 0 && index == 0) {
        char path[1024];
        path[0] = 0;
        // port: mvx / gax / max (extended pool / rules / mode; round 11: a world wider than 256 cells)
        s_ext = pool_extended() || rules_extended() || mode_active() || world_wide();
        s_version = 0;
        if (!s_rec_dir.empty()) movie_path_in(s_rec_dir, path, sizeof path, fmt_mvi(), (int16_t)g_cfg->movie);
        s_out = path[0] ? std::fopen(path, "wb") : nullptr;     // file_open_619a0(name, 0x222)
        if (s_out && s_ext) {
            const GameplayRules rules = gameplay_rules();
            const bool v3 = mode_active();                      // round 10: a game-mode run (demo.h)
            // round 11: a wide world without a mode is version 2 (version 1 needs an extended pool)
            const bool v2 = !v3 && (!gameplay_rules_faithful(rules) || world_wide());
            s_version = v3 ? DEMO_VERSION_MODE : v2 ? 2 : 1;
            DemoExtHeader h{{'M', 'C', 'P', 'X'}, (uint32_t)s_version, (uint32_t)thing_pool_slots(), 0u};
            DemoExtRules r{(uint32_t)rules.possession_range_pct, v3 ? g_mode.mode : 0u, {0, 0}};
            DemoModeHeader mh{g_mode.mode, g_mode.params, DEMO_RECORD_FORMAT, {0, 0}};
            if (std::fwrite(&h, 1, sizeof h, s_out) != sizeof h ||
                ((v2 || v3) && std::fwrite(&r, 1, sizeof r, s_out) != sizeof r) ||
                (v3 && std::fwrite(&mh, 1, sizeof mh, s_out) != sizeof mh)) { std::fclose(s_out); s_out = nullptr; }
        }
        if (!s_out) {                                           // -1: handle 0, record bit off
            handle_set(0);
            g_cfg->flags &= ~2;
            return;
        }
        s_written = 0;
        handle_set(2);
        if (g_hook_demo_textures_mark) g_hook_demo_textures_mark();          // texture_mark_needed_4c130
        demo_save_state((int16_t)g_cfg->movie);                              // demo_save_state_3c2c0
        demo_save_terrain((int16_t)g_cfg->movie);                            // demo_save_terrain_3c430
        models_initialise();                                                 // models_initialise_354c0
        g_state->active_top = -1;                                            // GameState+0x11f1 = -1
        g_cfg->credits_state[0] = 3;                                         // Config+0xa1 = 3
        const uint32_t wait = 200;                                           // Config+0xa2 = 200 (dword)
        std::memcpy(&g_cfg->credits_state[1], &wait, 4);
    }
    if (handle_get() == 0 || !s_out) return;
    if (s_version >= 3) {
        // round 10: v3 record = packet + u16 blob length + the player's order blob (demo.h)
        static uint8_t blob[DEMO_ORDER_BLOB_MAX];
        size_t n = g_hook_demo_order_out ? g_hook_demo_order_out(index, blob, sizeof blob) : 0;
        if (n > sizeof blob) n = sizeof blob;
        const uint8_t len[2] = {(uint8_t)n, (uint8_t)(n >> 8)};
        if (std::fwrite(pkt, 1, sizeof *pkt, s_out) != sizeof *pkt || std::fwrite(len, 1, 2, s_out) != 2 ||
            (n && std::fwrite(blob, 1, n, s_out) != n)) demo_close();
        else s_written++;
    } else if (std::fwrite(pkt, 1, sizeof *pkt, s_out) != sizeof *pkt) demo_close();      // file_write_61e20
    else s_written++;
    if (pkt->cmd == 0xc) pkt->cmd = 0;
}

// demo_record_playback_step_3c540
void demo_record_playback_step(CmdPacket *pkt) {
    if (!(g_cfg->flags & 4)) {
        if (!(g_cfg->flags & 2)) return;
        demo_record_step(pkt);
        return;
    }
    const int index = (int)(pkt - g_state->commands);
    if (handle_get() == 0 && index == 0) {
        char path[1024];
        movie_path(path, sizeof path, fmt_mvi(), (int16_t)g_cfg->movie);
        mc_blob_free(&s_file);
        s_data_off = s_ext ? s_hdr_size : 0;                    // port: skip the mvx header
        s_pos = s_data_off;
        if (!mc_read_file(path, &s_file) || s_file.len < s_data_off) {   // file_open_619a0 failed
            handle_set(0);
            g_cfg->flags &= ~4;
            if (s_rules_forced) { gameplay_force_rules(nullptr); s_rules_forced = false; }
            return;
        }
        handle_set(1);
        s_rec_read = 0;
        s_rec_total = s_version >= 3 ? count_v3_records(s_file.data, s_file.len, s_data_off) : 0;
        demo_load_state((int16_t)g_cfg->movie);
        demo_load_terrain((int16_t)g_cfg->movie);
        if (g_hook_demo_textures_reload) g_hook_demo_textures_reload();     // texture_load_needed_4c1a0
        if (g_hook_demo_input_snapshot) g_hook_demo_input_snapshot();       // input_snapshot_34060
    }
    if (handle_get() == 0) return;
    auto stop = [&]() {
        demo_close();
        g_state->players[g_state->local_player & 7].status = 8;
        pkt->cmd = 0;
    };
    if (pkt->cmd == 2) { stop(); return; }
    if (s_version >= 3) {
        // round 10: v3 record (demo.h); a truncated record ends the movie like a short read
        uint8_t len[2] = {0, 0};
        const bool ok = movie_read(pkt, sizeof *pkt) == sizeof *pkt && movie_read(len, 2) == 2;
        const size_t n = (size_t)len[0] | ((size_t)len[1] << 8);
        if (ok && s_file.len - s_pos >= n) {
            if (g_hook_demo_order_in) g_hook_demo_order_in(index, s_file.data + s_pos, n);
            s_pos += n;
            s_rec_read++;
        } else {
            stop();
        }
    } else if (movie_read(pkt, sizeof *pkt) != sizeof *pkt) stop();
    if (pkt->cmd != 2 && !(g_hook_demo_input_changed && g_hook_demo_input_changed())) return;
    stop();
}

// ---- port round 7: cell-list repair (demo.h) -------------------------------------------------------
// Walk every list from g_cell_things; a problem is a link out of the pool, a Thing reached twice (a
// cycle or two lists sharing a tail), a Thing without the "linked" flag 4, one in another cell's list,
// a cell_prev that does not point back, or a linked Thing that no list reaches.
static int cell_list_problems(std::string *mark) {
    const int n = thing_pool_slots();
    mark->assign((size_t)n, 0);
    int problems = 0;
    for (uint32_t cell = 0; cell < g_world.cells; cell++) {    // round 11: every cell of the world
        unsigned prev = 0;
        for (unsigned idx = g_cell_things[cell]; idx != 0; ) {
            if (idx >= (unsigned)n || (*mark)[idx]) { problems++; break; }
            const Thing *t = thing_at(idx);
            if (!(t->flags & 4) || world_cell_of(t->x, t->y) != cell || t->cell_prev != prev) { problems++; break; }
            (*mark)[idx] = 1;
            prev = idx;
            idx = t->cell_next;
        }
    }
    for (int i = 1; i < n; i++)
        if ((thing_at((unsigned)i)->flags & 4) && !(*mark)[(size_t)i]) problems++;
    return problems;
}

int demo_repair_cell_lists() {
    std::string mark;
    const int problems = cell_list_problems(&mark);
    if (problems == 0) return 0;
    const int n = thing_pool_slots();
    // 1. the heads the Things' own links name: the lists as they were when the Things were saved
    std::memset(g_cell_things, 0, sizeof(uint16_t) * g_world.cells);
    for (int i = 1; i < n; i++) {
        const Thing *t = thing_at((unsigned)i);
        if ((t->flags & 4) && t->cell_prev == 0) g_cell_things[world_cell_of(t->x, t->y)] = (uint16_t)i;
    }
    if (cell_list_problems(&mark) == 0) return problems;
    // 2. relink every linked Thing, lists in index order
    std::memset(g_cell_things, 0, sizeof(uint16_t) * g_world.cells);
    for (int i = n - 1; i >= 1; i--) {
        Thing *t = thing_at((unsigned)i);
        if (!(t->flags & 4)) continue;
        const uint32_t cell = world_cell_of(t->x, t->y);
        t->cell_prev = 0;
        t->cell_next = g_cell_things[cell];
        if (t->cell_next) thing_at(t->cell_next)->cell_prev = (uint16_t)i;
        g_cell_things[cell] = (uint16_t)i;
    }
    return problems;
}
