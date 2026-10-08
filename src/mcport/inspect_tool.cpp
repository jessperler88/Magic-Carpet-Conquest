// Port-only (Phase 4 round 10, task C): cursor mode, mouse picking and the tool keys of the Thing inspector /
// overlays (inspect_tool.h). Render-side only: picks read the last drawn frame's projection, nothing here
// writes game state. Report: docs/analysis/port_inspect.md.
#include "inspect_tool.h"
#include "platform.h"
#include "config.h"
#include "compose.h"
#include "debug_overlay.h"
#include "render.h"
#include "thing.h"
#include "mc_globals.h"
#include <SDL.h>
#include <cstdlib>

namespace {
bool s_cursor = false;
bool s_refresh = false;                 // read the OS pointer once after the cursor mode started
bool s_prev_l = false, s_prev_r = false, s_prev_m = false;
int s_cell_x = -1, s_cell_y = -1;
bool s_rdrag = false;                   // cursor mode: right button held - orbit when it moves
bool s_rmoved = false;
int s_rlast_x = 0, s_rlast_y = 0, s_rstart_x = 0, s_rstart_y = 0;
constexpr int kOrbitPerPx = 8;          // orbit angle units (0x800 = a turn) per game-frame pixel of motion
bool s_have_ptr = false;                // the pointer in game-frame pixels (cursor mode), for drawing it
int s_ptr_fx = 0, s_ptr_fy = 0;

void inspector_close() {
    debug_inspect_set(-1); debug_inspect_set_cell(-1, -1); s_cell_x = s_cell_y = -1;
    debug_inspect_set_panel_hidden(false);
}

// The pointer in view space (render.h) of the last drawn frame.
bool pointer_view(const mc::Platform &plat, const mc::Input &in, int *vx, int *vy) {
    if (plat.composed()) {
        const ComposeOutput &o = compose_output();
        if (o.has_view && render_view_info().composed) return compose_display_to_view(o, in.display_x, in.display_y, vx, vy);
    }
    int fx, fy;
    plat.display_to_frame(in.display_x, in.display_y, &fx, &fy);
    double x, y;
    if (!render_frame_to_view(fx + 0.5, fy + 0.5, &x, &y)) return false;
    const RenderViewInfo &v = render_view_info();
    *vx = (int)x; *vy = (int)y;
    return x >= 0 && y >= 0 && *vx < v.view_w && *vy < v.view_h;
}

void step_slot(int dir) {
    if (!g_state) return;
    const int n = thing_pool_slots();
    int s = debug_inspect_slot();
    if (s < 0) s = 0;
    for (int k = 0; k < n; k++) {
        s += dir;
        if (s <= 0) s = n - 1;
        if (s >= n) s = 1;
        if (thing_at((unsigned)s)->cls != 0) { debug_inspect_set(s); return; }
    }
}
} // namespace

bool inspect_tool_cursor_mode() { return s_cursor; }

void inspect_tool_set_cursor_mode(mc::Platform &plat, bool on) {
    if (on == s_cursor) return;
    s_cursor = on;
    s_refresh = on;
    s_have_ptr = false;
    plat.set_relative_mouse(!on);                 // a level steers with the relative mouse
    debug_overlay_set_pointer(false, 0, 0);
}

bool inspect_tool_key(mc::Platform &plat, int sc, uint8_t mods, bool level) {
    if (!level) return false;
    static const char *const k_overlay_keys[6] = {"grid", "labels", "anchors", "occupancy", "damage", "net"};
    if (sc == SDL_SCANCODE_HOME) {
        if (mods == 0) inspect_tool_set_cursor_mode(plat, !s_cursor);
        else if (mods == KEYMOD_SHIFT) inspector_close();
        else if (mods == KEYMOD_CTRL) debug_inspect_set_follow(!debug_inspect_follow());
        else return false;
        return true;
    }
    // Shift+Backspace / keypad /: hide or show the panel, keep inspecting (and following / orbiting)
    if ((sc == SDL_SCANCODE_BACKSPACE && mods == KEYMOD_SHIFT) || (sc == SDL_SCANCODE_KP_DIVIDE && mods == 0)) {
        if (debug_inspect_slot() < 0 && s_cell_x < 0) return false;
        if (g_state && g_state->players[g_state->local_player & 7].input_mode == 3) return false;
        debug_inspect_set_panel_hidden(!debug_inspect_panel_hidden());
        return true;
    }
    if (mods != 0) return false;
    // Backspace closes the panel (the game reads Backspace only while typing a chat line, input mode 3)
    if (sc == SDL_SCANCODE_BACKSPACE) {
        if (debug_inspect_slot() < 0 && s_cell_x < 0) return false;
        if (g_state && g_state->players[g_state->local_player & 7].input_mode == 3) return false;
        inspector_close();
        return true;
    }
    switch (sc) {
    case SDL_SCANCODE_KP_0: debug_overlay_set("none", false); return true;
    case SDL_SCANCODE_KP_7: step_slot(-1); return true;
    case SDL_SCANCODE_KP_9: step_slot(1); return true;
    case SDL_SCANCODE_KP_8: debug_inspect_set_follow(!debug_inspect_follow()); return true;
    case SDL_SCANCODE_KP_PERIOD: inspector_close(); return true;
    default: break;
    }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_6) {
        debug_overlay_toggle(k_overlay_keys[sc - SDL_SCANCODE_KP_1]);
        return true;
    }
    return false;
}

bool inspect_tool_mouse(mc::Platform &plat, mc::Input &in, bool level) {
    if (!level) {
        if (s_cursor) { s_cursor = false; debug_overlay_set_pointer(false, 0, 0); }
        s_prev_l = in.mouse_l; s_prev_r = in.mouse_r; s_prev_m = in.mouse_m;
        return false;
    }
    if (in.mouse_m && !s_prev_m) inspect_tool_set_cursor_mode(plat, !s_cursor);
    s_prev_m = in.mouse_m;
    if (!s_cursor) {
        s_prev_l = in.mouse_l; s_prev_r = in.mouse_r;
        if (!debug_inspect_follow()) return false;
        // Following a Thing: the (relative) mouse orbits the camera around it, the wheel zooms; the wizard's
        // steering is frozen meanwhile (Ctrl+Home / keypad 8 / Backspace end the follow).
        debug_inspect_orbit(in.mouse_dx * kOrbitPerPx, in.mouse_dy * kOrbitPerPx, in.wheel);
        return true;
    }
    if (s_refresh) { plat.refresh_pointer(in); s_refresh = false; }
    plat.display_to_frame(in.display_x, in.display_y, &s_ptr_fx, &s_ptr_fy);
    s_have_ptr = true;
    int vx = 0, vy = 0;
    const bool in_view = pointer_view(plat, in, &vx, &vy);
    debug_overlay_set_pointer(in_view, vx, vy);
    const bool click_l = in.mouse_l && !s_prev_l;
    bool click_r = false;
    // Right button: held and moved = orbit the follow camera; released without moving = pick the cell.
    if (in.mouse_r && !s_prev_r) {
        s_rdrag = true; s_rmoved = false;
        s_rlast_x = s_rstart_x = in.display_x; s_rlast_y = s_rstart_y = in.display_y;
    } else if (s_rdrag && in.mouse_r) {
        if (std::abs(in.display_x - s_rstart_x) + std::abs(in.display_y - s_rstart_y) > 4) s_rmoved = true;
        if (s_rmoved && debug_inspect_follow()) {
            int fx0, fy0, fx1, fy1;             // display pixels -> game-frame pixels, as the relative mouse
            plat.display_to_frame(s_rlast_x, s_rlast_y, &fx0, &fy0);
            plat.display_to_frame(in.display_x, in.display_y, &fx1, &fy1);
            debug_inspect_orbit((fx1 - fx0) * kOrbitPerPx, (fy1 - fy0) * kOrbitPerPx, 0);
        }
        s_rlast_x = in.display_x; s_rlast_y = in.display_y;
    } else if (s_rdrag && !in.mouse_r) {
        s_rdrag = false;
        click_r = !s_rmoved;
    }
    if (in.wheel && debug_inspect_follow()) debug_inspect_orbit(0, 0, in.wheel);
    s_prev_l = in.mouse_l; s_prev_r = in.mouse_r;
    if (in_view && click_l) {
        // left: the Thing under the pointer (and its ground cell); nothing there = close the panel
        const int slot = debug_overlay_pick_thing(vx, vy);
        int cx, cy;
        if (slot <= 0) inspector_close();
        else {
            debug_inspect_set(slot);
            if (debug_overlay_pick_cell(vx, vy, &cx, &cy)) { s_cell_x = cx; s_cell_y = cy; debug_inspect_set_cell(cx, cy); }
        }
    } else if (in_view && click_r) {
        int cx, cy;                                   // right: the ground cell only
        if (debug_overlay_pick_cell(vx, vy, &cx, &cy)) { s_cell_x = cx; s_cell_y = cy; debug_inspect_set_cell(cx, cy); }
    }
    return true;
}

bool inspect_tool_pointer_frame(int *fx, int *fy) {
    if (!s_cursor || !s_have_ptr) return false;
    *fx = s_ptr_fx; *fy = s_ptr_fy;
    return true;
}

bool inspect_tool_cursor_cell(int *cx, int *cy) {
    if (s_cell_x < 0) return false;
    *cx = s_cell_x; *cy = s_cell_y;
    return true;
}

bool inspect_tool_follow_camera(Camera *cam) {
    return debug_inspect_follow() && debug_inspect_follow_camera(cam);
}
