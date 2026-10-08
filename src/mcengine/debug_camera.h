// Debug camera override (Phase 4, round 10 task D; port-only, nothing here exists in carpet.exe).
//
// While `active`, the game view of a level (hud.cpp frame_pass: flight view, help screen, the map screen's
// small window) is drawn from `cam` instead of the local player's camera. Render-only: frame_pass uses it
// only in its draw-only pass (render_frame_draw), never in hud_tick_state / render_frame (the passes that
// write game state), and nothing in the tick reads it. mcport (main.cpp) drives it: the free-camera keys,
// `cam follow <slot>` (the Thing inspector's follow mode), `cam to x y`.
//
// Coordinates (round 11): cam.cam_x / cam_y are world units wrapped into 0 .. g_world.units - 1 (world.h) by
// whoever moves the camera (main.cpp, debug_inspect_follow_camera); the renderers wrap them again anyway.
//
// Header-only (an inline variable) so every target that links hud.cpp links without another source.
#pragma once
#include "render.h"   // Camera

struct DebugCamera {
    bool   active = false;
    Camera cam{};
};
inline DebugCamera g_debug_camera;
