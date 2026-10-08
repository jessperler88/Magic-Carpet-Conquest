// Port-only (Phase 4 round 10, task C): debug overlays and the Thing inspector. Nothing here exists in
// carpet.exe. Report: docs/analysis/port_inspect.md.
//
// Everything here is render-only: it reads GameState / the maps / the projection export of render.h and
// draws into the game frame during the 2D pass (after the HUD, inside main.cpp's draw_view_frame lambda,
// so the compositor puts it into the HUD layer). It never writes game state; the only state it keeps is
// its own (which overlays are on, the inspected slot, the damage-number history) - none of it is read by
// the simulation, saved, replayed or hashed.
//
// Per frame (main.cpp, see the report for the exact code):
//     debug_overlay_frame_begin(fb);            // opens the render.h capture when anything is on
//     draw_view_frame([&] { render_frame_draw(fb, local); ...; debug_overlay_draw(fb, local); ... });
//     debug_overlay_frame_end();
// Overlays are switched by name (`overlay <name> on|off` in the console); later rounds add their own with
// debug_overlay_register.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "render.h"

// ---- overlay registry ----------------------------------------------------------------------------
struct DebugOverlayCtx {
    FrameBuffer fb;          // the game frame the 2D pass draws into (320x200 / 640x480)
    int local;               // local player
    int input_mode;          // PlayerRec.input_mode of the local player (0/3 flight, 1 help, 2 book + map, 4 map)
};
using DebugOverlayFn = void (*)(const DebugOverlayCtx &ctx);
// Adds an overlay (off). `needs_view`: it uses the projection / anchors (the capture is opened for it).
// Returns its index; a second registration of the same name replaces the first.
int  debug_overlay_register(const char *name, const char *help, DebugOverlayFn fn, bool needs_view);
int  debug_overlay_count();
const char *debug_overlay_name(int i);
const char *debug_overlay_help(int i);
bool debug_overlay_on(int i);
int  debug_overlay_find(const char *name);               // -1 = unknown
// "all" / "none" switch every overlay. False = unknown name.
bool debug_overlay_set(const char *name, bool on);
bool debug_overlay_toggle(const char *name);
bool debug_overlay_is_on(const char *name);
bool debug_overlay_any_on();                             // an overlay, the inspector or the cursor mode
// One line per overlay: "name on|off - help" (console `overlay` without arguments).
std::string debug_overlay_list();

// Built-in overlays (registered on first use of the registry):
//   grid       cell grid around the camera (projected cell corners, the picked cell highlighted)
//   labels     slot / class.type / state (+ AI mode of wizards) at every drawn Thing's anchor
//   anchors    the projected anchor boxes of the drawn Things (what picking uses)
//   occupancy  cell-list lengths (g_cell_things chains) on the radar: flight radar and the map screen
//   damage     recent health losses as rising numbers at the Thing (render-side health history; see the report)
//   net        network sync status (net_sync_stats), the mode block, the state checksum

// ---- Thing inspector ------------------------------------------------------------------------------
void debug_inspect_set(int slot);                         // -1 = off (also clears the follow mode)
int  debug_inspect_slot();                                // -1 = none
uint32_t debug_inspect_generation();                      // thing_slot_generation at the time it was picked
bool debug_inspect_alive();                               // the inspected slot still holds the Thing picked
void debug_inspect_set_cell(int cell_x, int cell_y);      // the ground pick shown in the panel (-1 = none)
bool debug_inspect_cell(int *cell_x, int *cell_y);
void debug_inspect_set_follow(bool on);
// Orbit the follow camera: yaw / elevation steps (0x800 = a full turn), zoom steps (+ = closer). Render-only.
void debug_inspect_orbit(int dyaw, int delev, int zoom_steps);
bool debug_inspect_follow();
// Hide / show the inspector panel; the inspected Thing, the cell pick and the follow camera stay as they are.
void debug_inspect_set_panel_hidden(bool hidden);
bool debug_inspect_panel_hidden();
// The camera D's debug camera uses in follow mode: behind and above the inspected Thing, looking along
// its yaw. False when nothing is inspected / the slot was reused. Pure function of GameState.
bool debug_inspect_follow_camera(Camera *cam);
// The panel's text for `slot` (also printed by the console's `inspect`).
void debug_inspect_lines(int slot, std::vector<std::string> *out);

// ---- pointer (cursor mode, mcport) ----------------------------------------------------------------
// The pointer in view space (render.h) while a cursor tool is active: the hovered Thing is boxed.
void debug_overlay_set_pointer(bool active, int view_x, int view_y);
bool debug_overlay_pointer_active();
int  debug_overlay_hover();                               // slot under the pointer in the last frame, -1 none
// Pick at a view pixel with the last frame's anchors / the height field: the Thing (slot or -1) and the
// ground cell (false when the ray hits nothing).
int  debug_overlay_pick_thing(int view_x, int view_y);
bool debug_overlay_pick_cell(int view_x, int view_y, int *cell_x, int *cell_y);

// ---- per frame ------------------------------------------------------------------------------------
void debug_overlay_frame_begin(const FrameBuffer &fb);    // before the 2D pass (no-op when nothing is on)
void debug_overlay_frame_end();
void debug_overlay_draw(const FrameBuffer &fb, int local_player);   // inside the 2D pass, after the HUD
// Forget the damage history and the inspection (level start / load).
void debug_overlay_reset();

// ---- helpers (tests) ------------------------------------------------------------------------------
const char *debug_class_name(int cls);                    // "creature", "effect", ... ("?" unknown)
const char *debug_ai_mode_name(int mode);                 // P.ai_mode (ai_wizard.h)
