// The world: its size, the maps, the cell / coordinate accessors and the per-world state that does not fit the
// original's 16-bit image (Phase 4 round 11, docs/analysis/phase4_code_world.md section 7, approach B).
//
// A world is a torus of N x N cells, N a power of two from 256 (the original) to 4096. Positions are WCoord
// (mc_types.h): int32 world units wrapped into 0 .. N*256-1 on every store. With N = 256 every accessor below
// computes exactly what the original's 8-bit cell / 16-bit unit arithmetic computes (the per-tick references
// prove it), so the faithful game runs through the same code. Rules for code written against the original:
//
//   original                                  port (any N)
//   mc_cell(cx, cy), cell_xy(cx, cy)          world_cell(cx, cy)
//   mc_cell_of(x, y)                          world_cell_of(x, y)
//   (uint8_t)(cx + 1), cx & 0xff              world_wrap_cell(cx + 1)
//   (uint8_t)(x >> 8)                         world_cx(x)
//   cell & 0xff, cell >> 8                    world_cell_x(cell), world_cell_y(cell)
//   (int16_t)(b->x - a->x), (int16_t)p->x     world_signed(b->x - a->x), world_signed(p->x)
//   p->x = (uint16_t)(expr)                   p->x = expr                        (WCoord wraps)
//   uint16_t x = t->x (a position local)      int32_t x = t->x
//   dx * dx + dy * dy in int32                world_dist_sq(dx, dy)              (int64; the original's int32 wrap at N = 256)
//   math_atan2((int16_t)dx, (int16_t)dy)      math_atan2_w(dx, dy)               (thing.h)
//
// State outside GameState that only a wider world needs (the high halves of the original's u16 fields that
// are not Thing coordinates: the per-player start positions and the camera log) lives in g_world_hi; it is
// all zero with N = 256.
#pragma once
#include <cstdint>
#include <cstring>
#include "mc_globals.h"

struct World {
    int      size = 256;            // cells per side
    int      shift = 8;             // log2(size)
    uint32_t cell_mask = 0xff;      // size - 1
    int32_t  units = 0x10000;       // world units per side (size * 256)
    uint32_t unit_mask = 0xffff;    // units - 1 (= g_world_unit_mask)
    uint32_t cells = 0x10000;       // size * size
};
extern World g_world;

constexpr int MC_WORLD_MIN = 256;
constexpr int MC_WORLD_MAX = 4096;

// Size the world for the next level: N must be a power of two in MC_WORLD_MIN .. MC_WORLD_MAX (false otherwise,
// nothing changed). Points the five maps at N*N storage (the original's static arrays for 256, a heap block
// otherwise; mc_globals.h), clears them when the size changes, and sets g_world_unit_mask. Things keep their
// coordinates (call it before a level is generated or loaded).
bool world_set_size(int n);
inline bool world_wide() { return g_world.size != 256; }
// Test / prototype helper (thing.cpp): the loaded 256-cell level repeated (n/256)^2 times as an n-cell world.
// Things keep their coordinates (they stay in the first copy) and are relinked into the new cell lists.
bool world_widen_tiled(int n);

// ---- cells ---------------------------------------------------------------------------------------------
inline int world_wrap_cell(int c) { return (int)((uint32_t)c & g_world.cell_mask); }
inline uint32_t world_cell(int cx, int cy) {
    return (((uint32_t)cy & g_world.cell_mask) << g_world.shift) | ((uint32_t)cx & g_world.cell_mask);
}
inline int world_cx(int32_t x) { return world_wrap_cell(x >> 8); }        // cell column of a coordinate
inline uint32_t world_cell_of(int32_t x, int32_t y) { return world_cell(x >> 8, y >> 8); }
inline int world_cell_x(uint32_t cell) { return (int)(cell & g_world.cell_mask); }
inline int world_cell_y(uint32_t cell) { return (int)(cell >> g_world.shift) & (int)g_world.cell_mask; }
inline uint32_t world_cell_step(uint32_t cell, int dx, int dy) {
    return world_cell(world_cell_x(cell) + dx, world_cell_y(cell) + dy);
}

// ---- coordinates ---------------------------------------------------------------------------------------
inline int32_t world_wrap(int32_t v) { return (int32_t)((uint32_t)v & g_world.unit_mask); }
// v wrapped onto the torus and centred on 0 (-units/2 .. units/2 - 1): the shortest signed difference when v is
// a difference of two coordinates. With 256 cells exactly the original's (int16_t)v.
inline int32_t world_signed(int32_t v) {
    const int32_t h = g_world.units >> 1;
    return (int32_t)((uint32_t)(v + h) & g_world.unit_mask) - h;
}
// Squared distance of a difference vector. With 256 cells the original's int32 sum (it wraps negative at
// exactly 128 cells on both axes); otherwise int64.
inline int64_t world_dist_sq(int32_t dx, int32_t dy) {
    if (g_world.size == 256) return (int32_t)((uint32_t)(dx * dx) + (uint32_t)(dy * dy));
    return (int64_t)dx * dx + (int64_t)dy * dy;
}
inline int64_t world_dist_sq3(int32_t dx, int32_t dy, int32_t dz) {
    if (g_world.size == 256) return (int32_t)((uint32_t)(dy * dy) + (uint32_t)(dx * dx) + (uint32_t)(dz * dz));
    return (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz;
}
// Integer square root of a world_dist_sq value (mc_isqrt for the original's 32-bit range).
uint32_t world_isqrt(int64_t v);

// High halves of the u16 position fields of GameState that are not Thing coordinates.
struct WorldHi {
    uint16_t start_x[8], start_y[8];        // GameState.start_pos[p]
    uint16_t log_x[8], log_y[8];            // PlayerRec.log[] (only one entry is ever written per player)
};
extern WorldHi g_world_hi;

// ---- player start positions (GameState.start_pos: {u16 x, u16 y, i16 z} per player) ---------------------
inline Pos world_start_pos(int p) {
    PosImage im;
    std::memcpy(&im, g_state->start_pos[p & 7], sizeof im);
    Pos r;
    r.x = (int32_t)im.x | (int32_t)g_world_hi.start_x[p & 7] << 16;
    r.y = (int32_t)im.y | (int32_t)g_world_hi.start_y[p & 7] << 16;
    r.z = im.z;
    return r;
}
inline void world_set_start_pos(int p, const Pos &pos) {
    const PosImage im{(uint16_t)(int32_t)pos.x, (uint16_t)(int32_t)pos.y, pos.z};
    std::memcpy(g_state->start_pos[p & 7], &im, sizeof im);
    g_world_hi.start_x[p & 7] = (uint16_t)((uint32_t)(int32_t)pos.x >> 16);
    g_world_hi.start_y[p & 7] = (uint16_t)((uint32_t)(int32_t)pos.y >> 16);
}

// ---- the camera log (PlayerRec.log[]: {u16 x, u16 y, ...}) -----------------------------------------------
inline int32_t world_log_x(int player, const PosLogEntry &e) { return (int32_t)e.x | (int32_t)g_world_hi.log_x[player & 7] << 16; }
inline int32_t world_log_y(int player, const PosLogEntry &e) { return (int32_t)e.y | (int32_t)g_world_hi.log_y[player & 7] << 16; }
inline void world_log_set_xy(int player, PosLogEntry *e, int32_t x, int32_t y) {
    e->x = (uint16_t)x;
    e->y = (uint16_t)y;
    g_world_hi.log_x[player & 7] = (uint16_t)((uint32_t)x >> 16);
    g_world_hi.log_y[player & 7] = (uint16_t)((uint32_t)y >> 16);
}
