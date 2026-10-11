// The in-game debug console (Phase 4, round 10 task B; docs/analysis/port_console.md). Port-only.
//
// Opened / closed with the console key (default ` Backquote, not a key the game reads: input.h
// g_input_bindings, main.cpp set1_scancode). While it is open it takes every key before the game sees it (the
// level keeps running; the game's own keys are not fed), Esc closes it. A text line with a cursor, history
// (Up / Down, kept in <save dir>/console_history.txt), scrollback (Page Up / Down; it shows mcport's log, D's
// mclog, so every message of the port appears in it), Tab completes command names, Ctrl+U / Ctrl+C clear the
// line, Ctrl+L the scrollback. Text comes from scancodes + Shift (US layout, console_key_ascii): no SDL text
// input.
//
// Every line goes through the command language of scenario.h (cmd_parse): simulation commands become debug
// packets queued for the local player's command slot (debug_cmd_queue; mcport's sim_tick pumps one per tick
// through before_tick), queries / assertions / help print into the log, host commands (time, shot, save, load,
// sync, dump, cam, overlay, inspect, quit) are handed to main.cpp (take_host_commands), `run file.scn` runs a
// scenario from the current tick (after_tick steps it).
//
// Headless: start_stdin_reader() (MC_CONSOLE_STDIN=1 / --console-stdin) reads lines from stdin on a thread;
// poll_stdin() executes them between ticks. MC_SCENARIO=file / --scenario file: the scenario's header decides
// the run (level L -> play, rts ... -> a mode run) and it starts with the level; when it ends the console asks
// main.cpp to quit (HostOp::QUIT) and exit_code() is 1 if an assertion failed.
//
// Drawn after the HUD (draw) with ui_draw_text / ui_shade_rect in the 640-wide virtual screen, so it is part of
// the HUD layer when composed. Never writes game state itself (only through queued packets).
#pragma once
#include "scenario.h"
#include "gamepad.h"            // KeyChord, KEYMOD_*
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// The ASCII character of a key press (SDL scancode, US layout; Shift selects the upper row). 0 = none.
char console_key_ascii(int sdl_scancode, bool shift);

// `--console-stdin` and `--scenario FILE` (removed from argv; call before config_load) and the environment
// switches MC_CONSOLE_STDIN=1 / MC_SCENARIO=file.
struct ConsoleOptions {
    bool stdin_reader = false;
    std::string scenario;       // "" = none
};
ConsoleOptions console_parse_args(int *argc, char **argv);

class Console {
public:
    // History from <save_dir>/console_history.txt ("" = no file).
    void init(const std::string &save_dir);

    bool is_open() const { return open_; }
    void open();
    void close();
    void toggle() { if (open_) close(); else open(); }
    KeyChord toggle_key{53, 0};                 // SDL_SCANCODE_GRAVE ("Backquote"); [keys] console
    bool is_toggle(int sdl_scancode, uint8_t mods) const {
        return toggle_key.scancode != 0 && sdl_scancode == toggle_key.scancode && mods == toggle_key.mods;
    }

    // A key press (SDL scancode, KEYMOD_* bits) while open: editing, Enter executes the line.
    void key(int sdl_scancode, uint8_t mods);
    // Runs one line as if typed (keyboard, stdin, tests): output into the log / scrollback, packets queued,
    // host commands collected for take_host_commands.
    void execute(const std::string &line);
    // Host commands of executed lines / the running scenario since the last call (main.cpp runs them).
    std::vector<HostCmd> take_host_commands();

    // After every simulation tick (mcport sim_tick, after it logged debug_cmd_take_messages): the scenario step.
    // (Before the tick sim_tick calls debug_cmd_pump: the next queued packet into the command slot.)
    void after_tick();

    // Scenario files. from_start: the ticks of the file are the level's (MC_SCENARIO, the level was just
    // started); else they count from now. quit_when_done: ask main.cpp to quit when it ends (MC_SCENARIO).
    bool run_file(const std::string &path, bool quit_when_done, std::string *err);
    bool scenario_running() const;
    int  exit_code() const { return exit_code_; }
    void stop_scenario();

    // stdin (headless): a reader thread; poll_stdin executes the lines that arrived (call once per frame).
    void start_stdin_reader();
    void poll_stdin();

    // Draws the console over the target (ui_set_target done) when open; `vh` = 400 / 480.
    // menu_pal6 (the front end: the palette on screen, 256 x 6-bit RGB): a solid panel in that palette's colours.
    void draw(int vh, const uint8_t *menu_pal6 = nullptr);
    // Messages shown in the scrollback (also mirrored to the log): mclog lines plus the console's own.
    void print(const std::string &text, int level = 2 /* MCLOG_INFO */);

    // ---- tests ----
    const std::string &line() const { return line_; }
    int cursor() const { return cur_; }
    const std::vector<std::string> &history() const { return hist_; }
    std::vector<std::string> scrollback_text() const;   // what the scrollback holds (oldest first)
    const std::string &history_path() const { return hist_path_; }

private:
    void add_history(const std::string &l);
    void pull_log();
    std::string resolve_path(const std::string &p) const;

    bool open_ = false;
    std::string line_;
    int cur_ = 0;
    std::vector<std::string> hist_;
    int hist_pos_ = -1;                         // -1 = editing a new line
    std::string draft_;
    std::string hist_path_, save_dir_;
    struct Out { std::string text; int level; };
    std::deque<Out> scroll_;
    int scroll_off_ = 0;                        // lines scrolled back from the bottom
    uint64_t log_seq_ = 0;
    std::vector<HostCmd> host_;
    bool quit_when_done_ = false;
    int exit_code_ = 0;
    int scn_tick_ = 0;
    void (*saved_input_)() = nullptr;
    bool input_installed_ = false;
};
