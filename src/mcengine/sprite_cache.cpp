// Sprite cache of carpet.exe (0x4b580..0x4c27a): tmaps.dat chunk loading by group, LRU eviction,
// resident groups, per-level load priorities, the state+0x2c "needed" table of the demo recorder and
// the per-tick FLIC animation of the animated sprites (texture_anim_update_4be50 with
// texture_anim_next_frame_4c9f0 and the SS2 part of flic_play_chunk_50dfd).
//
// Sources: disassembly of 0x4b580..0x4c27a, 0x4c880..0x4ca50, 0x50de0..0x510ca.
//
// Port simplifications (see docs/analysis/port_sprites.md):
//  * No fixed-size pool (tmap_cache_create_4c280 and friends): every chunk is its own heap block and
//    tmap_free_space() reports 0x7fffffff, so sprite_ensure_loaded never has to evict and the
//    priority loader loads every wanted group. The bookkeeping the game and the renderer can observe
//    is kept: group ids, g_sprite_ptr, LRU stamps, lock and priority tables, the "drawn" flag bit.
//  * DAT_000b84f8 (second copy of the pool handle) has no counterpart.
//  * The 0x1c-byte animation records (texture_anim_table_create_4c7f0) are one record per sprite
//    instead of a searched table.
#include "sprites.h"
#include "gen/sprites_tables.h"
#include "tmaps.h"
#include <cstdlib>
#include <cstring>

uint8_t *g_sprite_ptr[MC_SPRITE_SLOTS];
uint32_t g_sprite_group_stamp[MC_SPRITE_SLOTS];
uint8_t  g_sprite_locked[MC_SPRITE_SLOTS];
uint8_t  g_sprite_group_priority[MC_SPRITE_SLOTS];
uint8_t  g_sprite_resident_enabled = 1;                 // DAT_000987e8

static mc_tmap_set s_tmaps;                             // DAT_000987e4 (file handle) + DAT_000b84f4 (directory)
static bool        s_tmaps_open = false;
static bool        s_cache_ready = false;               // DAT_000adf58 != 0
static size_t      s_sprite_bytes[MC_SPRITE_SLOTS];     // size of the loaded chunk

// Animation record of an animated sprite (0x1c bytes in the exe, filled by sprite_cache_register_4c880).
struct SpriteAnim {
    uint8_t  active;     // +0x00
    uint32_t offset;     // +0x08 next frame chunk, relative to the pixels
    uint16_t first;      // +0x0e offset of the first frame chunk (w*h + 6, 16 bits)
    uint16_t count;      // +0x10 frames in the loop
    uint16_t width;      // +0x12 (flic_set_dest x = line stride)
    uint16_t height;     // +0x14
    uint16_t frame;      // +0x16 1-based number of the next frame
};
static SpriteAnim s_anim[MC_SPRITE_COUNT];

// DAT_0009e468: "keep destination where the source byte is 0" flag of the FLIC decoder; only read by
// the odd-width last-byte opcode. The FLI player toggles it; it is 0 while the game runs.
static uint8_t s_flic_transparent = 0;

// ---- directory ------------------------------------------------------------------------------------

uint16_t sprite_group_of(unsigned sprite) {
    if (sprite >= MC_SPRITE_COUNT && sprite < MC_SPRITE_SLOTS) return (uint16_t)sprite;   // port sprite: its own group
    return s_tmaps_open && sprite < s_tmaps.count ? s_tmaps.entries[sprite].group : 0;
}

bool sprite_port_set(unsigned sprite, const uint8_t *chunk, size_t len) {
    if (sprite < MC_SPRITE_COUNT || sprite >= MC_SPRITE_SLOTS) return false;
    std::free(g_sprite_ptr[sprite]);
    g_sprite_ptr[sprite] = nullptr;
    s_sprite_bytes[sprite] = 0;
    if (!chunk || len < 6) return true;
    g_sprite_ptr[sprite] = static_cast<uint8_t *>(std::malloc(len));
    if (!g_sprite_ptr[sprite]) return false;
    std::memcpy(g_sprite_ptr[sprite], chunk, len);
    s_sprite_bytes[sprite] = len;
    return true;
}
uint32_t sprite_chunk_size(unsigned sprite) {
    return s_tmaps_open && sprite < s_tmaps.count ? s_tmaps.entries[sprite].unpacked_size : 0;
}
size_t sprite_loaded_bytes(unsigned sprite) {
    return sprite < MC_SPRITE_SLOTS && g_sprite_ptr[sprite] ? s_sprite_bytes[sprite] : 0;
}

// tmap_free_space_4c550: free bytes of the pool. The port allocates from the heap.
uint32_t tmap_free_space() { return 0x7fffffff; }

// tmaps_load_4b580: the exe only opens data/tmaps.dat here (the directory tmaps.tab is part of the
// start-up file list); the port reads both files into memory.
bool tmaps_load(const char *game_dir) {
    if (s_tmaps_open) return true;
    if (!mc_tmap_set_load(game_dir, &s_tmaps)) return false;
    s_tmaps_open = true;
    return true;
}

// ---- FLIC frames of animated sprites ----------------------------------------------------------------

static inline unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

// fli_decode_ss2_50f71: word-oriented delta chunk. `pix` is the destination (the sprite pixels) and
// the data source (the frames follow the pixels in the same chunk); `p` is the read offset, `area`
// the pixel bytes, `cap` the bytes readable from pix. Returns false when the data would make the
// decoder leave the chunk (the exe has no such checks).
static bool flic_decode_ss2(uint8_t *pix, uint32_t area, uint32_t cap, uint32_t &p, uint32_t stride) {
    if (p + 2 > cap) return false;
    unsigned lines = rd16(pix + p); p += 2;             // [ebp-8]
    uint32_t d = 0;                                      // edi - dest
    do {
        uint32_t line_start;
        unsigned op;
        for (;;) {
            if (p + 2 > cap) return false;
            op = rd16(pix + p); p += 2;
            line_start = d;                              // [ebp-0x10]
            if (!(op & 0x8000)) break;
            if (op & 0x4000) {                           // skip -op lines
                d += (uint32_t)(-(int32_t)(int16_t)op) * stride;
                continue;
            }
            // Last byte of an odd-width line. The exe then uses the same word as the packet count
            // (it does not read a new one), so this opcode cannot occur in working data.
            const uint32_t at = d + stride - 1;
            if (at >= area) return false;
            if (s_flic_transparent != 1 || (op & 0xff) != 0) pix[at] = (uint8_t)op;
            break;
        }
        for (unsigned packets = op; packets != 0; packets--) {
            if (p + 2 > cap) return false;
            d += pix[p];                                 // column skip
            const int8_t n = (int8_t)pix[p + 1];
            p += 2;
            if (n > 0) {                                 // copy n words
                const uint32_t bytes = (uint32_t)n * 2;
                if (p + bytes > cap || d + bytes > area) return false;
                std::memmove(pix + d, pix + p, bytes);
                p += bytes; d += bytes;
            } else {                                     // fill -n words (0 = 256: inc dl / jne)
                const uint32_t words = n == 0 ? 256u : (uint32_t)(-(int)n);
                if (p + 2 > cap || d + words * 2 > area) return false;
                const uint8_t lo = pix[p], hi = pix[p + 1];
                p += 2;
                for (uint32_t k = 0; k < words; k++) { pix[d] = lo; pix[d + 1] = hi; d += 2; }
            }
        }
        d = line_start + stride;
        lines = (lines - 1) & 0xffff;
    } while (lines != 0);
    return true;
}

// flic_play_chunk_50dfd(frame, dest) after flic_set_dest_50de0(width, 0): decodes one 0xF1FA frame.
// `off` (relative to pix) is advanced behind the frame. Chunk types: 7 = SS2 delta; 4 (palette) and
// everything else are skipped by their 16-bit size; 15 (byte run) is skipped too - the exe would
// decode it with a line count of 0 (= 65536 lines) because flic_set_dest passes y = 0, and no sprite
// in tmaps.dat contains one.
static bool flic_play_frame(uint8_t *pix, uint32_t area, uint32_t cap, uint32_t &off, uint32_t stride) {
    uint32_t p = off;
    for (;;) {
        if (p + 6 > cap) return false;
        const unsigned magic = rd16(pix + p + 4);
        p += 6;
        if (magic == 0xaf12) {                           // fli_read_file_header_50f36: width / height
            if (p + 6 > cap) return false;
            stride = rd16(pix + p + 2);
            p += 6;
            continue;
        }
        if (magic != 0xf1fa) return false;               // the exe returns a null pointer
        break;
    }
    if (p + 10 > cap) return false;
    unsigned chunks = rd16(pix + p);                     // DAT_0009e466
    p += 2 + 8;
    while (chunks != 0) {
        chunks--;
        if (p + 6 > cap) return false;
        const unsigned size16 = rd16(pix + p);           // only the low word of the size is used
        const unsigned type = rd16(pix + p + 4);
        p += 6;
        if (type == 7) {
            if (!flic_decode_ss2(pix, area, cap, p, stride)) return false;
        } else {
            p += size16 - 6;                             // fli_skip_chunk_50f60
            if (p > cap) return false;
        }
    }
    off = p;
    return true;
}

// sprite_cache_register_4c880: start the animation record of a freshly loaded animated sprite.
static void sprite_anim_register(unsigned n) {
    const uint8_t *s = g_sprite_ptr[n];
    SpriteAnim &a = s_anim[n];
    const uint32_t wh = (uint32_t)rd16(s + 2) * rd16(s + 4);
    a = SpriteAnim{};
    if ((size_t)wh + 6 + 6 > s_sprite_bytes[n]) return;   // no animation data behind the pixels
    a.count  = (uint16_t)rd16(s + 6 + wh);
    a.first  = (uint16_t)(wh + 6);
    a.offset = wh + 6;
    a.width  = (uint16_t)rd16(s + 2);
    a.height = (uint16_t)rd16(s + 4);
    a.frame  = 1;
    a.active = 1;
}

// texture_anim_next_frame_4c9f0
static void texture_anim_next_frame(unsigned n) {
    SpriteAnim &a = s_anim[n];
    if (a.frame > a.count) {
        a.frame = 1;
        a.offset = a.first;
    }
    uint8_t *pix = g_sprite_ptr[n] + 6;
    const uint32_t cap = (uint32_t)(s_sprite_bytes[n] - 6);
    const uint32_t area = (uint32_t)a.width * a.height;
    uint32_t off = a.offset;
    if (flic_play_frame(pix, area, cap, off, a.width)) a.offset = off;
    else a.active = 0;                                   // port: stop animating a chunk that does not decode
    a.frame++;
}

bool sprite_anim_info(unsigned sprite, unsigned *frame, unsigned *count, uint32_t *offset) {
    if (sprite >= MC_SPRITE_COUNT || !g_sprite_ptr[sprite] || !s_anim[sprite].active) return false;
    if (frame) *frame = s_anim[sprite].frame;
    if (count) *count = s_anim[sprite].count;
    if (offset) *offset = s_anim[sprite].offset;
    return true;
}

// texture_anim_update_4be50
void texture_anim_update() {
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++) {
        uint8_t *s = g_sprite_ptr[i];
        if (!s) continue;
        uint8_t f = s[0];
        if (!(f & 8)) continue;
        f &= 0xf7;
        s[0] = f;
        if ((f & 1) && s_anim[i].active) texture_anim_next_frame(i);
    }
}

// ---- groups ---------------------------------------------------------------------------------------

// sprite_group_size_4b850
uint32_t sprite_group_size(unsigned sprite) {
    const unsigned g = sprite_group_of(sprite & 0xffff);
    uint32_t sum = 0;
    for (unsigned i = g; i < MC_SPRITE_COUNT && sprite_group_of(i) == g; i++) sum += sprite_chunk_size(i) + 10;
    return sum;
}

// tmap_read_chunk_4b5e0 + tmap_find_chunk_4c560: the exe reserves ((size + 0xd) >> 2) * 4 bytes in the
// pool and reads / RNC-unpacks the chunk into it; a missing file or a full pool leaves the sprite
// unloaded ("ERROR decompressing tmap%03d" on a bad chunk).
static bool sprite_load_chunk(unsigned i) {
    mc_tmap tm;
    if (!s_tmaps_open || !mc_tmap_get(&s_tmaps, i, &tm)) return false;
    g_sprite_ptr[i] = tm.raw;                            // ownership of the malloc'd chunk moves here
    s_sprite_bytes[i] = tm.raw_len;
    return true;
}

static void sprite_free_chunk(unsigned i) {
    s_anim[i] = SpriteAnim{};                            // sprite_cache_find_4cb10 + sprite_ptr_clear_4ca50
    std::free(g_sprite_ptr[i]);                          // sprite_cache_compact_4c610
    g_sprite_ptr[i] = nullptr;
    s_sprite_bytes[i] = 0;
    g_sprite_group_stamp[i] = 0;
}

// sprite_group_load_4b8b0
void sprite_group_load(unsigned sprite) {
    const unsigned g = sprite_group_of(sprite & 0xffff);
    const uint32_t tick = g_cfg->tick;
    for (unsigned i = g; i < MC_SPRITE_COUNT && sprite_group_of(i) == g; i++) {
        if (g_sprite_ptr[i]) continue;
        if (!sprite_load_chunk(i)) continue;
        g_sprite_group_stamp[i] = tick;
        if (g_sprite_ptr[i][0] & 1) sprite_anim_register(i);
    }
}

// sprite_group_unload_4b780
bool sprite_group_unload(unsigned sprite) {
    const unsigned g = sprite_group_of(sprite & 0xffff);
    if (g >= MC_SPRITE_COUNT || !g_sprite_ptr[g]) return false;
    for (unsigned i = g; i < MC_SPRITE_COUNT && sprite_group_of(i) == g; i++)
        if (g_sprite_ptr[i]) sprite_free_chunk(i);
    return true;
}

// sprite_group_unload_if_unlocked_4b690
bool sprite_group_unload_if_unlocked(unsigned sprite) {
    const unsigned g = sprite_group_of(sprite & 0xffff);
    if (g >= MC_SPRITE_COUNT || g_sprite_locked[g] != 0) return false;
    return sprite_group_unload(sprite);
}

// sprite_cache_evict_lru_4b9b0(bytes): unloads up to five loaded groups, least recently used first,
// until `bytes_needed` bytes are free. Resident groups are only candidates when nothing else is
// loaded. Returns the bytes freed.
uint32_t sprite_cache_evict_lru(uint32_t bytes_needed) {
    bool all_locked = true;                              // [esp+0x2c]
    uint32_t stamps[5];
    int32_t  ids[5];
    for (int k = 0; k < 5; k++) { stamps[k] = 0xffffffffu; ids[k] = -1; }

    // One step per group: the index runs to the group's last sprite, then one further.
    auto next_group = [](unsigned i, unsigned g) {
        while (i < MC_SPRITE_COUNT && sprite_group_of(i + 1) == g) i++;
        return i + 1;
    };
    for (unsigned i = 0; i < MC_SPRITE_COUNT;) {
        const unsigned g = sprite_group_of(i);
        if (g < MC_SPRITE_COUNT && g_sprite_ptr[g] && g_sprite_locked[g] == 0) all_locked = false;
        i = next_group(i, g);
    }
    for (unsigned i = 0; i < MC_SPRITE_COUNT;) {
        const unsigned g = sprite_group_of(i);
        if (g < MC_SPRITE_COUNT && !(g_sprite_locked[g] != 0 && !all_locked) && g_sprite_ptr[g]) {
            uint32_t st = g_sprite_group_stamp[g];
            int32_t id = (int32_t)g;
            for (int k = 0; k < 5; k++) {                // keep the five smallest stamps, sorted
                if (st < stamps[k]) {
                    const uint32_t ts = stamps[k]; stamps[k] = st; st = ts;
                    const int32_t ti = ids[k]; ids[k] = id; id = ti;
                }
            }
        }
        i = next_group(i, g);
    }
    uint32_t freed = 0;
    for (int k = 0; k < 5; k++) {
        if (freed >= bytes_needed) break;
        if (ids[k] <= -1) break;
        const unsigned id = (unsigned)ids[k] & 0xffff;
        const bool ok = all_locked ? sprite_group_unload(id) : sprite_group_unload_if_unlocked(id);
        if (ok) freed += sprite_group_size(id);
    }
    return freed;
}

// sprite_ensure_loaded_4bdd0
int sprite_ensure_loaded(unsigned sprite) {
    sprite &= 0xffff;
    if (sprite >= MC_SPRITE_COUNT)                       // the exe has no range check; port sprites are set or not
        return sprite < MC_SPRITE_SLOTS && g_sprite_ptr[sprite] != nullptr;
    int32_t need = (int32_t)(sprite_group_size(sprite) - tmap_free_space() + 0x14);
    for (unsigned tries = 0; need > 0 && tries < 4; tries++)
        need -= (int32_t)sprite_cache_evict_lru((uint32_t)need);
    if (need <= 0) {
        sprite_group_load(sprite);
        reinterpret_cast<uint8_t *>(g_cfg)[0x95] = 5;    // cfg+0x95: disk-activity countdown (inside Config::session in mc_types.h)
    }
    return g_sprite_ptr[sprite] != nullptr;
}

// ---- level priorities -----------------------------------------------------------------------------

// sprite_group_priorities_clear_4bec0
void sprite_group_priorities_clear() { std::memset(g_sprite_group_priority, 0, sizeof g_sprite_group_priority); }

// sprite_set_group_priority_4bf60(sprite_desc)
void sprite_set_group_priority(unsigned sprite_desc) {
    sprite_desc &= 0xffff;
    if (sprite_desc >= MC_SPRITE_DESC_COUNT) return;     // the exe indexes DAT_00097678 unchecked
    const SpriteDesc &d = g_sprite_desc[sprite_desc];
    const unsigned g = sprite_group_of(d.base_sprite);
    if (g < MC_SPRITE_COUNT) g_sprite_group_priority[g] = d.load_priority;
}

// sprite_mark_needed_for_model_4bee0(class, model, fallback): the last record of DAT_0009649e whose
// key matches lists the SpriteDesc indices the model needs; the list ends at the first negative word
// (a full record therefore continues into the next record's key and list, as in the exe).
void sprite_mark_needed_for_model(unsigned cls, unsigned model, int fallback_sprite) {
    const int16_t *const table = g_sprite_model_table;
    const int count = (int)(sizeof g_sprite_model_table / sizeof g_sprite_model_table[0]);
    const int16_t *found = nullptr;
    for (const int16_t *rec = table; rec < table + count && rec[0] >= 0; rec += 17)
        if ((int32_t)rec[0] == (int32_t)(cls & 0xffff) && (int32_t)rec[1] == (int32_t)(model & 0xffff)) found = rec + 2;
    if (found) {
        for (; found < table + count && *found >= 0; found++) sprite_set_group_priority((unsigned)*found);
    } else if ((int16_t)fallback_sprite >= 0) {
        sprite_set_group_priority((unsigned)(int16_t)fallback_sprite);
    }
}

// sprite_groups_reload_by_priority_4bfb0: called at level start after every level record was marked.
// Unloads the groups nobody needs, then loads the others in descending priority while they fit. (The
// "pool nearly full" early-out of the exe compares the constant 0x400 with the *address* of
// tmap_free_space_4c550 and therefore never triggers; texture_load_needed has the intended test.)
void sprite_groups_reload_by_priority() {
    if (!s_cache_ready) sprite_cache_init();
    std::memset(g_sprite_group_stamp, 0, sizeof g_sprite_group_stamp);
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++)
        if (g_sprite_group_priority[i] == 0) sprite_group_unload_if_unlocked(i);
    for (unsigned prio = 0xff; prio != 0; prio--) {
        for (unsigned i = 0; i < MC_SPRITE_COUNT; i++) {
            if (g_sprite_group_priority[i] != prio || g_sprite_ptr[i]) continue;
            if (sprite_group_size(i) < tmap_free_space()) sprite_group_load(i);
        }
    }
}

// ---- resident groups and the demo recorder's table ------------------------------------------------

// texture_mark_resident_4c0a0
void texture_mark_resident() {
    std::memset(g_sprite_locked, 0, sizeof g_sprite_locked);
    if (!g_sprite_resident_enabled) return;
    for (int i = 0; i < MC_SPRITE_DESC_COUNT; i++) {
        const SpriteDesc &d = g_sprite_desc[i];
        if (d.half_xy == 0 && d.half_z == 0) break;      // end of the table
        if (d.load_priority == 0xff && d.base_sprite < MC_SPRITE_COUNT) g_sprite_locked[d.base_sprite] = 0xff;
    }
}

// texture_load_resident_4c0f0
void texture_load_resident() {
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++)
        if (g_sprite_locked[i]) sprite_group_load(i);
}

// texture_mark_needed_4c130 (the exe clears 0x214 bytes: the table and the 3 bytes behind it)
void texture_mark_needed() {
    std::memset(g_state->texture_needed, 0, 0x214);
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++) {
        if (!g_sprite_ptr[i]) continue;
        g_state->texture_needed[i] = 1;
        if (g_sprite_locked[i]) g_state->texture_needed[i]++;
    }
}

// texture_load_needed_4c1a0
void texture_load_needed() {
    bool full = false;
    if (!s_cache_ready) sprite_cache_init();
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++)
        if (g_state->texture_needed[i] == 0) sprite_group_unload_if_unlocked(i);
    for (unsigned prio = 2; prio != 0 && !full; prio--) {
        for (unsigned i = 0; i < MC_SPRITE_COUNT && !full; i++) {
            if (g_state->texture_needed[i] != prio || g_sprite_ptr[i]) continue;
            if (sprite_group_size(i) < tmap_free_space()) sprite_group_load(i);
            else if (tmap_free_space() < 0x400) full = true;
        }
    }
}

// ---- life cycle -----------------------------------------------------------------------------------

// sprite_cache_init_4bbf0 (tmaps_load is done by sprites_init: it needs the game directory)
void sprite_cache_init() {
    if (s_cache_ready) return;                           // the exe only calls it while DAT_000adf58 == 0
    s_cache_ready = true;
    std::memset(g_sprite_ptr, 0, MC_SPRITE_COUNT * sizeof g_sprite_ptr[0]);       // port sprites stay
    std::memset(s_sprite_bytes, 0, MC_SPRITE_COUNT * sizeof s_sprite_bytes[0]);
    std::memset(g_sprite_group_stamp, 0, sizeof g_sprite_group_stamp);
    for (SpriteAnim &a : s_anim) a = SpriteAnim{};
    texture_mark_resident();
    texture_load_resident();
}

// sprite_cache_shutdown_4bc80
void sprite_cache_shutdown() {
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++)
        if (g_sprite_ptr[i]) sprite_free_chunk(i);       // tmap_cache_destroy_4c790 / texture_anim_table_destroy_4cb50
    sprite_group_priorities_clear();
    std::memset(g_sprite_ptr, 0, MC_SPRITE_COUNT * sizeof g_sprite_ptr[0]);       // port sprites stay (sprite_port_set)
    std::memset(g_sprite_group_stamp, 0, sizeof g_sprite_group_stamp);
    s_cache_ready = false;
}

bool sprites_init(const char *game_dir) {
    if (!tmaps_load(game_dir)) return false;
    sprite_cache_init();
    return true;
}

void sprites_shutdown() {
    sprite_cache_shutdown();
    if (s_tmaps_open) {                                  // tmaps_close_file_4bcf0
        mc_tmap_set_free(&s_tmaps);
        s_tmaps_open = false;
    }
}
