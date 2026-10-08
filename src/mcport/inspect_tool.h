// Port-only (Phase 4 round 10, task C): the mcport side of the Thing inspector / overlays - the cursor mode
// (absolute pointer at display resolution, steering frozen), picking with the mouse, the tool keys.
// The drawing is mcengine/debug_overlay.*; the main.cpp calls are in docs/analysis/port_inspect.md.
//
// Keys (SDL scancodes the game never reads; main.cpp does not feed them to the game when they are handled;
// round 10 task D owns Pause / End / Page Up / Page Down / Insert / Delete for time control and the camera):
//   Home or middle mouse button     cursor mode on / off (absolute pointer, steering frozen)
//   (cursor mode) left click        inspect the Thing under the pointer (and pick its ground cell); on nothing:
//                                   inspector off
//   (cursor mode) right click       pick the ground cell only (released without moving)
//   (cursor mode) right drag        orbit the follow camera around the followed Thing; wheel = zoom
//   following, cursor mode off      the mouse orbits the camera around the Thing, wheel = zoom (steering frozen)
//   Backspace, Shift+Home, keypad . inspector off (Thing and cell); Backspace not while typing a chat line
//   Shift+Backspace, keypad /       hide / show the panel only (inspection, follow and orbit go on)
//   Ctrl+Home, keypad 8             follow the inspected Thing with the debug camera on / off
//   keypad 7 / keypad 9             inspect the previous / next live Thing slot
//   keypad 1..6                     overlays grid / labels / anchors / occupancy / damage / net on / off
//   keypad 0                        every overlay off
#pragma once
#include <cstdint>

namespace mc { class Platform; struct Input; }
struct Camera;

// A key press (SDL scancode, the KEYMOD_* bits of config.h). True = the tool's key: do not feed it to the game.
// `level` = a level is running (the tool keys do nothing elsewhere).
bool inspect_tool_key(mc::Platform &plat, int sdl_scancode, uint8_t mods, bool level);
// Once per frame after Platform::poll, in a level (`level` false: the cursor mode is switched off). Tracks the
// pointer, picks on button presses. Returns true while the cursor mode owns the mouse: main.cpp then feeds the
// game the centre position (no steering) and no mouse buttons.
bool inspect_tool_mouse(mc::Platform &plat, mc::Input &in, bool level);
bool inspect_tool_cursor_mode();
void inspect_tool_set_cursor_mode(mc::Platform &plat, bool on);
// Cursor mode: the pointer in game-frame pixels (main.cpp draws the spell book's pointer there); false = none.
bool inspect_tool_pointer_frame(int *fx, int *fy);
// The last ground cell picked with the pointer (for a cursor spawn / teleport); false = none.
bool inspect_tool_cursor_cell(int *cell_x, int *cell_y);
// Follow mode: the camera the debug camera should use this frame (after the interpolation was filled).
bool inspect_tool_follow_camera(Camera *cam);
