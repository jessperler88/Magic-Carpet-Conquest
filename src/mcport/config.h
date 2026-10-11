// mcport configuration (port round 7, task E; docs/analysis/port_settings.md): the config file
// mcport.ini in the save directory, environment variables and command-line overrides, filling
// PortSettings g_settings (engine, settings.h) and PlatformOptions (this file: window, audio, keys, pad).
//
// Precedence, lowest first:
//   1. built-in defaults - the *playing* defaults of config_playing_defaults() (far draw distance, larger
//      Thing pool, native-resolution view, interpolation, full quick save), not the engine's faithful
//      defaults (tests and references keep a default-constructed PortSettings);
//   2. mcport.ini (created on first run with every key commented out at its default, with a comment;
//      keys added by a later version are appended the same way; unknown keys are kept in the file and
//      reported as warnings);
//   3. the environment variables that existed before (MC_TICK_HZ, MC_SOUND, MC_SOUND_VOLUME, MC_MUSIC,
//      MC_MUSIC_VOLUME, MC_MOVIE_DIR, MC_INTERPOLATE, MC_FPS_CAP) and MC_FAITHFUL=1;
//   4. the command line, left to right: --faithful (every PortSettings field back to the original's
//      behaviour), --set section.key=value (repeatable), --config <file> (another ini file, read at step 2).
// Every value is range-checked: out-of-range numbers are clamped, malformed ones ignored, with a warning.
// After loading, config_load exports the audio / pacing values to the environment variables above, so
// the code that reads them (audio_sdl.cpp, net_tcp.cpp, main.cpp) sees the final value.
#pragma once
#include <string>
#include <vector>
#include "settings.h"
#include "gamepad.h"

struct PlatformOptions {
    // [video]
    int  window_width = 1280;       // initial window client size (64..16384)
    int  window_height = 960;
    int  fullscreen = 0;            // 0 window, 1 borderless full screen on `display`
    int  display = 0;               // display index (0 = primary)
    bool vsync = true;
    // [audio]
    bool        sound = true;       // MC_SOUND (0 = off)
    int         sound_volume = 96;  // MC_SOUND_VOLUME: sample mix gain, 256 = unity (0..512)
    std::string music = "opl";      // MC_MUSIC: opl | midi | square | off
    int         music_volume = 256; // MC_MUSIC_VOLUME: OPL output gain, 256 = default (0..1024)
    // [game]
    double      tick_hz = 25.0;     // MC_TICK_HZ: simulation ticks per second (1..1000)
    std::string movie_dir;          // MC_MOVIE_DIR: full-length FLI movies ("" = the CD folder if found)
    // [keys]: port actions; game keys are the original's (docs/analysis/port_input.md)
    KeyChord save_slot[10];         // save the state into slot 0..9 (0 = the quick slot)
    KeyChord load_slot[10];
    KeyChord quit_now;              // leave the port at once (F12)
    KeyChord menu;                  // round 9: the in-level pause menu (Esc; main.cpp only takes it in a level)
    bool mouse_invert_y = true;     // flight: mouse forward = pointer up, as the original (the nose goes down); off:
                                    // mouse forward = pointer down. Not the spell book / map pointer, not the free camera
    // [pad]
    GamepadConfig pad;

    // filled by config_load
    std::string ini_path;           // the file that was read ("" when none)
    std::vector<std::string> warnings;
    bool faithful = false;          // --faithful / MC_FAITHFUL was given
    PlatformOptions();
};

// The defaults mcport plays with (the engine's own defaults are the faithful ones).
void config_playing_defaults(PortSettings *s);

// Read everything (see the precedence above). `save_dir` is SDL_GetPrefPath("Bullfrog", "MagicCarpet")
// (may be "" / null: no file). Options it understands are removed from argv (argc is updated), so the
// positional arguments (game dir, mode, level) keep their indices. Warnings are printed to stderr and
// kept in p->warnings. Returns false only when the command line is malformed (unknown --option).
bool config_load(const char *save_dir, int *argc, char **argv, PortSettings *s, PlatformOptions *p);

// Pieces of config_load (tests, tools).
bool config_apply(const char *key, const char *value, PortSettings *s, PlatformOptions *p, std::string *warning);
std::string config_get(const char *key, const PortSettings &s, const PlatformOptions &p);   // "" = unknown key
bool config_read_file(const char *path, PortSettings *s, PlatformOptions *p, std::vector<std::string> *warnings,
                      std::vector<std::string> *missing_keys);
bool config_write_default_file(const char *path);               // every key commented out at its default
// Every key with these values (commented = "# key = value" lines, which read as "use the default").
bool config_write_file(const char *path, const PortSettings &s, const PlatformOptions &p, bool commented = false);
bool config_append_keys(const char *path, const std::vector<std::string> &keys);  // keys a newer version added
// Round 9 (in-game options menu): set `key` = value (config_get form) for each pair in the file, in place - a
// "key = v" or "# key = v" line of the key's section is replaced by an active "key = value" line, the rest
// of the file (comments, unknown keys, other values) is kept; keys not found are appended under their
// section header. A missing file is created first (config_write_default_file).
bool config_set_keys(const char *path, const std::vector<std::pair<std::string, std::string>> &values);
void config_apply_env(PortSettings *s, PlatformOptions *p, std::vector<std::string> *warnings);
void config_export_env(const PlatformOptions &p);
std::vector<std::string> config_keys();                         // "section.key" of every known key

// Key chords: "F11", "Ctrl+F1", "Shift+Alt+S", "none". Names: A..Z, 0..9, F1..F12, Esc, Enter, Space,
// Tab, Backspace, Up, Down, Left, Right, Insert, Delete, Home, End, PageUp, PageDown, Minus, Equals,
// [, ], Backquote, KP0..KP9, Pause.
bool        config_parse_key(const char *text, KeyChord *out);
std::string config_key_name(const KeyChord &k);
bool        config_parse_pad_action(const char *text, PadAction *out);   // "cast_left", "Enter", "Ctrl+1", ...
std::string config_pad_action_name(const PadAction &a);

// Port actions for a key press (SDL scancode + KEYMOD_* of the held modifiers). main.cpp swallows the
// key (does not feed it to the game) when this returns something.
enum PortKeyAction { PORT_KEY_NONE, PORT_KEY_SAVE, PORT_KEY_LOAD, PORT_KEY_QUIT, PORT_KEY_MENU };
PortKeyAction config_key_action(const PlatformOptions &p, int sdl_scancode, uint8_t mods, int *slot);

// One-line summary of the PortSettings in effect (printed at start-up).
std::string config_summary(const PortSettings &s);
