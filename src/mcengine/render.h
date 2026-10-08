// Landscape renderer of carpet.exe (render_landscape_29050 and helpers). Owner: render_landscape.cpp.
#pragma once
#include <cstdint>

// Camera as render_frame_1fab0 passes it (from the player's position-history entry):
// cam_x / cam_y are world coordinates (cell = value >> 8; the original's 16 bits, round 11: the renderers wrap
// them at g_world.units, so a 16-bit or sign-extended value means the same in the 256 world), yaw/pitch/roll are angle units
// (0..0x7ff), cam_z is the world height (+0x80 above the player), zoom is the focal scale (0x100 = 1.0).
// yaw 0 looks towards decreasing y, 0x200 towards increasing x (quadrant q = ((yaw+0x100)>>9)&3).
// pitch is a horizon offset in 1/256 of the view width (horizon = height/2 + pitch*width/256), roll
// rotates the projected screen (positive = counter-clockwise image rotation).
struct Camera {
    int cam_x, cam_y, yaw, cam_z, pitch, roll, zoom;
};

// Frame-buffer description for the view (DAT_0012ed74 / ed70 / ed78): whole-screen 8-bit buffer.
struct FrameBuffer {
    uint8_t *pixels;
    int width, height;   // 320x200 or 640x480
};

// render_set_view_window_2f3c0(size): size = g_state->view_size (0x28 = full screen); sets the
// render target to the reduced window inside `fb`.
void render_set_view_window(const FrameBuffer &fb, int size);

// render_landscape_29050: draws sky + terrain (+ things through the per-cell hook) into the current
// render target. Requires the maps, tables, textures and g_uv_table to be loaded.
void render_landscape(const Camera &cam);

// render_view_2f6e0, mono path only (no anaglyph / SIRDS / interlaced): slope smoothing state,
// then render_landscape, then the optional 2x2 smoothing / motion-blur post filters.
void render_view(const FrameBuffer &fb, const Camera &cam);
// The renderer's slope low-pass (static state carried between frames; render_reference_test).
void render_get_slope_state(int32_t *x, int32_t *y);
void render_set_slope_state(int32_t x, int32_t y);

// ---- round 6 (task D, render references; implemented in ui_draw.cpp / hud.cpp) ----
// mouse_cursor_set_sprite_5ba5c + mouse_cursor_draw_5b35c: the original's software pointer is drawn
// into the back buffer when a frame is presented (after the frame, before the copy to VRAM) and stays
// there for the next frame (visible through the motion-blur blend). `s` = the pointer sprite
// (ui_sprite(g_ui_pointers, 1) in the book, entry 0 = none otherwise), mouse position in 640-space.
struct UiSprite;
void ui_draw_mouse_pointer(const FrameBuffer &fb, const UiSprite *s, int mouse_x, int mouse_y);
// Port-only: draw the flight HUD during movie playback too (what tools/reference/fb/patch_carpet.py
// --hud does to the original at 0x1fc85); the credits roll still sees Config bit 2.
extern bool g_hud_force_flight_hud;

// ---- Phase 3 seams (round 7, docs/port/BRIEFING_round7.md); all inactive by default ----
// render_frame_1fab0's view calls (hud.cpp frame_pass / map_screen) go through render_view_frame:
// with an override installed (task B's compositor, extended mode) the 3D view is drawn elsewhere -
// e.g. at the display resolution - and the override puts what the game frame needs into `fb`.
using RenderViewFn = void (*)(const FrameBuffer &fb, const Camera &cam);
extern RenderViewFn g_render_view_override;           // null = render_view (the original)
inline void render_view_frame(const FrameBuffer &fb, const Camera &cam) {
    if (g_render_view_override) g_render_view_override(fb, cam); else render_view(fb, cam);
}
// Render-time interpolation between the last two simulation ticks (task D fills it, task A's Thing
// renderer reads it). Never read by game logic.
struct RenderInterp {
    bool active = false;
    uint32_t alpha = 0;                    // 0 = previous tick .. 0x10000 = current tick
    const int16_t (*prev_pos)[3] = nullptr; // per thing slot: x, y, z after the previous tick (null = no lerp)
    int prev_count = 0;                    // number of slots in prev_pos
    bool have_camera = false;
    Camera camera{};                       // interpolated local view, replaces player_camera(local)
};
extern RenderInterp g_render_interp;

// Extended renderer (round 7, task A; render_ext.cpp, docs/analysis/port_render_ext.md): draws the 3D
// view (sky + terrain + things, + the 2x2 smoothing / motion blur options) into the WHOLE of `fb` at its
// size (any size up to 3840x2160 and beyond), with the draw distance / fog / LOD of g_settings section
// A. Same vertical field of view as render_view for the same zoom; a wider frame sees more sideways.
// Does not use the render target of raster.h (saved and restored) nor g_work_buf. `cam` is the camera
// as render_view receives it (the slope low-pass of render_view_2f6e0 is applied here too, with its
// own state, so the faithful path's carried state is not disturbed).
void render_view_ext(const FrameBuffer &fb, const Camera &cam);
// The same into the current render target (g_rt_dest / pitch / width / height: the view window that
// render_set_view_window picked inside `fb`) - a drop-in RenderViewFn for g_render_view_override when
// the extended renderer runs at the game frame's own size (no compositor).
void render_view_ext_target(const FrameBuffer &fb, const Camera &cam);

// render_sky_2f080(roll) and render_build_roll_table_28580(roll) are internal but exposed for tests.
// render_landscape calls render_sky with the camera roll (the yaw scroll comes from g_cam_yaw16) and
// render_build_roll_table with (-roll) & 0x7ff.
void render_sky(int roll);
void render_build_roll_table(int roll);

// Hook for the Thing renderer (render_cell_things_2c600): called per visible cell with the first
// thing index of the cell. Null until the sprite renderer is ported.
using CellThingsFn = void (*)(int first_thing, const struct VertexRec *cell);
extern CellThingsFn g_render_cell_things;
// Same for render_cell_things_mirrored_2e5a0: called per cell of the mirrored (second surface) pass,
// after the reflected terrain quad (also for texture-0 cells, whose quad is not drawn).
extern CellThingsFn g_render_cell_things_mirrored;

// Water animation phase: the original reads the local player's tick (PlayerRec+0x12, "player+0x341d")
// as a dword and uses tick*0x40 as the wave phase. The game loop copies the player's tick here.
extern uint32_t g_anim_tick;

// Grid geometry and the squared-distance limits of render_landscape_29050 (Phase 3 enlarges them).
constexpr int     MC_GRID_COLS  = 40;         // 0x28 columns (lateral), -19..+20 cells
constexpr int     MC_GRID_ROWS  = 21;         // 0x15 rows (forward), -1..+19 cells
constexpr int     MC_GRID_CELLS = MC_GRID_COLS * MC_GRID_ROWS;   // 0x348 VertexRec at g_work_buf
constexpr int     kColStart     = -0x1300;    // lateral coordinate of column 0 (1/256 cell)
constexpr int     kRowStart     = -0x100;     // forward coordinate of row 0
constexpr int32_t kCullDist2    = 0x1900000;  // DAT_000b584c: vertex culled when x^2+z^2 >= this (20 cells)
constexpr int32_t kFogFar2      = 0x1690000;  // DAT_000b5850: shade reaches 0 here (19 cells)
constexpr int32_t kFogNear2     = 0xe10000;   // DAT_000b5848: full light below this (15 cells)
constexpr int32_t kFogDiv       = 0x880000;   // DAT_000b5844: = kFogFar2 - kFogNear2

// Camera state written by render_landscape_29050 (DAT_000b58xx), read by the sky and by the Thing /
// sprite renderers. Exposed for the later sprite port; render_landscape owns them.
struct RenderCamState {
    uint8_t  shadows;        // DAT_000b58af  = g_state->opt_shadows
    int32_t  screen_cx;      // DAT_000b5894  = width/2 + g_eye_offset
    int32_t  screen_cy;      // DAT_000b586c  = height/2
    uint16_t cam_x16;        // DAT_000b58ac
    uint16_t cam_y16;        // DAT_000b58aa
    uint16_t yaw16;          // DAT_000b58a8  yaw & 0x7ff
    int32_t  cam_z;          // DAT_000b5884
    int32_t  cos_yaw;        // DAT_000b5864  16.16
    int32_t  sin_yaw;        // DAT_000b58a0
    int32_t  focal;          // DAT_000b5854  = isqrt(w^2+h^2) * zoom >> 8
    int32_t  horizon;        // DAT_000b588c  = pitch * width >> 8
    int32_t  sin_roll;       // DAT_000b585c
    int32_t  cos_roll;       // DAT_000b5870
    int32_t  fog_far2;       // DAT_000b5850  (= kFogFar2, written every frame)
    int32_t  fog_near2;      // DAT_000b5848
    int32_t  cull_dist2;     // DAT_000b584c
    int32_t  fog_div;        // DAT_000b5844
};
extern RenderCamState g_rcam;
extern int32_t g_eye_offset;   // DAT_00093b1c (0 in mono)

// Screen-roll step table built by render_build_roll_table_28580 for the sprite renderer:
// DAT_000b3a10 has one 3-dword entry per pixel along the rolled major axis {delta offset to the
// previous entry, byte offset into the render target, number of minor-axis steps so far}, the list at
// g_work_buf + 0xe7e0 holds the (1-based) major-axis indices at which a minor step happened.
struct RollEntry { int32_t delta, offset, steps; };
constexpr int MC_ROLL_MAX = 640;                 // >= max(width, height)
extern RollEntry g_roll_table[MC_ROLL_MAX + 1];  // [0] is a zero guard: entry 0's delta reads entry -1
inline RollEntry *g_roll_entries() { return g_roll_table + 1; }   // DAT_000b3a10
constexpr size_t MC_ROLL_LIST_OFFSET = 0xe7e0;   // int32 list inside g_work_buf
struct RollState {
    int32_t octant;          // DAT_000b587c  = angle >> 8
    int32_t sin_a;           // DAT_000b58a4
    int32_t cos_a;           // DAT_000b5868
    int32_t step;            // DAT_000b5880  minor/major slope in 16.16 (0x10000 on the exact diagonals)
    int32_t steps;           // DAT_000b5878  minor steps over the whole major extent
    int32_t neg_steps;       // DAT_000b5888  = -steps
    int32_t clip_a;          // DAT_000b5858  = minor extent - steps
    int32_t extent_minor;    // DAT_000b5898  width or height (the minor axis extent)
    int32_t extent_major;    // DAT_000b589c  the major axis extent (entries built)
    int32_t extent_sum;      // DAT_000b5874  width + height
    int32_t list_last;       // DAT_000b5860  index into the list (steps - 1) of the last entry written
    int32_t minor_step;      // DAT_000b5890  byte step along the minor axis (+-1 or +-pitch)
};
extern RollState g_roll;

// ---- Phase 4 round 10 (task C): projection export (render_things.cpp; docs/analysis/port_inspect.md) ----
// Render-only: nothing here is read by the simulation, nothing writes game state.
//
// "View space" = the pixels of the target the 3D view was drawn into: the faithful renderer's render
// target (the view window inside the game frame), render_view_ext_target's window, or the compositor's
// hi-res view buffer (compose.h). Each view drawn records a RenderViewInfo (which renderer, its camera /
// projection after the slope nudge, the view-space -> game-frame mapping); the projection, the picks and
// the anchors below use the last one.
enum : int { RENDER_VIEW_NONE = 0, RENDER_VIEW_FAITHFUL = 1, RENDER_VIEW_EXT = 2 };
struct RenderViewInfo {
    int      kind = RENDER_VIEW_NONE;
    uint32_t serial = 0;               // ++ per recorded view
    int      view_w = 0, view_h = 0;   // view-space size
    // view pixel -> game-frame pixel: fx = frame_x0 + vx * frame_sx (fy likewise); frame_known false = the
    // view was drawn into a buffer that is not the game frame (render_view_ext called directly)
    bool     frame_known = false;
    bool     composed = false;         // the view is the compositor's buffer (compose_output().view)
    double   frame_x0 = 0, frame_y0 = 0, frame_sx = 1, frame_sy = 1;
    int      frame_w = 0, frame_h = 0; // the game frame (0 = unknown)
    // camera of the drawn view (after the slope low-pass)
    int      cam_x = 0, cam_y = 0, cam_z = 0, yaw = 0;
    RenderCamState rcam{};             // faithful: the copy of g_rcam (integer projection, thing_project)
    double   focal = 0, horizon = 0, scx = 0, scy = 0, cr = 1, sr = 0, cyaw = 1, syaw = 0;   // extended
    double   cull2 = 0;                // squared draw radius (world units)
};
const RenderViewInfo &render_view_info();

// The camera-space point (forward distance `depth`, world units) of world position x / y (world units,
// wrap-aware: the nearest copy on the N-cell torus) / z (world height) projected with the last view, in view
// space. False when the point is not in front of the camera (depth <= 0x40, the renderers' thing cull)
// or no view was recorded; sx / sy may lie outside the view.
bool render_project_world(int32_t x, int32_t y, int32_t z, int *sx, int *sy, int *depth);
// View space -> game-frame pixels (false when the mapping is unknown) and back.
bool render_view_to_frame(double vx, double vy, double *fx, double *fy);
bool render_frame_to_view(double fx, double fy, double *vx, double *vy);
// Game-frame pixels -> the 640-wide virtual HUD space of ui_draw.h (x * 640 / frame width, y * 480 or
// 400 / frame height).
void render_frame_to_hud640(double fx, double fy, int *hx, int *hy);
// render_project_world straight into the 640-wide HUD space.
bool render_project_world_hud640(int32_t x, int32_t y, int32_t z, int *hx, int *hy, int *depth);

// Inverse projection: the ray through view pixel (sx, sy) against the height field (terrain_sample_height),
// first hit within the draw radius (x 1.5). cell_x / cell_y = the map cell; wx / wy / wz (optional) = the
// world point. False when the ray hits nothing (sky).
bool render_pick_ground(int sx, int sy, int *cell_x, int *cell_y, int32_t *wx = nullptr, int32_t *wy = nullptr,
                        int32_t *wz = nullptr);

// Per-frame list of projected Thing anchors (the drawn sprite, not shadows / reflections): collected by both
// renderers while a capture is open with anchors on (render_capture_begin), cleared when a view is drawn.
struct RenderAnchor {
    uint16_t slot;
    uint32_t generation;               // thing_slot_generation(slot) when drawn
    int32_t  sx, sy;                   // view space: bottom centre of the sprite (the blitter's anchor point)
    int32_t  w, h;                     // on-screen size (view pixels; unrotated)
    int32_t  depth;                    // camera-space forward distance (world units)
    int32_t  x, y, z;                  // world position drawn (extended: interpolated / segment layout)
};
const RenderAnchor *render_anchors(int *count);
// The anchor nearest to view pixel (sx, sy): the front-most sprite whose box contains it, else the nearest box
// edge within max_dist pixels. Returns the slot, -1 for none.
int render_pick_thing(int sx, int sy, int max_dist = 12);

// Capture bracket around one frame's 2D pass (render_frame_draw ... ): installs a pass-through wrapper as
// g_render_view_override (it calls the override that was there, or render_view - the pixels are the same)
// which records the view drawn; `anchors` turns the anchor collection on. render_capture_end restores the
// override. Without a capture nothing of this runs (the faithful path is untouched); the extended renderer
// records its RenderViewInfo always (a few doubles per frame).
void render_capture_begin(const FrameBuffer &fb, bool anchors);
void render_capture_end();
bool render_capture_active();

// Internal (render_ext.cpp / compose.cpp -> render_things.cpp): recording by the renderers.
RenderViewInfo *render_view_info_edit();
void render_view_info_ext_drawn(const uint8_t *target);   // after the extended renderer filled the record
void render_view_info_set_frame_mapping(double x0, double y0, double sx, double sy, int frame_w, int frame_h, bool composed);
bool render_anchors_wanted();
void render_anchors_clear();
void render_anchor_push(const RenderAnchor &a);
