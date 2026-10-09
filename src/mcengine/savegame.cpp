// Save games: fe_savegame_read_names_58f60, fe_savegame_load_59030, fe_savegame_save_591d0.
// See savegame.h for the record layout.
#define _CRT_SECURE_NO_WARNINGS
#include "savegame.h"
#include "world_set.h"
#include "frontend.h"
#include "mc_globals.h"
#include "demo.h"
#include "thing.h"
#include "terrain.h"
#include "terrain_paint.h"
#include "mc_math.h"
#include "player.h"
#include "ai_wizard.h"
#include "settings.h"
#include "projectiles.h"
#include "mode.h"
#include "world.h"
#include "mc_image.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>


static std::string s_save_dir;      // c:\carpet.cd\save
static std::string s_dos_dir;       // <original game dir>/save (read only)

void savegame_set_dirs(const char *save_dir, const char *dos_game_dir) {
    s_save_dir = save_dir ? save_dir : "";
    s_dos_dir = (dos_game_dir && *dos_game_dir) ? std::string(dos_game_dir) + "/save" : "";
}
const char *savegame_save_dir() { return s_save_dir.c_str(); }

bool savegame_slot_path(int slot, char *buf, size_t cap) {
    if (s_save_dir.empty() || slot < 0 || slot >= SAVE_SLOTS) return false;
    std::snprintf(buf, cap, "%s/carpet%02X.gam", s_save_dir.c_str(), slot);   // "c:%s\save\carpet%02X.gam"
    return true;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

void savegame_encode(const SaveGame &g, uint8_t out[SAVEGAME_FILE_SIZE]) {
    uint8_t *p = out;
    put32(p, g.version); p += 4;
    std::memcpy(p, g.name, SAVE_NAME_LEN); p += SAVE_NAME_LEN;
    std::memcpy(p, g.player_name, 0x20); p += 0x20;
    std::memcpy(p, g.call_name, 0x20); p += 0x20;
    std::memcpy(p, g.options, 0xc); p += 0xc;
    put32(p, g.checksum); p += 4;
    std::memcpy(p, g.spells, 0x18); p += 0x18;
    *p++ = g.session;
    *p++ = g.players;
    std::memcpy(p, g.options2, 0xc); p += 0xc;
}

bool savegame_decode(const uint8_t *data, size_t len, SaveGame *out) {
    if (!data || len < SAVEGAME_FILE_SIZE) return false;
    const uint8_t *p = data;
    out->version = get32(p); p += 4;
    std::memcpy(out->name, p, SAVE_NAME_LEN); p += SAVE_NAME_LEN;
    std::memcpy(out->player_name, p, 0x20); p += 0x20;
    std::memcpy(out->call_name, p, 0x20); p += 0x20;
    std::memcpy(out->options, p, 0xc); p += 0xc;
    out->checksum = get32(p); p += 4;
    std::memcpy(out->spells, p, 0x18); p += 0x18;
    out->session = *p++;
    out->players = *p++;
    std::memcpy(out->options2, p, 0xc);
    return true;
}

static uint8_t *state_bytes(size_t off) { return reinterpret_cast<uint8_t *>(g_state) + off; }
static uint8_t *cfg_bytes(size_t off) { return reinterpret_cast<uint8_t *>(g_cfg) + off; }

// fe_savegame_save_591d0: the fields in file order.
void savegame_capture(SaveGame *g, const char *slot_name) {
    std::memset(g, 0, sizeof *g);
    g->version = SAVEGAME_VERSION;
    if (slot_name) std::memcpy(g->name, slot_name, strnlen(slot_name, SAVE_NAME_LEN));   // 0x14 bytes of the name buffer
    std::memcpy(g->player_name, cfg_bytes(0x1d), 0x20);
    std::memcpy(g->call_name, cfg_bytes(0x3d), 0x20);
    std::memcpy(g->options, state_bytes(0x2195), 0xc);
    g->checksum = ((uint32_t)g_cfg->level + g_fe_session + g_fe_lobby_players) * 4;
    std::memcpy(g->spells, state_bytes(0x3bd6), 0x18);
    g->session = g_fe_session;
    g->players = g_fe_lobby_players;
    std::memcpy(g->options2, state_bytes(0x2195), 0xc);
}

int savegame_level(const SaveGame &g) {
    // idiv by the version just read (4); the two bytes are unsigned
    const int32_t q = (int32_t)g.checksum / (int32_t)(g.version ? g.version : 4);
    return q - g.session - g.players;
}

// fe_savegame_load_59030 after the version check.
bool savegame_apply(const SaveGame &g) {
    if (g.version != SAVEGAME_VERSION) return false;
    std::memcpy(cfg_bytes(0x1d), g.player_name, 0x20);
    std::memcpy(cfg_bytes(0x3d), g.call_name, 0x20);
    std::memcpy(state_bytes(0x2195), g.options, 0xc);
    std::memcpy(state_bytes(0x3bd6), g.spells, 0x18);
    g_fe_session = g.session;
    g_fe_lobby_players = g.players;
    std::memcpy(state_bytes(0x2195), g.options2, 0xc);
    g_fe_game_in_progress = 0;
    g_cfg->level = (uint16_t)savegame_level(g);
    return true;
}

static bool read_record(const char *path, SaveGame *g, size_t *got) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[SAVEGAME_FILE_SIZE];
    std::memset(buf, 0, sizeof buf);
    *got = std::fread(buf, 1, sizeof buf, f);
    std::fclose(f);
    // a short file leaves the rest of the original's buffers as they were; the port reads zeros
    savegame_decode(buf, sizeof buf, g);
    return true;
}

// Open slot `slot` for reading: the save dir first, then the DOS game's save directory.
static bool slot_read_path(int slot, char *buf, size_t cap) {
    if (slot < 0 || slot >= SAVE_SLOTS) return false;
    if (savegame_slot_path(slot, buf, cap)) {
        FILE *f = std::fopen(buf, "rb");
        if (f) { std::fclose(f); return true; }
    }
    if (!s_dos_dir.empty()) {
        std::snprintf(buf, cap, "%s/carpet%02X.gam", s_dos_dir.c_str(), slot);
        FILE *f = std::fopen(buf, "rb");
        if (f) { std::fclose(f); return true; }
    }
    return false;
}

// fe_savegame_read_names_58f60
void savegame_read_names(char names[SAVE_SLOTS][SAVE_NAME_LEN + 1]) {
    for (int i = 0; i < SAVE_SLOTS; i++) {
        char path[1024];
        SaveGame g;
        size_t got = 0;
        if (slot_read_path(i, path, sizeof path) && read_record(path, &g, &got) && got >= 4 && g.version == SAVEGAME_VERSION) {
            std::memcpy(names[i], g.name, SAVE_NAME_LEN);
            names[i][SAVE_NAME_LEN] = 0;
        } else {
            std::snprintf(names[i], SAVE_NAME_LEN + 1, "--");
        }
    }
}

bool savegame_load_file(const char *path, char *name_out) {
    SaveGame g;
    size_t got = 0;
    if (!read_record(path, &g, &got) || got < 4 || g.version != SAVEGAME_VERSION) return false;
    if (name_out) { std::memcpy(name_out, g.name, SAVE_NAME_LEN); name_out[SAVE_NAME_LEN] = 0; }
    return savegame_apply(g);
}

bool savegame_load(int slot, char *name_out) {
    char path[1024];
    if (!slot_read_path(slot, path, sizeof path)) return false;
    return savegame_load_file(path, name_out);
}

bool savegame_save_file(const char *path, const char *name) {
    SaveGame g;
    savegame_capture(&g, name);
    uint8_t buf[SAVEGAME_FILE_SIZE];
    savegame_encode(g, buf);
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(buf, 1, sizeof buf, f) == sizeof buf;
    std::fclose(f);
    return ok;
}

bool savegame_save(int slot, const char *name) {
    char path[1024];
    if (!savegame_slot_path(slot, path, sizeof path)) return false;   // saving disabled
    return savegame_save_file(path, name);
}

// ==== save anywhere (port round 7, task E) ===========================================================
static std::string s_state_error;
const char *savestate_error() { return s_state_error.c_str(); }

static uint32_t fnv1a(const uint8_t *p, size_t n, uint32_t h = 0x811c9dc5u) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

bool savestate_slot_path(int slot, char *buf, size_t cap) {
    if (s_save_dir.empty() || slot < 0 || slot >= SAVESTATE_SLOTS) return false;
    std::snprintf(buf, cap, "%s/state%02d.mcs", s_save_dir.c_str(), slot);
    return true;
}

bool savestate_allowed() {
    return g_state && g_cfg && !(g_cfg->flags & 0x10) && !(g_cfg->flags & 6);
}

namespace {
struct Writer {
    std::vector<uint8_t> buf;
    void raw(const void *p, size_t n) { const uint8_t *b = static_cast<const uint8_t *>(p); buf.insert(buf.end(), b, b + n); }
    void u32(uint32_t v) { uint8_t b[4]; put32(b, v); raw(b, 4); }
    size_t begin(const char *tag) { raw(tag, 4); u32(0); return buf.size(); }
    void end(size_t at) { put32(&buf[at - 4], (uint32_t)(buf.size() - at)); }
};
constexpr uint32_t GLOB_VERSION = 1;
constexpr uint32_t RULE_VERSION = 2;          // "RULE": u32 version, u32 possession_range_pct, (v2) u32 mode
constexpr size_t   GLOB_FIXED = 11 * 4;      // version + 9 values + dummy block size
constexpr size_t   MAPS_BYTES = 4 * 0x10000 + 0x20000 + sizeof g_corner_tex_table;   // the 256-cell world
constexpr uint32_t WRLD_VERSION = 1;
// "WRLD" (round 11, a world wider than 256 cells, savegame.h): its byte count for size n and `slots` Things.
size_t wrld_bytes(uint32_t n, uint32_t slots) {
    return 12 + (size_t)n * n * 6 + sizeof g_corner_tex_table + (size_t)slots * sizeof(ThingHi) + sizeof(WorldHi);
}
}

// "GLOB": the simulation globals that live outside GameState / Config (version 1, u32 each, then the
// dummy player block that unowned Things write into).
static void write_globals(Writer &w) {
    w.u32(GLOB_VERSION);
    w.u32(g_rng16);
    w.u32(g_snapshot_things_base);
    w.u32(g_projectile_null_hit_index);
    w.u32(0);                                   // spells.cpp DAT_000943c4: always 0 in the retail build (unused)
    w.u32(g_ai_human_wizard);
    w.u32(g_ai_rand_seed);
    w.u32((uint32_t)g_terrain_nearly_flat);
    w.u32(g_timer_ticks);
    w.u32(g_video_mode_flags);
    w.u32((uint32_t)sizeof g_dummy_player_block);
    w.raw(g_dummy_player_block, sizeof g_dummy_player_block);
}
static bool globals_valid(const uint8_t *p, size_t n) {
    return p && n >= GLOB_FIXED && get32(p) == GLOB_VERSION && get32(p + 40) == sizeof g_dummy_player_block &&
           n >= GLOB_FIXED + sizeof g_dummy_player_block;
}
static void read_globals(const uint8_t *p) {
    g_rng16 = (uint16_t)get32(p + 4);
    g_snapshot_things_base = get32(p + 8);
    g_projectile_null_hit_index = (uint16_t)get32(p + 12);
    g_ai_human_wizard = (uint16_t)get32(p + 20);
    g_ai_rand_seed = get32(p + 24);
    g_terrain_nearly_flat = (int32_t)get32(p + 28);
    g_timer_ticks = get32(p + 32);
    g_video_mode_flags = (uint16_t)get32(p + 36);
    std::memcpy(g_dummy_player_block, p + GLOB_FIXED, sizeof g_dummy_player_block);
}

bool savestate_save_file(const char *path, const char *name) {
    s_state_error.clear();
    if (!g_state || !g_cfg) { s_state_error = "no game state"; return false; }
    Writer w;
    {   static GameStateImage img;                          // round 11: the original's byte layout (mc_image.h)
        state_to_image(*g_state, &img);
        const size_t c = w.begin("GAME"); w.raw(&img, sizeof img); w.end(c); }
    {   const size_t c = w.begin("CONF"); w.raw(g_cfg, sizeof(Config)); w.end(c); }
    const int slots = thing_pool_slots();
    if (!world_wide()) {
        const size_t c = w.begin("MAPS");
        w.raw(g_map_type, 0x10000); w.raw(g_map_height, 0x10000); w.raw(g_map_light, 0x10000);
        w.raw(g_map_flags, 0x10000); w.raw(g_cell_things, 0x20000); w.raw(g_corner_tex_table, sizeof g_corner_tex_table);
        w.end(c);
    } else {
        // round 11: a wider world replaces MAPS by WRLD (savegame.h), so a reader without WRLD support refuses
        // the file (MAPS is required) instead of loading 256 cells of it
        const size_t c = w.begin("WRLD");
        const size_t cells = g_world.cells;
        w.u32(WRLD_VERSION);
        w.u32((uint32_t)g_world.size);
        w.u32((uint32_t)slots);
        w.raw(g_map_type, cells); w.raw(g_map_height, cells); w.raw(g_map_light, cells);
        w.raw(g_map_flags, cells); w.raw(g_cell_things, cells * 2); w.raw(g_corner_tex_table, sizeof g_corner_tex_table);
        for (int i = 0; i < slots; i++) {
            const ThingHi hi = thing_hi(*thing_at((unsigned)i));
            w.raw(&hi, sizeof hi);
        }
        w.raw(&g_world_hi, sizeof g_world_hi);
        w.end(c);
    }
    {   const size_t c = w.begin("GLOB"); write_globals(w); w.end(c); }
    {   const size_t c = w.begin("CAMP");
        SaveGame g;
        savegame_capture(&g, name);
        uint8_t rec[SAVEGAME_FILE_SIZE];
        savegame_encode(g, rec);
        w.raw(rec, sizeof rec);
        w.raw(&g_fe_game_in_progress, 1);
        w.end(c); }
    {   const size_t c = w.begin("RULE");                     // round 9: the gameplay rules in force
        const GameplayRules r = gameplay_rules();
        w.u32(RULE_VERSION);
        w.u32((uint32_t)r.possession_range_pct);
        w.u32((uint32_t)r.mode);
        w.end(c); }
    if (mode_active()) {                                    // round 10: the game mode's block (mode.h)
        const size_t c = w.begin("MODE");
        std::vector<uint8_t> m(mode_serialised_size());
        mode_serialise(m.data());
        w.raw(m.data(), m.size());
        w.end(c); }
    if (slots > MC_THING_SLOTS) {
        const size_t c = w.begin("POOL");
        const size_t e = (size_t)(slots - MC_THING_SLOTS);
        w.u32((uint32_t)slots);
        for (size_t i = 0; i < e; i++) {
            ThingImage ti;
            thing_to_image(*thing_at((unsigned)(MC_THING_SLOTS + i)), &ti);
            w.raw(&ti, sizeof ti);
        }
        w.raw(thing_pool_ext_free_stack(), e * 4);
        w.raw(thing_pool_ext_active_stack(), e * 4);
        w.end(c);
    }

    uint8_t hdr[SAVESTATE_HEADER_SIZE];
    std::memset(hdr, 0, sizeof hdr);
    const PlayerRec &rec = g_state->players[g_state->local_player & 7];
    std::memcpy(hdr, "MCPSTATE", 8);
    put32(hdr + 8, SAVESTATE_FORMAT);
    put32(hdr + 12, SAVESTATE_HEADER_SIZE);
    put32(hdr + 16, MC_PORT_VERSION);
    put32(hdr + 20, g_cfg->level);
    put32(hdr + 24, rec.tick);
    put32(hdr + 28, (uint32_t)slots);
    const uint64_t now = (uint64_t)std::time(nullptr);
    put32(hdr + 32, (uint32_t)now);
    put32(hdr + 36, (uint32_t)(now >> 32));
    put32(hdr + 40, g_video_mode_flags != 1 ? 1u : 0u);
    put32(hdr + 44, (uint32_t)w.buf.size());
    put32(hdr + 48, fnv1a(w.buf.data(), w.buf.size()));
    char label[52];
    if (name && *name) std::snprintf(label, sizeof label, "%s", name);
    else std::snprintf(label, sizeof label, "level %d  tick %u", (int)g_cfg->level, (unsigned)rec.tick);
    std::memcpy(hdr + 52, label, std::strlen(label));

    // Written next to the target and renamed, so a failed write never destroys the previous save.
    const std::string tmp = std::string(path) + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) { s_state_error = std::string("cannot create ") + tmp; return false; }
    bool ok = std::fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr && std::fwrite(w.buf.data(), 1, w.buf.size(), f) == w.buf.size();
    ok = std::fclose(f) == 0 && ok;
    if (ok) {
        std::remove(path);
        ok = std::rename(tmp.c_str(), path) == 0;
    }
    if (!ok) { std::remove(tmp.c_str()); s_state_error = std::string("cannot write ") + path; }
    return ok;
}

bool savestate_save(int slot, const char *name) {
    char path[1024];
    if (!savestate_slot_path(slot, path, sizeof path)) { s_state_error = "saving is disabled (no save directory)"; return false; }
    if (!savestate_allowed()) { s_state_error = "not now (network game or movie)"; return false; }
    return savestate_save_file(path, name);
}

static bool decode_header(const uint8_t *p, size_t n, SaveStateHeader *h) {
    if (n < SAVESTATE_HEADER_SIZE || std::memcmp(p, "MCPSTATE", 8) != 0) return false;
    std::memcpy(h->magic, p, 8);
    h->format = get32(p + 8);
    h->header_size = get32(p + 12);
    h->port_version = get32(p + 16);
    h->level = (int32_t)get32(p + 20);
    h->tick = get32(p + 24);
    h->thing_slots = get32(p + 28);
    h->time = (uint64_t)get32(p + 32) | ((uint64_t)get32(p + 36) << 32);
    h->flags = get32(p + 40);
    h->chunk_bytes = get32(p + 44);
    h->checksum = get32(p + 48);
    std::memcpy(h->name, p + 52, sizeof h->name);
    h->name[sizeof h->name - 1] = 0;
    return h->format == SAVESTATE_FORMAT && h->header_size >= SAVESTATE_HEADER_SIZE && h->header_size <= n &&
           (size_t)h->header_size + h->chunk_bytes == n;
}

static bool read_whole(const char *path, std::vector<uint8_t> *out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (64L << 20)) { std::fclose(f); return false; }
    out->resize((size_t)len);
    const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}

bool savestate_read_header(const char *path, SaveStateHeader *out) {
    std::vector<uint8_t> data;
    if (!read_whole(path, &data)) return false;
    SaveStateHeader h{};
    if (!decode_header(data.data(), data.size(), &h)) return false;
    if (fnv1a(data.data() + h.header_size, h.chunk_bytes) != h.checksum) return false;
    *out = h;
    return true;
}

bool savestate_load_file(const char *path) {
    s_state_error.clear();
    if (!g_state || !g_cfg) { s_state_error = "no game state"; return false; }
    if (!savestate_allowed()) { s_state_error = "not now (network game or movie)"; return false; }
    std::vector<uint8_t> data;
    if (!read_whole(path, &data)) { s_state_error = std::string("cannot read ") + path; return false; }
    SaveStateHeader h{};
    if (!decode_header(data.data(), data.size(), &h)) { s_state_error = "not a save state of this format"; return false; }
    if (fnv1a(data.data() + h.header_size, h.chunk_bytes) != h.checksum) { s_state_error = "checksum mismatch (damaged file)"; return false; }
    if (h.thing_slots < (uint32_t)MC_THING_SLOTS || h.thing_slots > (uint32_t)MC_THING_SLOTS_MAX) { s_state_error = "bad pool size"; return false; }

    // Collect and check every chunk before anything is changed.
    const uint8_t *game = nullptr, *conf = nullptr, *maps = nullptr, *glob = nullptr, *camp = nullptr, *pool = nullptr;
    const uint8_t *rule = nullptr, *mode = nullptr, *wrld = nullptr;
    size_t glob_n = 0, camp_n = 0, pool_n = 0, rule_n = 0, mode_n = 0, wrld_n = 0;
    for (size_t off = h.header_size; off + 8 <= data.size(); ) {
        const uint8_t *c = data.data() + off;
        const size_t len = get32(c + 4);
        if (off + 8 + len > data.size()) { s_state_error = "truncated chunk"; return false; }
        const uint8_t *body = c + 8;
        if (!std::memcmp(c, "GAME", 4) && len == MC_STATE_IMAGE_SIZE) game = body;
        else if (!std::memcmp(c, "CONF", 4) && len == sizeof(Config)) conf = body;
        else if (!std::memcmp(c, "MAPS", 4) && len == MAPS_BYTES) maps = body;
        else if (!std::memcmp(c, "GLOB", 4)) { glob = body; glob_n = len; }
        else if (!std::memcmp(c, "CAMP", 4)) { camp = body; camp_n = len; }
        else if (!std::memcmp(c, "POOL", 4)) { pool = body; pool_n = len; }
        else if (!std::memcmp(c, "RULE", 4)) { rule = body; rule_n = len; }
        else if (!std::memcmp(c, "MODE", 4)) { mode = body; mode_n = len; }
        else if (!std::memcmp(c, "WRLD", 4)) { wrld = body; wrld_n = len; }
        off += 8 + len;                                         // unknown chunks are skipped
    }
    // Round 11: a world wider than 256 cells has WRLD instead of MAPS (savegame.h); exactly one of them.
    uint32_t wrld_size = 256;
    if (wrld) {
        wrld_size = wrld_n >= 12 ? get32(wrld + 4) : 0;
        if (wrld_n < 12 || get32(wrld) != WRLD_VERSION || wrld_size <= 256 || wrld_size > (uint32_t)MC_WORLD_MAX ||
            (wrld_size & (wrld_size - 1)) || get32(wrld + 8) != h.thing_slots ||
            wrld_n != wrld_bytes(wrld_size, h.thing_slots)) {
            s_state_error = "world chunk of another version or size";
            return false;
        }
        if (maps) { s_state_error = "both a 256-cell map and a world chunk"; return false; }
    }
    if (!game || !conf || (!maps && !wrld)) { s_state_error = "a required chunk is missing"; return false; }
    if (!globals_valid(glob, glob_n)) { s_state_error = "globals chunk missing or of another version"; return false; }
    if (mode && !mode_serialised_size_ok(mode_n)) { s_state_error = "mode chunk of another version"; return false; }
    const size_t e = h.thing_slots - (uint32_t)MC_THING_SLOTS;
    if (e && (!pool || pool_n != 4 + e * (sizeof(ThingImage) + 8) || get32(pool) != h.thing_slots)) {
        s_state_error = "pool chunk missing or of the wrong size";
        return false;
    }

    // port: a Hidden Worlds level (world_set.h) needs its data set before anything is restored.
    {
        uint16_t lvl;
        std::memcpy(&lvl, conf + offsetof(Config, level), 2);
        if (!world_set_select(world_set_for_level(lvl))) { s_state_error = "Hidden Worlds data not installed"; return false; }
    }

    // Round 11: the saved world's size before any coordinate is stored (WCoord wraps at g_world.units);
    // world_set_size clears the maps when the size changes, they are restored below.
    if (!world_set_size((int)wrld_size)) { s_state_error = "no memory for the saved world"; return false; }
    const uint8_t *wrld_maps = wrld ? wrld + 12 : nullptr;
    const size_t wrld_cells = (size_t)wrld_size * wrld_size;
    const uint8_t *wrld_hi = wrld ? wrld_maps + wrld_cells * 6 + sizeof g_corner_tex_table : nullptr;
    std::vector<ThingHi> thi;
    if (wrld) {
        thi.resize(h.thing_slots);
        std::memcpy(thi.data(), wrld_hi, thi.size() * sizeof(ThingHi));
        std::memcpy(&g_world_hi, wrld_hi + thi.size() * sizeof(ThingHi), sizeof g_world_hi);
    } else {
        std::memset(&g_world_hi, 0, sizeof g_world_hi);
    }

    // GameState in the port's form, except the first dword and the option bytes the user set
    // (+0x2195..+0x21b8), which are kept the way demo_load_state_3c200 keeps them.
    {
        static GameStateImage img;                              // round 11: the chunk is the original's layout
        std::memcpy(&img, game, sizeof img);
        uint8_t *d = reinterpret_cast<uint8_t *>(&img);
        const uint8_t *st = reinterpret_cast<const uint8_t *>(g_state);
        std::memcpy(d, st, 4);
        std::memcpy(d + 0x2195, st + 0x2195, 0x24);
        state_from_image(img, g_state, wrld ? thi.data() : nullptr);
    }

    // Config: everything but the fields of the platform / the session.
    {
        const Config keep = *g_cfg;
        std::memcpy(g_cfg, conf, sizeof(Config));
        const uint16_t session_bits = 0x0096;                   // 2 record, 4 play, 0x10 / 0x80 network
        g_cfg->flags = (uint16_t)((g_cfg->flags & ~session_bits) | (keep.flags & session_bits));
        g_cfg->demo_file = keep.demo_file;
        g_cfg->pentium = keep.pentium;
        g_cfg->language = keep.language;
        g_cfg->frame_time = keep.frame_time;
        g_cfg->net_time = keep.net_time;
        g_cfg->pool = keep.pool;                                // the renderer's texture pool
        g_cfg->pool_size = keep.pool_size;
        g_cfg->pit_reload = keep.pit_reload;
        g_cfg->pit_accum = keep.pit_accum;
    }

    {
        const uint8_t *m = wrld ? wrld_maps : maps;
        const size_t cells = g_world.cells;                     // 0x10000 for a MAPS chunk
        std::memcpy(g_map_type, m, cells); m += cells;
        std::memcpy(g_map_height, m, cells); m += cells;
        std::memcpy(g_map_light, m, cells); m += cells;
        std::memcpy(g_map_flags, m, cells); m += cells;
        std::memcpy(g_cell_things, m, cells * 2); m += cells * 2;
        std::memcpy(g_corner_tex_table, m, sizeof g_corner_tex_table);
    }
    read_globals(glob);

    if (camp && camp_n >= SAVEGAME_FILE_SIZE) {
        SaveGame g;
        savegame_decode(camp, camp_n, &g);
        g_fe_session = g.session;
        g_fe_lobby_players = g.players;
        if (camp_n > SAVEGAME_FILE_SIZE) g_fe_game_in_progress = camp[SAVEGAME_FILE_SIZE];
    }

    // The pool: the saved size when it was extended (for the rest of this level), otherwise the
    // setting's size with an empty extension (a 1000-slot image is a valid pool of any size, thing.h).
    {
        const int32_t free_top = g_state->free_top, active_top = g_state->active_top;
        thing_pool_force_slots(e ? (int)h.thing_slots : 0);
        thing_pool_ext_reset();
        thing_pool_force_slots(0);
        if (e) {
            const uint8_t *p = pool + 4;
            for (size_t i = 0; i < e; i++, p += sizeof(ThingImage)) {
                ThingImage ti;
                std::memcpy(&ti, p, sizeof ti);
                thing_from_image(ti, thing_at((unsigned)(MC_THING_SLOTS + i)), wrld ? &thi[MC_THING_SLOTS + i] : nullptr);
            }
            std::memcpy(thing_pool_ext_free_stack(), p, e * 4); p += e * 4;
            std::memcpy(thing_pool_ext_active_stack(), p, e * 4);
            g_state->free_top = free_top;                       // ext_reset clamps a top below -1
            g_state->active_top = active_top;
        }
    }

    // The rules the state was played with, for the rest of this level (game_after_frontend releases them at
    // the next level, net_release_rules). A state without the chunk (round 7 / 8) keeps the current settings.
    // Round 10: the game mode's block; a state without one is the original game (mode_end drops a running
    // mode). Before the rules: mode_end releases forced rules, the RULE chunk forces them again.
    if (mode) mode_deserialise(mode, mode_n);
    else mode_end();
    if (rule && rule_n >= 8 && (get32(rule) == 1 || (get32(rule) == RULE_VERSION && rule_n >= 12))) {
        GameplayRules r;
        r.possession_range_pct = (int)get32(rule + 4);
        r.mode = get32(rule) >= 2 ? (int)get32(rule + 8) : 0;
        gameplay_force_rules(&r);
    }

    const int repaired = demo_repair_cell_lists();
    if (repaired) s_state_error = "loaded; " + std::to_string(repaired) + " cell-list problems repaired";
    if (g_hook_demo_textures_reload) g_hook_demo_textures_reload();     // sprites / textures the state uses
    return true;
}

bool savestate_load(int slot) {
    char path[1024];
    if (!savestate_slot_path(slot, path, sizeof path)) { s_state_error = "saving is disabled (no save directory)"; return false; }
    return savestate_load_file(path);
}

void savestate_install_quick_hooks() {
    g_hook_demo_quick_save = [] { return savestate_save(0, nullptr); };
    g_hook_demo_quick_load = [] { return savestate_load(0); };
}
