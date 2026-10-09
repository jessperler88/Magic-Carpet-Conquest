// Extended renderer internals (round 7, task A): the display list that render_ext.cpp builds from the
// visible terrain cells and Things, and the band rasteriser (render_ext_raster.cpp) that draws it.
// Nothing here is part of carpet.exe; the public entry point is render_view_ext (render.h).
//
// The list is built once per frame on the calling thread (projection, culling, sprite cache access)
// and then drawn by several threads, each into its own horizontal band of the target, in list order
// (painter's order, far to near). No global render state (g_rt_*, g_fill_mode, g_texture_ptr) is used.
#pragma once
#include <cstdint>

// One triangle vertex on screen: x, y integer pixels (any range, the rasteriser clips), u / v / shade in
// 16.16 as the original's PolyVertex, hz = haze (blend towards the sky) 0..0x10000.
struct ExtVtx {
    int32_t x, y;
    int32_t u, v, s, hz;
};

// Terrain fill modes of the extended path (subset of raster.h's modes, same pixel formulas).
enum : uint8_t {
    EXT_FILL_FLAT_SHADED = 4,     // SHADE[g][colour]           (LOD flat colour, skirts)
    EXT_FILL_TEX_GOURAUD = 5,     // SHADE[g][texel]
    EXT_FILL_TEX_FLATLIT = 7,     // SHADE[colour][texel]       (flat-shaded textures, second surface on)
    EXT_FILL_WATER       = 26,    // translucent water over the reflection
    EXT_FILL_WATER_SKY   = 27,    // port: translucent water over the sky (water above sea level, render_ext.cpp)
};

struct ExtTri {
    ExtVtx         v[3];          // clockwise on screen (counter-clockwise ones are culled)
    const uint8_t *tex;           // 256-byte row stride texture (atlas or mip atlas)
    int32_t        uv_max;        // largest u / v of the texture in 16.16 ((B << 16) - 1)
    uint8_t        mode;
    uint8_t        colour;        // modes 4 / 7
    uint8_t        haze;          // any vertex has hz > 0
    int16_t        ymin, ymax;    // screen rows touched [ymin, ymax) (band rejection)
};

// A sprite (Thing, shadow, reflection) as an affine image: screen position of source pixel (c, r)'s
// top-left corner = o + c * a + r * b (a, b in pixels per source texel). Pixel modes as
// render_sprite_scaled_2ad60 (0 copy, 1 shade, 2/3 blend, 4/5 tint, 6/7 blend + shade, 8 darken the
// destination, 9 solid colour), `upright` selects the upright path's variants of modes 4 / 5 / 8.
struct ExtSprite {
    const uint8_t *pix;
    int32_t        sw, sh;        // source size; stride = sw
    float          ox, oy, ax, ay, bx, by;
    int32_t        mode, shade;   // shade = light level << 8 (mode 9: colour << 16)
    uint8_t        upright;
    uint16_t       hz;            // haze 0..0x10000 (constant over the sprite)
    int16_t        ymin, ymax;
};

// Sky as an affine texture function of the screen position (render_sky_2f080's mapping, scaled to the
// view's 4:3-equivalent width): texel = g_sky[((v >> 16) & 255) << 8 | ((u >> 16) & 255)] with
// u = u0 + x * dux + y * duy, v = v0 + x * dvx + y * dvy. `textured` false = constant `colour`.
struct ExtSky {
    bool    textured;
    uint8_t colour;
    int32_t u0, v0, dux, dvx, duy, dvy;
};

// Destination band: rows [y0, y1) of a width x height target with `pitch` bytes per row.
struct ExtBand {
    uint8_t *dest;
    int      pitch, width, height;
    int      y0, y1;
    // Water (EXT_FILL_WATER) blends with this image (same pitch as dest: the sky + the mirrored landscape drawn before
    // the real one) instead of the pixel already in dest; null = with dest (the original: whatever was drawn before).
    const uint8_t *refl = nullptr;
};

// Mix tables for the haze: g_ext_mix[k - 1][(p << 8) | sky] = palette colour nearest to
// p * (4 - k) / 4 + sky * k / 4 (k = 1..3); k = 4 is the sky colour itself.
extern uint8_t g_ext_mix[3][65536];
void ext_build_mix_tables(const uint8_t *pal6);

void ext_fill_triangle(const ExtBand &band, const ExtSky &sky, const ExtTri &t);
void ext_draw_sprite(const ExtBand &band, const ExtSky &sky, const ExtSprite &s);
void ext_draw_sky(const ExtBand &band, const ExtSky &sky);

// Per-frame counters of the last render_view_ext call (tests, debug overlay).
struct RenderExtStats {
    int triangles = 0, quads = 0, skirts = 0, sprites = 0, things_seen = 0, things_drawn = 0;
    int levels = 0, threads = 0, bands = 0;
    double draw_radius = 0;      // cells
    double build_ms = 0, raster_ms = 0, post_ms = 0;   // display list / bands / filters (wall clock)
};
extern RenderExtStats g_render_ext_stats;
// Test hook (null by default): when set, every terrain quad of render_view_ext increments the count of
// the map cells it covers (g_world.cells entries - 65536 in the original's world - indexed by world_cell,
// saturating) - "no cell drawn twice".
extern uint8_t *g_render_ext_cell_count;
// Test hook: where the last render_view_ext drew Thing `idx` (after interpolation and the sleeping-segment
// layout, see thing_pos in render_ext.cpp). False when the slot was not laid out as a segment last frame.
bool render_ext_segment_pos(unsigned idx, int32_t *x, int32_t *y, int32_t *z);
