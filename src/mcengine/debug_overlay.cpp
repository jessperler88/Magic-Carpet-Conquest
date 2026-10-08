// Port-only (Phase 4 round 10, task C): debug overlays and the Thing inspector (debug_overlay.h).
// Render-only: reads GameState, the maps and render.h's projection export; draws into the game frame.
// Report: docs/analysis/port_inspect.md.
#include "debug_overlay.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "mode.h"
#include "net.h"
#include "settings.h"
#include "terrain.h"
#include "thing.h"
#include "world.h"
#include "ui_draw.h"
#include "level.h"
#include "gen/dispatch_tables.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------------------------
// state (all of it the overlay's own; nothing here is game state)
// ---------------------------------------------------------------------------------------------

struct Overlay {
    std::string name, help;
    DebugOverlayFn fn = nullptr;
    bool needs_view = false;
    bool on = false;
};
std::vector<Overlay> s_overlays;
bool s_builtins = false;

int s_inspect = -1;
uint32_t s_inspect_gen = 0;
bool s_follow = false;
bool s_panel_hidden = false;            // the inspector panel is not drawn (inspection / follow go on)
int s_cell_x = -1, s_cell_y = -1;

bool s_pointer = false;
int s_ptr_x = 0, s_ptr_y = 0;
int s_hover = -1;
bool s_frame_open = false;

// damage history (render side): health per slot at the last tick seen
struct HealthSeen { int32_t health; uint32_t gen; uint8_t cls; bool valid; };
std::vector<HealthSeen> s_health;
struct DamageEvent { uint16_t slot; int32_t amount; int32_t x, y, z; uint32_t tick; };
std::vector<DamageEvent> s_events;
uint32_t s_last_tick = 0;
bool s_have_tick = false;
constexpr uint32_t kDamageShowTicks = 40;
constexpr size_t kDamageMaxEvents = 96;

void register_builtins();
void ensure_builtins() { if (!s_builtins) register_builtins(); }

// ---------------------------------------------------------------------------------------------
// drawing helpers (game-frame pixels / 640-space text)
// ---------------------------------------------------------------------------------------------

// The view's rectangle in game-frame pixels (lines are clipped to it so they stay off the HUD frame).
struct Rect { int x0, y0, x1, y1; };
Rect view_frame_rect(const FrameBuffer &fb) {
    const RenderViewInfo &v = render_view_info();
    Rect r{0, 0, fb.width, fb.height};
    if (v.frame_known) {
        r.x0 = std::max(0, (int)std::floor(v.frame_x0));
        r.y0 = std::max(0, (int)std::floor(v.frame_y0));
        r.x1 = std::min(fb.width, (int)std::ceil(v.frame_x0 + v.view_w * v.frame_sx));
        r.y1 = std::min(fb.height, (int)std::ceil(v.frame_y0 + v.view_h * v.frame_sy));
    }
    return r;
}

void put(const FrameBuffer &fb, const Rect &clip, int x, int y, uint8_t c) {
    if (x < clip.x0 || y < clip.y0 || x >= clip.x1 || y >= clip.y1) return;
    fb.pixels[(size_t)y * fb.width + x] = c;
}

// Bresenham in game-frame pixels, clipped (lines far outside are rejected first).
void line(const FrameBuffer &fb, const Rect &clip, double fx0, double fy0, double fx1, double fy1, uint8_t c) {
    const double lim = 4.0 * (fb.width + fb.height);
    if (std::fabs(fx0) > lim || std::fabs(fy0) > lim || std::fabs(fx1) > lim || std::fabs(fy1) > lim) return;
    if ((fx0 < clip.x0 && fx1 < clip.x0) || (fy0 < clip.y0 && fy1 < clip.y0) ||
        (fx0 >= clip.x1 && fx1 >= clip.x1) || (fy0 >= clip.y1 && fy1 >= clip.y1)) return;
    int x0 = (int)std::floor(fx0), y0 = (int)std::floor(fy0), x1 = (int)std::floor(fx1), y1 = (int)std::floor(fy1);
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int n = 0; n < 8192; n++) {
        put(fb, clip, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void box(const FrameBuffer &fb, const Rect &clip, double x0, double y0, double x1, double y1, uint8_t c) {
    line(fb, clip, x0, y0, x1, y0, c);
    line(fb, clip, x1, y0, x1, y1, c);
    line(fb, clip, x1, y1, x0, y1, c);
    line(fb, clip, x0, y1, x0, y0, c);
}

// World point -> game-frame pixel (false when behind the camera / no mapping).
bool world_to_frame(int32_t x, int32_t y, int32_t z, double *fx, double *fy) {
    int sx, sy, depth;
    if (!render_project_world(x, y, z, &sx, &sy, &depth)) return false;
    return render_view_to_frame(sx + 0.5, sy + 0.5, fx, fy);
}

// The anchor's box in game-frame pixels.
bool anchor_frame_box(const RenderAnchor &a, double *x0, double *y0, double *x1, double *y1) {
    if (!render_view_to_frame(a.sx - a.w * 0.5, (double)a.sy - a.h, x0, y0)) return false;
    return render_view_to_frame(a.sx + a.w * 0.5, (double)a.sy, x1, y1);
}

int text_w(const char *s) { return ui_text_width(s); }
int line_h() { const int h = ui_font_line_height(); return h > 0 ? h : 10; }

// 640-space text with a dark box behind it.
void text_boxed(const char *s, int x, int y, uint8_t colour) {
    const int w = text_w(s) + 4, h = line_h() + 1;
    ui_shade_rect(x - 2, y - 1, w, h, 0x30);
    ui_draw_text(s, x, y, colour);
}

int virtual_h(const FrameBuffer &fb) { return fb.width <= 320 ? 400 : 480; }

bool frame_to_640(const FrameBuffer &fb, double fx, double fy, int *hx, int *hy) {
    *hx = (int)std::floor(fx * 640.0 / fb.width);
    *hy = (int)std::floor(fy * virtual_h(fb) / fb.height);
    return true;
}

std::string fmt(const char *f, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// "creature_bee_s3_update_..." -> "creature_bee_s3_update" (the Table A handler name of the state).
std::string state_name(int cls, int state) {
    if (cls < 0 || cls >= 14) return "";
    const DispatchClass &dc = g_dispatch_classes[cls];
    if (!dc.a || state < 0 || state >= dc.a_count) return "";
    std::string n = dc.a[state].name ? dc.a[state].name : "";
    const size_t u = n.rfind('_');
    if (u != std::string::npos && n.size() - u == 6) n.resize(u);   // address suffix
    return n;
}

const char *model_name(int cls, int type) {
    switch (cls) {
    case 3: return type == 0 ? "flyer (human)" : type == 1 ? "wizard (computer)" : "";
    case 2: case 5: case 10: case 12: return mc_model_name(cls, type);
    default: return "";
    }
}

int player_of_block(uint32_t off) {
    if (off == 0) return -1;
    const uint32_t base = player_block_offset(0);
    if (off < base) return -1;
    const uint32_t d = off - base;
    if (d % sizeof(PlayerRec) != 0) return -1;
    const int p = (int)(d / sizeof(PlayerRec));
    return p < 8 ? p : -1;
}

// ---------------------------------------------------------------------------------------------
// built-in overlays
// ---------------------------------------------------------------------------------------------

void draw_grid(const DebugOverlayCtx &c) {
    const RenderViewInfo &v = render_view_info();
    if (v.kind == RENDER_VIEW_NONE || !v.frame_known) return;
    const Rect clip = view_frame_rect(c.fb);
    const int R = 10;
    // round 11: cells and corners wrap at the world size (world.h); render_project_world takes the nearest
    // copy on the torus, so the grid is continuous across x = N*256 -> 0
    const int ccx = world_cx(v.cam_x), ccy = world_cx(v.cam_y);
    const int N = 2 * R + 2;
    std::vector<double> fx((size_t)N * N), fy((size_t)N * N);
    std::vector<uint8_t> ok((size_t)N * N);
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            const int cx = world_wrap_cell(ccx + i - R), cy = world_wrap_cell(ccy + j - R);
            const int32_t wx = world_wrap((ccx + i - R) * 256), wy = world_wrap((ccy + j - R) * 256);
            const int32_t h = (int32_t)g_map_height[world_cell(cx, cy)] * 0x20;
            const size_t k = (size_t)j * N + i;
            ok[k] = world_to_frame(wx, wy, h, &fx[k], &fy[k]) ? 1 : 0;
        }
    const uint8_t col = ui_colour(12, 12, 3), col_cam = ui_colour(15, 6, 0);
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            const size_t k = (size_t)j * N + i;
            if (!ok[k]) continue;
            if (i + 1 < N && ok[k + 1]) line(c.fb, clip, fx[k], fy[k], fx[k + 1], fy[k + 1], col);
            if (j + 1 < N && ok[k + N]) line(c.fb, clip, fx[k], fy[k], fx[k + N], fy[k + N], col);
        }
    // the camera's cell and the picked cell
    auto outline = [&](int cx, int cy, uint8_t colour) {
        double px[4], py[4];
        const int dxs[4] = {0, 1, 1, 0}, dys[4] = {0, 0, 1, 1};
        for (int q = 0; q < 4; q++) {
            const int x = world_wrap_cell(cx + dxs[q]), y = world_wrap_cell(cy + dys[q]);
            const int32_t h = (int32_t)g_map_height[world_cell(x, y)] * 0x20;
            if (!world_to_frame(x * 256, y * 256, h, &px[q], &py[q])) return;
        }
        for (int q = 0; q < 4; q++) line(c.fb, clip, px[q], py[q], px[(q + 1) & 3], py[(q + 1) & 3], colour);
    };
    outline(ccx, ccy, col_cam);
    if (s_cell_x >= 0) outline(s_cell_x, s_cell_y, ui_col_red());
}

void draw_anchors(const DebugOverlayCtx &c) {
    int n = 0;
    const RenderAnchor *a = render_anchors(&n);
    const Rect clip = view_frame_rect(c.fb);
    const uint8_t col = ui_colour(0, 12, 15);
    for (int i = 0; i < n; i++) {
        double x0, y0, x1, y1;
        if (!anchor_frame_box(a[i], &x0, &y0, &x1, &y1)) continue;
        box(c.fb, clip, x0, y0, x1, y1, col);
    }
}

void draw_labels(const DebugOverlayCtx &c) {
    int n = 0;
    const RenderAnchor *a = render_anchors(&n);
    std::vector<int> order((size_t)n);
    for (int i = 0; i < n; i++) order[(size_t)i] = i;
    std::sort(order.begin(), order.end(), [&](int p, int q) { return a[p].depth < a[q].depth; });
    const size_t max_labels = c.fb.width <= 320 ? 16 : 48;   // the nearest ones
    if (order.size() > max_labels) order.resize(max_labels);
    ui_set_font(1);
    for (int i : order) {
        const RenderAnchor &an = a[i];
        if ((int)an.slot >= thing_pool_slots()) continue;
        const Thing *t = thing_at(an.slot);
        if (t->cls == 0) continue;
        std::string s = fmt("%u %d.%d s%d", (unsigned)an.slot, t->cls, t->type, t->state);
        if (t->cls == 3) {
            const PlayerBlock *P = reinterpret_cast<const PlayerBlock *>(thing_player_block(t));
            s += fmt(" ai%d", P->ai_mode);
        }
        double fx, fy;
        if (!render_view_to_frame(an.sx + 0.5, (double)an.sy - an.h, &fx, &fy)) continue;
        int hx, hy;
        frame_to_640(c.fb, fx, fy, &hx, &hy);
        const int w = text_w(s.c_str());
        hy -= line_h() + 2;
        if (hx - w / 2 < 0 || hx + w / 2 >= 640 || hy < 0 || hy >= virtual_h(c.fb) - line_h()) continue;
        text_boxed(s.c_str(), hx - w / 2, hy, (int)an.slot == s_inspect ? ui_col_red() : ui_col_white());
    }
}

// Radar placement as hud.cpp draws it (flight radar / map screen), in game-frame pixels.
struct RadarGeo { int x, y, w, h, cam_x, cam_y, yaw, scale; bool round; };
bool radar_geometry(const DebugOverlayCtx &c, RadarGeo *g) {
    const PlayerRec &rec = g_state->players[c.local & 7];
    const PosLogEntry &e = rec.log[rec.view_entry % 32];
    const bool lo = g_video_mode_flags == 1;
    g->cam_x = world_log_x(c.local & 7, e); g->cam_y = world_log_y(c.local & 7, e); g->yaw = (int16_t)e.yaw;
    g->x = 0; g->y = 0;
    if (c.input_mode == 0 || c.input_mode == 3) {
        if (!g_state->opt_hud_a) return false;
        const Thing *T = thing_at(thing_wrap(rec.thing));
        if (T->health < 0 || (g_cfg->flags & 4)) return false;
        g->w = 0x80;
        g->h = (g_settings.radar_round && lo) ? 0x6c : 0x80;
        g->scale = 0x100 * std::clamp(g_settings.radar_zoom_pct, 50, 200) / 100;
        g->round = true;
    } else if (c.input_mode == 2 || c.input_mode == 4) {
        g->w = 0x17e;
        g->h = lo ? 0x17a : 0x19e;
        g->scale = 0xaa;
        g->round = false;
    } else {
        return false;
    }
    if (c.fb.width <= 320) { g->w >>= 1; g->h >>= 1; g->scale *= 2; }
    return g->w > 0 && g->h > 0;
}

void draw_occupancy(const DebugOverlayCtx &c) {
    RadarGeo g;
    if (!radar_geometry(c, &g)) return;
    const unsigned yaw = (unsigned)g.yaw & 0x7ff;
    const double s = (double)(int32_t)(((int64_t)mc_sin(yaw) * g.scale) >> 16);
    const double co = (double)(int32_t)(((int64_t)mc_cos(yaw) * g.scale) >> 16);
    const double A = co * g.h / g.w, B = s * g.h / g.w;
    const double det = A * co + s * B;
    if (std::fabs(det) < 1e-9) return;
    const Rect clip{g.x, g.y, std::min(c.fb.width, g.x + g.w), std::min(c.fb.height, g.y + g.h)};
    const int dot = (c.input_mode == 2 || c.input_mode == 4) && c.fb.width > 320 ? 2 : 1;
    const uint8_t cols[5] = {ui_colour(15, 15, 15), ui_colour(0, 15, 0), ui_colour(15, 15, 0), ui_colour(15, 4, 0), ui_colour(15, 0, 15)};
    const int slots = thing_pool_slots();
    for (uint32_t cell = 0; cell < g_world.cells; cell++) {
        unsigned idx = g_cell_things[cell];
        if (idx == 0) continue;
        int len = 0;
        for (int budget = 64; idx != 0 && idx < (unsigned)slots && budget > 0; budget--) { len++; idx = thing_at(idx)->cell_next; }
        if (len == 0) continue;
        const int32_t wx = (world_cell_x(cell) << 8) + 0x80, wy = (world_cell_y(cell) << 8) + 0x80;
        const double du = world_signed(wx - g.cam_x), dv = world_signed(wy - g.cam_y);
        const double I = g.w / 2.0 + (co * du + s * dv) / det, R = g.h / 2.0 + (-B * du + A * dv) / det;
        if (g.round) {
            const double ex = (I - g.w / 2.0) / (g.w / 2.0), ey = (R - g.h / 2.0) / (g.h / 2.0);
            if (ex * ex + ey * ey > 1.0) continue;
        }
        const uint8_t col = cols[len >= 9 ? 4 : len >= 5 ? 3 : len >= 3 ? 2 : len - 1];
        const int px = g.x + (int)std::floor(I), py = g.y + (int)std::floor(R);
        for (int yy = 0; yy < dot; yy++)
            for (int xx = 0; xx < dot; xx++) put(c.fb, clip, px + xx, py + yy, col);
    }
    if (c.input_mode == 2 || c.input_mode == 4) {
        ui_set_font(1);
        text_boxed("cell list: 1 white 2 green 3-4 yellow 5-8 red 9+ magenta", 4, virtual_h(c.fb) - 2 * line_h() - 4, ui_col_white());
    }
}

// Health history: once per tick (the local player's tick counter), health drops of damageable Things
// become events. Render-side bookkeeping only.
void damage_track(int local) {
    const uint32_t tick = g_state->players[local & 7].tick;
    if (s_have_tick && tick == s_last_tick) return;
    const bool consecutive = s_have_tick && tick > s_last_tick && tick - s_last_tick <= 4;
    const int slots = thing_pool_slots();
    if ((int)s_health.size() != slots) { s_health.assign((size_t)slots, HealthSeen{0, 0, 0, false}); }
    for (int i = 1; i < slots; i++) {
        const Thing *t = thing_at((unsigned)i);
        HealthSeen &h = s_health[(size_t)i];
        const uint32_t gen = thing_slot_generation((unsigned)i);
        const bool tracked = t->cls == 3 || t->cls == 5 || (t->prop_flags & 3) != 0;
        if (!tracked || t->cls == 0) { h.valid = false; continue; }
        if (consecutive && h.valid && h.gen == gen && h.cls == t->cls && t->health < h.health) {
            if (s_events.size() >= kDamageMaxEvents) s_events.erase(s_events.begin());
            s_events.push_back(DamageEvent{(uint16_t)i, h.health - t->health, t->x, t->y, t->z, tick});
        }
        h.health = t->health; h.gen = gen; h.cls = t->cls; h.valid = true;
    }
    s_events.erase(std::remove_if(s_events.begin(), s_events.end(),
                                  [&](const DamageEvent &e) { return tick < e.tick || tick - e.tick > kDamageShowTicks; }),
                   s_events.end());
    s_last_tick = tick;
    s_have_tick = true;
}

void draw_damage(const DebugOverlayCtx &c) {
    const uint32_t tick = g_state->players[c.local & 7].tick;
    ui_set_font(1);
    for (const DamageEvent &e : s_events) {
        if (e.amount <= 0) continue;
        const uint32_t age = tick - e.tick;
        double fx, fy;
        const Thing *t = (int)e.slot < thing_pool_slots() ? thing_at(e.slot) : nullptr;
        const int32_t height = t && t->cls ? std::max<int32_t>(t->ext_h, 0x40) : 0x80;
        if (!world_to_frame(e.x, e.y, e.z + height + (int32_t)age * 6, &fx, &fy)) continue;
        int hx, hy;
        frame_to_640(c.fb, fx, fy, &hx, &hy);
        const std::string s = fmt("-%d", e.amount);
        const int w = text_w(s.c_str());
        if (hx - w / 2 < 0 || hx + w >= 640 || hy < 0 || hy >= virtual_h(c.fb) - line_h()) continue;
        ui_draw_text(s.c_str(), hx - w / 2 + 1, hy + 1, ui_col_black());
        ui_draw_text(s.c_str(), hx - w / 2, hy, age < 10 ? ui_colour(15, 15, 0) : ui_col_red());
    }
}

void draw_net(const DebugOverlayCtx &c) {
    std::vector<std::string> L;
    const bool network = (g_cfg->flags & 0x10) != 0;
    const NetSyncStats st = net_sync_stats();
    L.push_back(network ? "net: network game" : "net: local game");
    L.push_back(fmt("sync: %u exchanges, %u checks, %u mismatches", st.exchanges, st.checks, st.mismatches));
    if (st.first_mismatch >= 0) L.push_back(fmt("first mismatch at exchange %d", st.first_mismatch));
    L.push_back(fmt("last local checksum %08x", st.last_local));
    L.push_back(fmt("state checksum %08x  tick %u", net_state_checksum(), g_state->players[c.local & 7].tick));
    // (task E's NetChecksumParts: one line per differing part goes here once net.h has them)
    if (mode_active())
        L.push_back(fmt("mode %s seed %u tick %u", mode_name(g_mode.mode), (unsigned)g_mode.params.seed, (unsigned)g_mode.tick));
    else
        L.push_back("mode: original");
    L.push_back(fmt("things live %d / %d slots", thing_pool_live_count(), thing_pool_slots()));
    ui_set_font(1);
    const bool map = c.input_mode == 2 || c.input_mode == 4;
    const int x = map ? 0x184 : 4;                       // map screen: right of the radar, under the inspector
    int y = map ? virtual_h(c.fb) - (int)(L.size() + 1) * (line_h() + 1) : 0x86;
    for (const std::string &s : L) { text_boxed(s.c_str(), x, y, ui_col_white()); y += line_h() + 1; }
}

void register_builtins() {
    s_builtins = true;
    debug_overlay_register("grid", "cell grid around the camera (projected cell corners; the camera's and the picked cell outlined)", draw_grid, true);
    debug_overlay_register("labels", "slot class.type state (+ ai mode) at the drawn Things", draw_labels, true);
    debug_overlay_register("anchors", "the projected anchor boxes picking uses", draw_anchors, true);
    debug_overlay_register("occupancy", "cell-list lengths on the radar (flight radar, map screen)", draw_occupancy, false);
    debug_overlay_register("damage", "recent health losses as rising numbers", draw_damage, true);
    debug_overlay_register("net", "network sync status, mode block, state checksum", draw_net, false);
}

bool any_needs_view() {
    if (s_inspect >= 0 || s_pointer || s_cell_x >= 0) return true;
    for (const Overlay &o : s_overlays) if (o.on && o.needs_view) return true;
    return false;
}

// ---------------------------------------------------------------------------------------------
// inspector panel
// ---------------------------------------------------------------------------------------------

void draw_inspector(const DebugOverlayCtx &c) {
    if (s_panel_hidden) return;
    std::vector<std::string> L;
    if (s_inspect >= 0) debug_inspect_lines(s_inspect, &L);
    if (s_cell_x >= 0) {
        const uint32_t cell = world_cell(s_cell_x, s_cell_y);
        L.push_back(fmt("cell %d,%d type %d height %d light %d flags 0x%02x", s_cell_x, s_cell_y, g_map_type[cell],
                        g_map_height[cell], g_map_light[cell], g_map_flags[cell]));
        std::string things = "  things:";
        int n = 0;
        unsigned idx = g_cell_things[cell];
        for (int budget = 64; idx != 0 && idx < (unsigned)thing_pool_slots() && budget > 0; budget--, n++) {
            if (n < 10) things += fmt(" %u", idx);
            idx = thing_at(idx)->cell_next;
        }
        if (n > 10) things += " ...";
        things += fmt(" (%d)", n);
        L.push_back(things);
    }
    if (L.empty()) return;
    L.push_back("Backspace / click on nothing: close");
    if (c.fb.width <= 320) {
        // 320x200: the font is as large as in 640x480 relative to the 640-space - keep the essential lines
        static const char *const keep[] = {"Thing", "class", "state", "pos", "health", "owner", "player", "ai mode", "cell ", "  things", "Backspace"};
        std::vector<std::string> K;
        for (const std::string &s : L)
            for (const char *k : keep)
                if (s.rfind(k, 0) == 0) { K.push_back(s.size() > 44 ? s.substr(0, 44) : s); break; }
        L.swap(K);
    }
    ui_set_font(1);
    int w = 0;
    for (const std::string &s : L) w = std::max(w, text_w(s.c_str()));
    w += 8;
    const int lh = line_h() + 1;
    const int vh = virtual_h(c.fb);
    int y = (c.input_mode == 2 || c.input_mode == 4) ? 0xd0 : 0x34;   // map screen: below its view window
    int h = (int)L.size() * lh + 6;
    if (y + h > vh) h = vh - y;
    const int x = std::max(0, 640 - w - 4);
    ui_shade_rect(x, y, w, h, 0x34);
    vga_draw_box(x, y, w, h, ui_colour(8, 8, 8));
    int ty = y + 3;
    for (size_t i = 0; i < L.size() && ty + lh <= y + h; i++, ty += lh)
        ui_draw_text(L[i].c_str(), x + 4, ty, i == 0 ? ui_colour(15, 15, 0) : ui_col_white());
}

void draw_highlights(const DebugOverlayCtx &c) {
    int n = 0;
    const RenderAnchor *a = render_anchors(&n);
    const Rect clip = view_frame_rect(c.fb);
    for (int i = 0; i < n; i++) {
        const bool insp = (int)a[i].slot == s_inspect, hov = (int)a[i].slot == s_hover && s_pointer;
        if (!insp && !hov) continue;
        double x0, y0, x1, y1;
        if (!anchor_frame_box(a[i], &x0, &y0, &x1, &y1)) continue;
        box(c.fb, clip, x0 - 1, y0 - 1, x1 + 1, y1 + 1, insp ? ui_col_red() : ui_col_white());
    }
    if (s_cell_x >= 0 && !debug_overlay_is_on("grid")) {
        const RenderViewInfo &v = render_view_info();
        if (v.kind != RENDER_VIEW_NONE && v.frame_known) {
            double px[4], py[4];
            const int dxs[4] = {0, 1, 1, 0}, dys[4] = {0, 0, 1, 1};
            bool ok = true;
            for (int q = 0; q < 4 && ok; q++) {
                const int x = world_wrap_cell(s_cell_x + dxs[q]), y = world_wrap_cell(s_cell_y + dys[q]);
                ok = world_to_frame(x * 256, y * 256, (int32_t)g_map_height[world_cell(x, y)] * 0x20, &px[q], &py[q]);
            }
            if (ok) for (int q = 0; q < 4; q++) line(c.fb, clip, px[q], py[q], px[(q + 1) & 3], py[(q + 1) & 3], ui_col_red());
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------------------------

int debug_overlay_register(const char *name, const char *help, DebugOverlayFn fn, bool needs_view) {
    ensure_builtins();
    for (size_t i = 0; i < s_overlays.size(); i++)
        if (s_overlays[i].name == name) {
            s_overlays[i].help = help ? help : ""; s_overlays[i].fn = fn; s_overlays[i].needs_view = needs_view;
            return (int)i;
        }
    Overlay o;
    o.name = name; o.help = help ? help : ""; o.fn = fn; o.needs_view = needs_view;
    s_overlays.push_back(o);
    return (int)s_overlays.size() - 1;
}

int debug_overlay_count() { ensure_builtins(); return (int)s_overlays.size(); }
const char *debug_overlay_name(int i) { ensure_builtins(); return i >= 0 && i < (int)s_overlays.size() ? s_overlays[(size_t)i].name.c_str() : ""; }
const char *debug_overlay_help(int i) { ensure_builtins(); return i >= 0 && i < (int)s_overlays.size() ? s_overlays[(size_t)i].help.c_str() : ""; }
bool debug_overlay_on(int i) { ensure_builtins(); return i >= 0 && i < (int)s_overlays.size() && s_overlays[(size_t)i].on; }

int debug_overlay_find(const char *name) {
    ensure_builtins();
    for (size_t i = 0; i < s_overlays.size(); i++) if (s_overlays[i].name == name) return (int)i;
    return -1;
}

bool debug_overlay_set(const char *name, bool on) {
    ensure_builtins();
    if (!std::strcmp(name, "all") || !std::strcmp(name, "none")) {
        const bool v = !std::strcmp(name, "all") ? on : false;
        for (Overlay &o : s_overlays) o.on = v;
        return true;
    }
    const int i = debug_overlay_find(name);
    if (i < 0) return false;
    s_overlays[(size_t)i].on = on;
    return true;
}

bool debug_overlay_toggle(const char *name) {
    const int i = debug_overlay_find(name);
    if (i < 0) return false;
    s_overlays[(size_t)i].on = !s_overlays[(size_t)i].on;
    return true;
}

bool debug_overlay_is_on(const char *name) {
    const int i = debug_overlay_find(name);
    return i >= 0 && s_overlays[(size_t)i].on;
}

bool debug_overlay_any_on() {
    ensure_builtins();
    if (s_inspect >= 0 || s_pointer || s_cell_x >= 0) return true;
    for (const Overlay &o : s_overlays) if (o.on) return true;
    return false;
}

std::string debug_overlay_list() {
    ensure_builtins();
    std::string s;
    for (const Overlay &o : s_overlays) s += o.name + (o.on ? " on - " : " off - ") + o.help + "\n";
    return s;
}

// ---------------------------------------------------------------------------------------------
// inspector
// ---------------------------------------------------------------------------------------------

void debug_inspect_set(int slot) {
    if (slot <= 0 || !g_state || slot >= thing_pool_slots()) { s_inspect = -1; s_follow = false; return; }
    s_inspect = slot;
    s_inspect_gen = thing_slot_generation((unsigned)slot);
}
int debug_inspect_slot() { return s_inspect; }
void debug_inspect_set_panel_hidden(bool hidden) { s_panel_hidden = hidden; }
bool debug_inspect_panel_hidden() { return s_panel_hidden; }
uint32_t debug_inspect_generation() { return s_inspect_gen; }
bool debug_inspect_alive() {
    return s_inspect > 0 && s_inspect < thing_pool_slots() && thing_slot_generation((unsigned)s_inspect) == s_inspect_gen &&
           thing_at((unsigned)s_inspect)->cls != 0;
}
void debug_inspect_set_cell(int cx, int cy) {
    if (cx < 0 || cy < 0) { s_cell_x = s_cell_y = -1; return; }
    s_cell_x = world_wrap_cell(cx); s_cell_y = world_wrap_cell(cy);
}
bool debug_inspect_cell(int *cx, int *cy) {
    if (s_cell_x < 0) return false;
    *cx = s_cell_x; *cy = s_cell_y;
    return true;
}
// Orbit of the follow camera (render-only): absolute yaw around the Thing, elevation angle, distance.
static bool s_orb_init = false;
static int  s_orb_yaw = 0, s_orb_elev = 0x96, s_orb_dist = 0x360;
void debug_inspect_set_follow(bool on) { s_follow = on && s_inspect > 0; s_orb_init = false; }
void debug_inspect_orbit(int dyaw, int delev, int zoom_steps) {
    if (!s_follow) return;
    s_orb_yaw = (s_orb_yaw + dyaw) & 0x7ff;
    s_orb_elev = std::clamp(s_orb_elev + delev, 0x10, 0x170);          // ~6..65 degrees above the Thing
    for (; zoom_steps > 0; zoom_steps--) s_orb_dist = s_orb_dist * 7 / 8;
    for (; zoom_steps < 0; zoom_steps++) s_orb_dist = s_orb_dist * 8 / 7;
    s_orb_dist = std::clamp(s_orb_dist, 0x100, 0x3000);
}
bool debug_inspect_follow() { return s_follow; }

bool debug_inspect_follow_camera(Camera *cam) {
    if (!g_state || !debug_inspect_alive()) return false;
    const Thing *t = thing_at((unsigned)s_inspect);
    int32_t x = t->x, y = t->y, z = t->z;
    const RenderInterp &ri = g_render_interp;                // drawn between ticks: the interpolated position
    if (ri.active && ri.prev_pos && s_inspect < ri.prev_count) {
        const int16_t *p = ri.prev_pos[s_inspect];
        const int64_t a = ri.alpha;
        // prev_pos holds the low 16 bits (render_ext.cpp thing_pos_lerp): back from the current position by
        // the tick's 16-bit movement, in any world size
        const int32_t dx = (int16_t)(uint16_t)(t->x - (uint16_t)p[0]), dy = (int16_t)(uint16_t)(t->y - (uint16_t)p[1]);
        x = world_wrap(t->x - dx + (int32_t)(((int64_t)dx * a) >> 16));
        y = world_wrap(t->y - dy + (int32_t)(((int64_t)dy * a) >> 16));
        z = (int32_t)p[2] + (int32_t)(((int64_t)((int32_t)t->z - p[2]) * a) >> 16);
    }
    // Orbit (debug_inspect_orbit): starts behind the Thing; yaw 0 looks towards -y, 0x200 towards +x, so
    // forward = (sin, -cos). The camera sits `dist` away at the elevation angle.
    if (!s_orb_init) { s_orb_yaw = t->yaw & 0x7ff; s_orb_init = true; }
    const unsigned yaw = (unsigned)s_orb_yaw;
    const int32_t back = (int32_t)(((int64_t)s_orb_dist * mc_cos((unsigned)s_orb_elev)) >> 16);
    const int32_t up = (int32_t)(((int64_t)s_orb_dist * mc_sin((unsigned)s_orb_elev)) >> 16);
    cam->cam_x = world_wrap(x - ((mc_sin(yaw) * back) >> 16));
    cam->cam_y = world_wrap(y + ((mc_cos(yaw) * back) >> 16));
    const int ground = terrain_sample_height(cam->cam_x, cam->cam_y);
    cam->cam_z = std::max((int)z + up, ground + 0x100);
    cam->yaw = (int)yaw;
    // The projection cannot tilt: it shifts the horizon (horizon px = pitch * width / 256, render_landscape /
    // render_ext). A point dz below the camera at forward depth d projects focal * dz / d above the horizon
    // line, so this pitch keeps the Thing at the view centre; focal / width from the last drawn view.
    const RenderViewInfo &vi = render_view_info();
    double ratio = 1.0;
    if (vi.view_w > 0) {
        if (vi.focal > 0) ratio = vi.focal / vi.view_w;
        else if (vi.rcam.focal > 0) ratio = (double)vi.rcam.focal / vi.view_w;
    }
    const double pitch = ratio * 256.0 * (double)(cam->cam_z - z) / (double)std::max(back, 0x40);
    cam->pitch = (int)std::clamp(pitch, -512.0, 512.0);
    cam->roll = 0;
    cam->zoom = 0x80;
    return true;
}

const char *debug_class_name(int cls) {
    switch (cls) {
    case 0: return "free";
    case 1: return "class1";
    case 2: return "scenery";
    case 3: return "player";
    case 5: return "creature";
    case 7: return "weather";
    case 9: return "projectile";
    case 10: return "effect";
    case 11: return "switch";
    case 12: return "spell";
    default: return "?";
    }
}

const char *debug_ai_mode_name(int mode) {
    switch (mode) {
    case 0: return "choose goal";
    case 1: return "upgrade castle";
    case 3: return "fly to castle site";
    case 4: return "approach target";
    case 6: return "collect mana";
    case 7: return "attack castle";
    case 8: return "attack wizard";
    case 9: return "attack player";
    case 0xb: return "return home";
    case 0xc: return "idle";
    case 0xd: return "attack creature";
    case 2: case 5: case 0xa: return "none";
    default: return "?";
    }
}

void debug_inspect_lines(int slot, std::vector<std::string> *out) {
    out->clear();
    if (!g_state || slot <= 0 || slot >= thing_pool_slots()) { out->push_back(fmt("Thing %d: no such slot", slot)); return; }
    const Thing *t = thing_at((unsigned)slot);
    const uint32_t gen = thing_slot_generation((unsigned)slot);
    const bool reused = slot == s_inspect && gen != s_inspect_gen;
    out->push_back(fmt("Thing %d  gen %u%s%s%s", slot, gen, t->cls == 0 ? "  FREE" : "", reused ? "  (slot reused)" : "",
                       slot == s_inspect && s_follow ? "  [follow]" : ""));
    if (t->cls == 0) return;
    const char *mn = model_name(t->cls, t->type);
    out->push_back(fmt("class %d %s  type %d %s", t->cls, debug_class_name(t->cls), t->type, mn));
    out->push_back(fmt("state %d %s", t->state, state_name(t->cls, t->state).c_str()));
    out->push_back(fmt("flags %05x  prop %04x  sprite %d frame %d draw %d", t->flags, t->prop_flags, t->sprite, t->frame, t->draw_type));
    out->push_back(fmt("pos %d,%d z %d  cell %d,%d  ext %d,%d,%d", (int)t->x, (int)t->y, t->z, world_cx(t->x), world_cx(t->y), t->ext_x,
                       t->ext_y, t->ext_h));
    out->push_back(fmt("yaw %03x pitch %03x  speed %d cur %d base %d turn %d", t->yaw & 0x7ff, t->pitch & 0x7ff, t->speed,
                       t->speed_cur, t->speed_base, t->turn_rate));
    out->push_back(fmt("health %d / %d  mana %d total %d cost %d", t->health, t->max_health, t->mana, t->mana_total, t->mana_cost));
    out->push_back(fmt("owner %u parent %u child %u target %u (sig %u)", t->owner, t->parent, t->child, t->target, t->unk94));
    out->push_back(fmt("killer %u attacker %u caster %u mana_owner %u", t->killer, t->last_attacker, t->caster, t->mana_owner));
    out->push_back(fmt("timers a %u b %u  tick %u  aux %d  dur %d  cast %d  zvel %d", t->timer_a, t->timer_b, t->tick, t->aux,
                       t->duration, t->cast_ticks, t->z_vel));
    out->push_back(fmt("home %d,%d,%d", (int)t->home.x, (int)t->home.y, t->home.z));
    std::string dmg;
    for (int k = 0; k < 6; k++)
        if (t->damage_slots[k].amount) dmg += fmt(" [%d]%d@%u", k, t->damage_slots[k].amount, t->damage_slots[k].attacker);
    if (!dmg.empty()) out->push_back("pending damage" + dmg);
    if (const MoveDesc *d = mc_move_desc(t->desc))
        out->push_back(fmt("move desc %u: turn %u/%u height %d..%d sight %u fov %u think %u", t->desc, d->turn_min, d->turn_max,
                           d->clear_hi, d->clear_lo, d->sight_radius, d->fov, d->think_period));
    // the cell list the Thing is on
    {
        const uint32_t cell = world_cell_of(t->x, t->y);
        std::string s = fmt("cell list %d,%d:", world_cx(t->x), world_cx(t->y));
        int n = 0;
        bool found = false;
        unsigned idx = g_cell_things[cell];
        for (int budget = 64; idx != 0 && idx < (unsigned)thing_pool_slots() && budget > 0; budget--, n++) {
            if (n < 8) s += fmt(" %u", idx);
            found |= idx == (unsigned)slot;
            idx = thing_at(idx)->cell_next;
        }
        if (n > 8) s += " ...";
        s += fmt(" (%d)%s", n, (t->flags & 4) ? (found ? "" : " NOT ON IT") : " (not linked)");
        out->push_back(s);
    }
    const int p = player_of_block(t->player);
    if (p >= 0) {
        const PlayerRec &rec = g_state->players[p];
        const PlayerBlock &P = rec.blk;
        out->push_back(fmt("player %d%s%s  '%.16s'", p, rec.is_computer ? " computer" : " human", rec.thing == slot ? " (its wizard)" : "",
                           rec.name));
        if (t->cls == 3) {
            out->push_back(fmt("ai mode %d %s  castle %u lvl %u", P.ai_mode, debug_ai_mode_name(P.ai_mode), P.castle, P.castle_level));
            out->push_back(fmt("P mana %d  transit %d  regen %d  invuln %d  kills %d", P.mana, P.mana_in_transit, P.health_regen,
                               P.invuln_timer, P.kills));
            out->push_back(fmt("aggression %d accuracy %d reaction %d  speed %d", P.ai_aggression, P.ai_accuracy, P.ai_reaction,
                               P.target_speed));
            out->push_back(fmt("hands %d / %d  tether %u", P.slot_left, P.slot_right, P.tether_target));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// pointer / picks
// ---------------------------------------------------------------------------------------------

void debug_overlay_set_pointer(bool active, int vx, int vy) {
    s_pointer = active;
    s_ptr_x = vx; s_ptr_y = vy;
    if (!active) s_hover = -1;
}
bool debug_overlay_pointer_active() { return s_pointer; }
int debug_overlay_hover() { return s_hover; }
int debug_overlay_pick_thing(int vx, int vy) { return render_pick_thing(vx, vy, 12); }
bool debug_overlay_pick_cell(int vx, int vy, int *cx, int *cy) { return render_pick_ground(vx, vy, cx, cy); }

// ---------------------------------------------------------------------------------------------
// per frame
// ---------------------------------------------------------------------------------------------

void debug_overlay_frame_begin(const FrameBuffer &fb) {
    ensure_builtins();
    if (s_frame_open) debug_overlay_frame_end();
    if (!any_needs_view()) return;
    render_capture_begin(fb, true);
    s_frame_open = true;
}

void debug_overlay_frame_end() {
    if (!s_frame_open) return;
    render_capture_end();
    s_frame_open = false;
}

void debug_overlay_draw(const FrameBuffer &fb, int local) {
    ensure_builtins();
    if (!g_state || !fb.pixels || !debug_overlay_any_on()) return;
    if (s_inspect >= 0 && (s_inspect >= thing_pool_slots())) s_inspect = -1;
    DebugOverlayCtx c{fb, local & 7, g_state->players[local & 7].input_mode};
    ui_set_target(fb);
    if (debug_overlay_is_on("damage")) damage_track(c.local);
    s_hover = s_pointer ? render_pick_thing(s_ptr_x, s_ptr_y, 12) : -1;
    for (const Overlay &o : s_overlays)
        if (o.on && o.fn) o.fn(c);
    draw_highlights(c);
    draw_inspector(c);
    ui_set_font(1);
}

void debug_overlay_reset() {
    s_inspect = -1; s_follow = false;
    s_cell_x = s_cell_y = -1;
    s_events.clear(); s_health.clear();
    s_have_tick = false;
    s_hover = -1;
}
