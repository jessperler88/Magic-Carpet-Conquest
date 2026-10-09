// In-level pause menu (port round 9, port-only; docs/analysis/port_round9.md). The original has no such
// menu: Esc in flight leaves the level at once (commands 0x1b / 0x1d), saving is the front end's six
// campaign slots plus the Alt+S quick save, and the options are F-keys. Opened with keys.menu (Esc) in a
// level, the menu offers
//   Resume / Save state (slots 1..9, 0 = the quick slot) / Load state / Options / Leave level / Quit game.
// "Leave level" hands the original's Esc to the game. Options edit mcport.ini keys (config.h) and apply at
// once; the changed keys are written into mcport.ini (config_set_keys) when the menu closes.
//
// The menu is drawn by the game's own 2D code (ui_draw.h, the HUD font) into the 640-wide virtual screen
// (640 x 400 in the 320x200 mode, 640 x 480 otherwise), so it lands in the HUD layer when composed. Input
// is device-neutral: keys (SDL scancodes: arrows, Enter, Esc, Backspace, Page Up / Down) and a pointer in
// the virtual screen (the mouse, or the controller's menu cursor + A / B as main.cpp feeds them).
// It never touches the game state; main.cpp runs the commands it returns (pausing the level while open,
// except in a network game).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "config.h"

struct GameMenuEnv {
    bool network = false;        // a network game: the level keeps running, no save / load
    bool can_save = true;        // savestate_allowed() (not while a movie plays / records)
    int  level = 0;              // the level being played (0-based, for the title)
    bool fullscreen = false;     // the platform's current state (the Fullscreen row)
    bool mode_run = false;       // round 12: a Conquest match - "Leave level" reads "Forfeit the match" (LEAVE_LEVEL;
                                 // main.cpp shows the result screen)
};

enum class GameMenuCmd { NONE, RESUME, SAVE, LOAD, LEAVE_LEVEL, QUIT, FULLSCREEN };
struct GameMenuResult {
    GameMenuCmd cmd = GameMenuCmd::NONE;
    int  slot = 0;               // SAVE / LOAD
    bool on = false;             // FULLSCREEN: the wanted state
};

class GameMenu {
public:
    enum Page { PAGE_MAIN, PAGE_SAVE, PAGE_LOAD, PAGE_OPTIONS };

    // Opens on the main page. `s` / `p` are edited in place by the options page.
    void open(const GameMenuEnv &env, PortSettings *s, PlatformOptions *p);
    // Closes (also what RESUME / LEAVE_LEVEL / QUIT / LOAD / SAVE imply: main.cpp calls it). Returns the
    // config keys changed while it was open with their new values (config_get form), for config_set_keys.
    std::vector<std::pair<std::string, std::string>> close();
    bool is_open() const { return open_; }
    Page page() const { return page_; }
    int  selected() const { return sel_; }

    // A key press (SDL scancode). Up / Down select, Left / Right change an option, Enter activates, Esc /
    // Backspace go back (Esc on the main page = Resume).
    GameMenuResult key(int sdl_scancode);
    // Pointer in the virtual screen (640 x vh): hover selects; `click` activates (on an option row: the
    // left half steps it down, the right half up); `back` (right button / controller B) = Esc.
    GameMenuResult pointer(int x, int y, bool click, bool back, int vh);
    // Mouse wheel: scrolls the options / changes nothing else.
    void wheel(int dy);

    // Draws the menu over whatever the target holds (call after the game's 2D pass, ui_set_target done).
    // `vh` = 400 / 480; (px, py) = the pointer (the game's book pointer sprite), px < 0 = none.
    void draw(int vh, int px = -1, int py = -1);
    // A one-line message under the panel (e.g. "slot 3 saved"), shown until the next input.
    void set_message(const std::string &m) { message_ = m; }

    // The slot list is read when a save / load page opens; tests can refresh it.
    void refresh_slots();
    // Tests: the text of row `i` of the current page (label + value), -1 = the title.
    std::string row_text(int i) const;
    int row_count() const;

private:
    struct Row;
    std::vector<Row> rows() const;
    GameMenuResult activate(int i, int dir);     // dir 0 = Enter / click, -1 / +1 = Left / Right
    void go(Page p);
    void layout(int vh, int *x, int *y, int *w, int *row_h, int *visible) const;
    void step_option(int opt, int dir);
    std::string option_value(int opt) const;

    bool open_ = false;
    Page page_ = PAGE_MAIN;
    int sel_ = 0, scroll_ = 0;
    int confirm_slot_ = -1;                      // save page: Enter again overwrites this slot
    GameMenuEnv env_;
    PortSettings *s_ = nullptr;
    PlatformOptions *p_ = nullptr;
    std::vector<std::pair<std::string, std::string>> changed_;
    std::string message_;
    struct SlotInfo { bool used = false; int level = 0; unsigned tick = 0; uint64_t time = 0; std::string name; };
    SlotInfo slots_[10];
};
