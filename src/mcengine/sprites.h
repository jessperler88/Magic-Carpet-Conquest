// Sprite cache (tmaps.dat groups, LRU) and the Thing renderer (render_cell_things_2c600 and
// friends, render_sprite_scaled_2ad60). Owner: sprite_cache.cpp + render_things.cpp.
//
// A 3D sprite is one tmaps.dat chunk: {u8 flags, u8 draw_type, u16 width, u16 height} followed by
// width*height raw pixels (0 = transparent). flags bit0 = the chunk carries a FLIC animation behind
// the pixels (frames are decoded in place by texture_anim_update), bit3 = "drawn since the last
// texture_anim_update" (set by the renderer). Sprites are loaded and evicted in groups; the group id
// in tmaps.tab is the index of the group's first sprite, and every per-group table of the exe is
// simply indexed with that sprite index.
#pragma once
#include "thing.h"
#include "render.h"

constexpr int MC_SPRITE_COUNT = 0x211;                 // tmaps.dat chunks
// Port-only sprites after the tmaps chunks (world_art.h: art imported from another data set, palette-remapped).
// Each is its own group, set with sprite_port_set, never evicted; not loaded = not drawn.
constexpr int MC_SPRITE_PORT_SLOTS = 8;
constexpr int MC_SPRITE_SLOTS = MC_SPRITE_COUNT + MC_SPRITE_PORT_SLOTS;
constexpr int MC_SPRITE_OAK = MC_SPRITE_COUNT;          // Hidden Worlds' broadleaf tree (its tmaps1-0 chunk 140)

// ---- cache state (indexed by sprite number; "group" entries use the group's first sprite) --------
extern uint8_t *g_sprite_ptr[MC_SPRITE_SLOTS];          // DAT_000b8d3c: loaded chunk (original: pointer to the pool handle), null = not loaded
extern uint32_t g_sprite_group_stamp[MC_SPRITE_SLOTS];  // DAT_000b7cb0: g_cfg->tick of the last use (LRU)
extern uint8_t  g_sprite_locked[MC_SPRITE_SLOTS];       // DAT_000b9580: 0xff = resident (SpriteDesc.load_priority == 0xff)
extern uint8_t  g_sprite_group_priority[MC_SPRITE_SLOTS]; // DAT_000b9791: load priority set by the level's models
// A port sprite (MC_SPRITE_COUNT <= sprite < MC_SPRITE_SLOTS): a copy of CHUNK (tmaps layout: u8 flags, u8 draw
// type, u16 w, u16 h, w*h pixels); null clears it. False for another index.
bool sprite_port_set(unsigned sprite, const uint8_t *chunk, size_t len);
extern uint8_t  g_sprite_resident_enabled;              // DAT_000987e8 (1; mem_init_pools_59500 sets it)

// Opens data/tmaps.dat + tmaps.tab (tmaps_load_4b580) and creates the cache (sprite_cache_init_4bbf0,
// which also loads the resident groups). False when the files are missing.
bool sprites_init(const char *game_dir);
void sprites_shutdown();                                // sprite_cache_shutdown_4bc80

// Directory (DAT_000b84f4, 10-byte tmaps.tab records).
uint16_t sprite_group_of(unsigned sprite);              // record +8; 0 for the sentinel / out of range
uint32_t sprite_chunk_size(unsigned sprite);            // record +0 (unpacked size)
size_t   sprite_loaded_bytes(unsigned sprite);          // port: size of the loaded chunk (0 = not loaded)

bool     tmaps_load(const char *game_dir);              // tmaps_load_4b580
void     sprite_cache_init();                           // sprite_cache_init_4bbf0
void     sprite_cache_shutdown();                       // sprite_cache_shutdown_4bc80 (keeps tmaps.dat open, see sprites_shutdown)
uint32_t sprite_group_size(unsigned sprite);            // sprite_group_size_4b850: sum of (chunk size + 10) over the group
void     sprite_group_load(unsigned sprite);            // sprite_group_load_4b8b0
bool     sprite_group_unload(unsigned sprite);          // sprite_group_unload_4b780
bool     sprite_group_unload_if_unlocked(unsigned sprite);  // sprite_group_unload_if_unlocked_4b690
uint32_t sprite_cache_evict_lru(uint32_t bytes_needed); // sprite_cache_evict_lru_4b9b0: returns the bytes freed
int      sprite_ensure_loaded(unsigned sprite);         // sprite_ensure_loaded_4bdd0: 1 when g_sprite_ptr[sprite] != null
uint32_t tmap_free_space();                             // tmap_free_space_4c550: the port has no pool, always 0x7fffffff

void sprite_group_priorities_clear();                   // sprite_group_priorities_clear_4bec0
void sprite_mark_needed_for_model(unsigned cls, unsigned model, int fallback_sprite);   // sprite_mark_needed_for_model_4bee0
void sprite_set_group_priority(unsigned sprite_desc);   // sprite_set_group_priority_4bf60 (argument = SpriteDesc index)
void sprite_groups_reload_by_priority();                // sprite_groups_reload_by_priority_4bfb0

void texture_mark_resident();                           // texture_mark_resident_4c0a0
void texture_load_resident();                           // texture_load_resident_4c0f0
void texture_mark_needed();                             // texture_mark_needed_4c130: g_state->texture_needed[] = 0 / 1 loaded / 2 loaded + resident
void texture_load_needed();                             // texture_load_needed_4c1a0: reload from g_state->texture_needed[] (demo state load)
// texture_anim_update_4be50: once per tick (game_tick_update_32e80, not while paused): clears the
// "drawn" bit of every loaded sprite and advances the FLIC animation of the animated ones that were
// drawn (texture_anim_next_frame_4c9f0 with the SS2 decoder of flic_play_chunk_50dfd).
void texture_anim_update();
// State of an animated sprite's record (for tests / debug overlays): false when `sprite` is not a
// loaded animated sprite. frame = 1-based number of the next frame, count = frames in the loop,
// offset = position of the next frame chunk relative to the pixels.
bool sprite_anim_info(unsigned sprite, unsigned *frame, unsigned *count, uint32_t *offset);

// ---- sprite blitter ------------------------------------------------------------------------------
// Parameter block of render_sprite_scaled_2ad60 (DAT_000b5814..DAT_000b5840, DAT_000b58ae). The
// blitter modifies x / y / dst_w / dst_h / src_x / src_y while it clips.
struct SpriteBlit {
    int32_t        mode;        // DAT_000b5814: 0 copy, 1 shade, 2/3 blend, 4/5 tint, 6/7 blend + shade, 8 darken dest (shadow), 9 solid colour
    int32_t        shade;       // DAT_000b5818: light level << 8 (0x2000 = full light; mode 9: colour << 16)
    int32_t        dst_w;       // DAT_000b581c: width on screen
    const uint8_t *pixels;      // DAT_000b5820: sprite pixels (chunk + 6)
    int32_t        y;           // DAT_000b5824: anchor point, then the top-left corner
    int32_t        x;           // DAT_000b5828
    int32_t        src_w;       // DAT_000b582c: source width, negative = mirrored
    int32_t        src_y;       // DAT_000b5830: 16.16 source row accumulator
    int32_t        src_x;       // DAT_000b5834: 16.16 source column accumulator
    int32_t        dst_h;       // DAT_000b5838: height on screen
    int32_t        src_h;       // DAT_000b583c
    int32_t        src_stride;  // DAT_000b5840: bytes per source row (= |src_w|)
    uint8_t        upright;     // DAT_000b58ae: 1 = draw unrotated (ignores the screen roll)
    uint32_t       pixels_size; // port only: readable bytes behind `pixels` (reads outside count as transparent)
    const Thing   *thing;       // port only: the thing being drawn (null for other callers), for g_sprite_blit_probe
};
extern SpriteBlit g_sprite_blit;
// Port only, null by default: called at the top of render_sprite_scaled with the untouched parameter
// block (x / y still the projected anchor point). For tests and debug overlays.
extern void (*g_sprite_blit_probe)(unsigned anchor);

// Work-buffer layout of the blitter (docs/ENGINE.md 2.2): 8-byte column records {delta, source
// column} and 12-byte per-row clip triples {first roll-table entry, count, columns skipped}.
constexpr size_t MC_SPRITE_COLTAB_OFFSET  = 0x9060;
constexpr size_t MC_SPRITE_TRIPLES_OFFSET = 0xb360;
constexpr int    MC_SPRITE_TRIPLE_MAX     = (int)((MC_ROLL_LIST_OFFSET - MC_SPRITE_TRIPLES_OFFSET) / 12);   // 1120

// render_sprite_scaled_2ad60(anchor): anchor 1 = the point (x, y) is the bottom centre (things),
// 0 = top centre, drawn upside down (shadows), 2 = same as 0 (reflections). Uses g_rcam.sin_roll /
// cos_roll, g_roll / g_roll_table and the render target of raster.h.
void render_sprite_scaled(unsigned anchor);

// Number of pixel reads / writes the port refused because they fell outside the sprite or the render
// target (the exe would have read or written stray memory). Stays 0 in every tested situation.
extern uint32_t g_sprite_oob_reads, g_sprite_oob_writes;

// ---- Thing renderer --------------------------------------------------------------------------------
void render_cell_things(int first_thing, const VertexRec *cell);            // render_cell_things_2c600
void render_cell_things_mirrored(int first_thing, const VertexRec *cell);   // render_cell_things_mirrored_2e5a0

// Installs g_render_cell_things / g_render_cell_things_mirrored and the three sprite-group hooks of
// switch_activate (thing.h).
void render_things_install();
