// Global game state of the port: the two big blocks (g_state, g_cfg), the five 256x256 maps, the
// renderer work buffer and the data tables. Names follow ghidra/names/carpet_types.txt so the
// translated code reads like the typed decompilation.
#pragma once
#include <cstdint>
#include "mc_types.h"

extern GameState *g_state;          // 0xadf6c
extern Config    *g_cfg;            // 0xadf74

// Terrain maps, index = (y_cell << 8) | x_cell (see mc_cell(); world.h world_cell() for any world size).
// Round 11: pointers to g_world.cells entries - the original's static arrays for the 256-cell world, a heap
// block for a wider one (world.h world_set_size).
extern uint8_t  *g_map_type;     // 0xcdfb0 texture index per cell
extern uint8_t  *g_map_height;   // 0xddfb0 world height = byte * 0x20
extern uint8_t  *g_map_light;    // 0xedfb0
extern uint8_t  *g_map_flags;    // 0xfdfb0 bits0-2 class, bit3 water anim, bits4-6 rotation, bit7 built-on
extern uint16_t *g_cell_things;  // 0x10dfb0 first Thing per cell; also the generator's 16-bit scratch

// Work buffer (DAT_000adf68, 64000+ bytes): vertex grid (840 x 44), sprite column table (+0x9060),
// per-row clip triples (+0xb360), roll index list (+0xe7e0). Also the front end's swap buffer.
constexpr size_t MC_WORK_SIZE = 0x10000;
extern uint8_t    g_work_buf[MC_WORK_SIZE];
inline VertexRec *g_work_vertices() { return reinterpret_cast<VertexRec *>(g_work_buf); }

// Second full-screen buffer (DAT_000adf70): right eye / previous frame. May be null.
extern uint8_t *g_frame2;

// The tables.dat image (0x14600 bytes at 0xb99b0..0xcdfb0): shade, blend, texture averages, circle.
constexpr size_t MC_TABLES_SIZE = 0x14600;
extern uint8_t g_tables_image[MC_TABLES_SIZE];
inline uint8_t *g_shade_table()     { return g_tables_image + 0x0000; }  // 0xb99b0 [level<<8 | colour], 64 rows (33 used)
inline uint8_t *g_blend_table()     { return g_tables_image + 0x4000; }  // 0xbd9b0 [a<<8 | b]
inline uint8_t *g_tex_avg_colour()  { return g_tables_image + 0x14000; } // 0xcd9b0 per-texture average colour (+0x80 copy)
inline uint8_t *g_circle_profile()  { return g_tables_image + 0x14300; } // 0xcdcb0 u8[256] sqrt(1-x^2) scaled, [0] = 0xff

// Game palette (data/palette.dat, 6-bit VGA components) and the texture atlas.
extern uint8_t  g_palette6[768];              // DAT_000adf90 target
extern uint8_t *g_texture_atlas;              // DAT_000adf5c: block16.dat / block32.dat unpacked (256 pixels wide)
extern size_t   g_texture_atlas_size;
extern const uint8_t *g_texture_table[256];   // DAT_0009afec: atlas + row*B*256 + col*B per texture id
extern uint8_t  g_sky[65536];                 // data/sky.dat 256x256
extern int32_t  g_uv_table[256];              // DAT_00093b48 after render_set_texture_uv_scale (16.16)

// Per-view-quadrant steps and sprite descriptors as typed views over the extracted tables.
const QuadStep   *mc_quad_steps();            // 4 entries
// The sprite descriptor table (DAT_00097678) is patched at load time by
// sprite_table_init_sizes_4bd10 (missing extent from the tmap aspect ratio, draw_type from the tmap
// header), so the port keeps a run-time copy of the extracted table.
constexpr int MC_SPRITE_DESC_ORIGINAL = 285;          // the exe's table
// Port-only descriptors after it: the oak of a generated world's woods (worldgen.h). sprite_table_init_sizes
// copies the palm's (0x53) into it, so its extents never depend on the data installed; world_art.h points its
// base_sprite at Hidden Worlds' broadleaf tree when that is available (else it stays the palm, sprite 140).
constexpr int MC_SPRITE_DESC_OAK = 285;
constexpr int MC_SPRITE_DESC_COUNT = 286;
extern SpriteDesc g_sprite_desc[MC_SPRITE_DESC_COUNT];
inline SpriteDesc *mc_sprite_desc(unsigned i) { return &g_sprite_desc[i < MC_SPRITE_DESC_COUNT ? i : 0]; }
const MoveDesc   *mc_move_desc(unsigned i);   // 30 entries

// DAT_0012edae: video-mode flags. bit0 = 320x200 (UI coordinates halved), bit3 = 640x480. Game logic
// reads it too (castle footprints are halved when it is 1), so it is state, not a render setting.
// The port defaults to 8 whatever the frame buffer size: full-size castles, which is also what the
// shipped demo recording (movie/*.dat) was made with (docs/analysis/port_features.md).
extern uint16_t g_video_mode_flags;

// Allocate / free the two blocks and reset all maps. Idempotent.
void mc_globals_init();
void mc_globals_shutdown();
