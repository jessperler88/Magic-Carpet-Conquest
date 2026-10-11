// mcport configuration: mcport.ini, environment, command line (config.h). Port-only.
#define _CRT_SECURE_NO_WARNINGS
#include "config.h"
#include <SDL_scancode.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

PlatformOptions::PlatformOptions() {
    // Ctrl+F1..F9 save slot 1..9, Shift+F1..F9 load it; F10 = slot 0, the quick slot (also Alt+S, the
    // original's quick save, with game.quicksave_full). The game reads F1..F10 only without modifiers.
    for (int i = 0; i <= 9; i++) {
        const int f = i == 0 ? SDL_SCANCODE_F10 : SDL_SCANCODE_F1 + i - 1;
        save_slot[i] = {f, KEYMOD_CTRL};
        load_slot[i] = {f, KEYMOD_SHIFT};
    }
    quit_now.scancode = SDL_SCANCODE_F12;
    menu.scancode = SDL_SCANCODE_ESCAPE;
}

void config_playing_defaults(PortSettings *s) {
    *s = PortSettings{};
    s->render_extended = true;      // A: the far view
    s->draw_distance = 125;         // far view (127 = the wrap limit), fog from 60 % of it
    s->fog_start_pct = 60;
    s->lod = 1;
    s->compose = 1;                 // B: the 3D view at the window's resolution under the 2D layer
    s->view_width = 0;
    s->view_height = 0;
    s->hud_scale_mode = 0;
    s->hud_corners = true;
    s->radar_zoom_pct = 150;        // radar ~96 cells each way, closer to the far view
    s->radar_round = true;
    s->thing_slots = 8192;          // C: ~3x the highest peak measured (2627, level 17) - mana balls must spawn
    s->interpolate = true;          // D: smooth motion between the 25 Hz ticks
    s->fps_cap = 0;
    s->quicksave_full = true;       // E: Alt+S / quick load keep consistent lists
    s->possession_range_pct = 130;
    s->mouse_book_both = false;     // user, 2026-10-11: both buttons fire both hands, they do not open the book
    s->keys_wasd = true;           // round 8 (user request): WASD flies, Tab opens the spell book
    s->keys_book_tab = true;  // round 8 (user request): Possession reaches 30 % further
    s->mode_menu = true;            // round 12: the Conquest urn in the main menu and the lobby
}

// ---- key names -------------------------------------------------------------------------------------
namespace {
struct KeyName { const char *name; int sc; };
const KeyName k_keys[] = {
    {"Esc", SDL_SCANCODE_ESCAPE}, {"Escape", SDL_SCANCODE_ESCAPE}, {"Enter", SDL_SCANCODE_RETURN},
    {"Return", SDL_SCANCODE_RETURN}, {"Space", SDL_SCANCODE_SPACE}, {"Tab", SDL_SCANCODE_TAB},
    {"Backspace", SDL_SCANCODE_BACKSPACE}, {"Up", SDL_SCANCODE_UP}, {"Down", SDL_SCANCODE_DOWN},
    {"Left", SDL_SCANCODE_LEFT}, {"Right", SDL_SCANCODE_RIGHT}, {"Insert", SDL_SCANCODE_INSERT},
    {"Delete", SDL_SCANCODE_DELETE}, {"Home", SDL_SCANCODE_HOME}, {"End", SDL_SCANCODE_END},
    {"PageUp", SDL_SCANCODE_PAGEUP}, {"PageDown", SDL_SCANCODE_PAGEDOWN}, {"Minus", SDL_SCANCODE_MINUS},
    {"Equals", SDL_SCANCODE_EQUALS}, {"[", SDL_SCANCODE_LEFTBRACKET}, {"]", SDL_SCANCODE_RIGHTBRACKET},
    {"LeftBracket", SDL_SCANCODE_LEFTBRACKET}, {"RightBracket", SDL_SCANCODE_RIGHTBRACKET},
    {"Backquote", SDL_SCANCODE_GRAVE}, {"Pause", SDL_SCANCODE_PAUSE},
};
bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
    return *a == *b;
}
std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
int key_from_name(const std::string &n) {
    if (n.size() == 1 && std::isalpha((unsigned char)n[0])) return SDL_SCANCODE_A + (std::toupper((unsigned char)n[0]) - 'A');
    if (n.size() == 1 && n[0] >= '1' && n[0] <= '9') return SDL_SCANCODE_1 + (n[0] - '1');
    if (n == "0") return SDL_SCANCODE_0;
    if ((n[0] == 'F' || n[0] == 'f') && n.size() <= 3 && n.size() >= 2 && std::isdigit((unsigned char)n[1])) {
        const int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return SDL_SCANCODE_F1 + f - 1;
    }
    if (n.size() == 3 && (n[0] == 'K' || n[0] == 'k') && (n[1] == 'P' || n[1] == 'p') && std::isdigit((unsigned char)n[2]))
        return n[2] == '0' ? SDL_SCANCODE_KP_0 : SDL_SCANCODE_KP_1 + (n[2] - '1');
    for (const KeyName &k : k_keys) if (ieq(k.name, n.c_str())) return k.sc;
    return 0;
}
std::string name_from_key(int sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return std::string(1, (char)('A' + sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return std::string(1, (char)('1' + sc - SDL_SCANCODE_1));
    if (sc == SDL_SCANCODE_0) return "0";
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return "F" + std::to_string(sc - SDL_SCANCODE_F1 + 1);
    if (sc == SDL_SCANCODE_KP_0) return "KP0";
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return "KP" + std::to_string(sc - SDL_SCANCODE_KP_1 + 1);
    for (const KeyName &k : k_keys) if (k.sc == sc) return k.name;
    return "";
}
} // namespace

bool config_parse_key(const char *text, KeyChord *out) {
    std::string t = trim(text ? text : "");
    KeyChord k;
    if (t.empty() || ieq(t.c_str(), "none")) { *out = k; return true; }
    for (;;) {
        // "[" / "]" may themselves be the key after a '+'
        const size_t plus = t.size() > 1 ? t.find('+') : std::string::npos;
        if (plus == std::string::npos) break;
        const std::string m = trim(t.substr(0, plus));
        if (ieq(m.c_str(), "ctrl")) k.mods |= KEYMOD_CTRL;
        else if (ieq(m.c_str(), "shift")) k.mods |= KEYMOD_SHIFT;
        else if (ieq(m.c_str(), "alt")) k.mods |= KEYMOD_ALT;
        else return false;
        t = trim(t.substr(plus + 1));
    }
    k.scancode = key_from_name(t);
    if (!k.scancode) return false;
    *out = k;
    return true;
}

std::string config_key_name(const KeyChord &k) {
    if (!k.scancode) return "none";
    std::string s;
    if (k.mods & KEYMOD_CTRL) s += "Ctrl+";
    if (k.mods & KEYMOD_SHIFT) s += "Shift+";
    if (k.mods & KEYMOD_ALT) s += "Alt+";
    const std::string n = name_from_key(k.scancode);
    return s + (n.empty() ? "none" : n);
}

namespace {
struct PadActName { const char *name; PadAction::Kind kind; };
const PadActName k_pad_actions[] = {
    {"none", PadAction::NONE}, {"cast_left", PadAction::CAST_LEFT}, {"cast_right", PadAction::CAST_RIGHT},
    {"spell_next", PadAction::SPELL_NEXT}, {"spell_prev", PadAction::SPELL_PREV},
    {"spell_next_right", PadAction::SPELL_NEXT_RIGHT}, {"spell_prev_right", PadAction::SPELL_PREV_RIGHT},
    {"save_quick", PadAction::SAVE_QUICK}, {"load_quick", PadAction::LOAD_QUICK},
};
}

bool config_parse_pad_action(const char *text, PadAction *out) {
    const std::string t = trim(text ? text : "");
    for (const PadActName &a : k_pad_actions)
        if (ieq(a.name, t.c_str())) { *out = PadAction{}; out->kind = a.kind; return true; }
    KeyChord k;
    if (!config_parse_key(t.c_str(), &k) || !k.scancode) return false;
    *out = PadAction{};
    out->kind = PadAction::KEY;
    out->key = k;
    return true;
}

std::string config_pad_action_name(const PadAction &a) {
    if (a.kind == PadAction::KEY) return config_key_name(a.key);
    for (const PadActName &n : k_pad_actions) if (n.kind == a.kind) return n.name;
    return "none";
}

// ---- the key table -----------------------------------------------------------------------------------
namespace {
enum Kind { K_BOOL, K_INT, K_FLOAT, K_DOUBLE, K_STRING, K_KEY, K_PAD };
using Getter = void *(*)(PortSettings &, PlatformOptions &);
struct Desc {
    const char *section, *key;
    Kind kind;
    Getter get;
    double lo, hi;
    const char *env;        // environment variable (or null)
    const char *help;
};
#define PS(f) [](PortSettings &s, PlatformOptions &) -> void * { return &s.f; }
#define PO(f) [](PortSettings &, PlatformOptions &p) -> void * { return &p.f; }

// Settings added by tasks A..D (settings.h) appear here as well; a new PortSettings field needs one line.
const Desc k_desc[] = {
    // ---- PortSettings (settings.h) ----
    {"render", "extended", K_BOOL, PS(render_extended), 0, 1, nullptr,
     "1 = the extended renderer (far view, LOD, any resolution); 0 = the original's 40x21-cell landscape"},
    {"render", "draw_distance", K_INT, PS(draw_distance), 8, 255, nullptr,
     "extended: view radius in cells (original 20; at most 127 = half the 256-cell map in the campaign, 255 in a "
     "generated Conquest world)"},
    {"render", "fog_start_pct", K_INT, PS(fog_start_pct), 0, 100, nullptr,
     "extended: full light below this % of the draw distance (original 75)"},
    {"render", "lod", K_INT, PS(lod), 0, 2, nullptr,
     "extended: 0 = full textures everywhere, 1 = quality (mips / merged quads far away), 2 = fast"},
    {"render", "fog_mode", K_INT, PS(fog_mode), 0, 2, nullptr,
     "extended: 0 = auto (haze into the textured sky, else darken), 1 = darken (original), 2 = haze"},
    {"render", "render_threads", K_INT, PS(render_threads), 0, 64, nullptr,
     "extended: rasteriser threads (0 = auto, 1 = single-threaded)"},
    {"render", "thing_min_px", K_INT, PS(thing_min_px), 1, 16, nullptr,
     "extended: skip things smaller than this many pixels on screen"},
    {"display", "compose", K_INT, PS(compose), 0, 1, nullptr,
     "0 = the original 320x200 / 640x480 frame scaled to the window; 1 = 3D view at the window's resolution"},
    {"display", "view_width", K_INT, PS(view_width), 0, 7680, nullptr,
     "native: 3D view render width (0 = the window's; else 64..7680, scaled to the window)"},
    {"display", "view_height", K_INT, PS(view_height), 0, 4320, nullptr,
     "native: 3D view render height (0 = the window's)"},
    {"display", "hud_scale_mode", K_INT, PS(hud_scale_mode), 0, 2, nullptr,
     "native: HUD scaling 0 = integer nearest, 1 = fit nearest, 2 = fit filtered"},
    {"display", "radar_zoom_pct", K_INT, PS(radar_zoom_pct), 50, 200, nullptr,
     "flight radar zoom-out in % (100 = the original, 64 cells each way; 150 = 96 cells)"},
    {"display", "radar_round", K_BOOL, PS(radar_round), 0, 1, nullptr,
     "320x200: flight radar drawn as a circle on a 4:3 display (0 = the original's tall oval)"},
    {"display", "hud_corners", K_BOOL, PS(hud_corners), 0, 1, nullptr,
     "native, widescreen: radar / status / spell labels at the display corners"},
    {"game", "thing_slots", K_INT, PS(thing_slots), 1000, 32768, nullptr,
     "Thing pool size (original 1000: busy late levels run out and stop spawning mana balls)"},
    {"game", "thing_cap_villagers", K_BOOL, PS(thing_cap_villagers), 0, 1, nullptr,
     "extended pool: towns stop sending villagers at 999 slots in use, as the original (0 = unlimited)"},
    {"game", "possession_range_pct", K_INT, PS(possession_range_pct), 100, 200, nullptr,
     "Possession (mana claim) range in % of the original (100 = the original: 11 ticks, 20-cell target pick)"},
    {"game", "quicksave_full", K_BOOL, PS(quicksave_full), 0, 1, nullptr,
     "Alt+S quick save / quick load: 1 = the complete state (slot 0), 0 = the original's gam10000.dat"},
    {"game", "mode_menu", K_BOOL, PS(mode_menu), 0, 1, nullptr,
     "the Conquest mode's urn in the main menu (setup screen) and the multiplayer lobby (0 = the original's menus)"},
    {"pacing", "interpolate", K_BOOL, PS(interpolate), 0, 1, "MC_INTERPOLATE",
     "draw between the last two ticks (smooth motion above the tick rate; display only)"},
    {"pacing", "fps_cap", K_INT, PS(fps_cap), 0, 1000, "MC_FPS_CAP",
     "frame rate limit (0 = none / vsync decides)"},
    // ---- PlatformOptions ----
    {"video", "window_width", K_INT, PO(window_width), 64, 16384, nullptr, "window client width at start-up"},
    {"video", "window_height", K_INT, PO(window_height), 64, 16384, nullptr, "window client height at start-up"},
    {"video", "fullscreen", K_INT, PO(fullscreen), 0, 1, nullptr, "0 = window, 1 = borderless full screen (Alt+Enter toggles)"},
    {"video", "display", K_INT, PO(display), 0, 15, nullptr, "display index (0 = primary)"},
    {"video", "vsync", K_BOOL, PO(vsync), 0, 1, nullptr, "wait for the vertical blank when presenting"},
    {"audio", "sound", K_BOOL, PO(sound), 0, 1, "MC_SOUND", "sound effects available (F1 toggles in the game)"},
    {"audio", "sound_volume", K_INT, PO(sound_volume), 0, 512, "MC_SOUND_VOLUME", "sample mix gain (256 = unity, default 96)"},
    {"audio", "music", K_STRING, PO(music), 0, 0, "MC_MUSIC", "opl (FM synthesis, default) | midi (Windows MIDI) | square | off"},
    {"audio", "music_volume", K_INT, PO(music_volume), 0, 1024, "MC_MUSIC_VOLUME", "OPL music gain (256 = default)"},
    {"game", "tick_hz", K_DOUBLE, PO(tick_hz), 1, 1000, "MC_TICK_HZ", "simulation ticks per second (25 = the pace the game was tuned for)"},
    {"game", "movie_dir", K_STRING, PO(movie_dir), 0, 0, "MC_MOVIE_DIR", "folder with the full-length FLI movies (the CD's CARPET/INTRO); empty = auto"},
    {"keys", "wasd", K_BOOL, PS(keys_wasd), 0, 1, nullptr,
     "flight: W / S / A / D = faster / slower / slide left / slide right, like the arrow keys (which keep working)"},
    {"keys", "book_tab", K_BOOL, PS(keys_book_tab), 0, 1, nullptr,
     "Tab opens and closes the spell book, like Enter (which keeps working)"},
    {"keys", "mouse_book_both", K_BOOL, PS(mouse_book_both), 0, 1, nullptr,
     "both mouse buttons at once open and close the spell book (the original); 0 = they only fire both hands"},
    {"keys", "mouse_invert_y", K_BOOL, PO(mouse_invert_y), 0, 1, nullptr,
     "flight: 1 = the mouse's up / down as the original, 0 = reversed (the spell book's pointer is not affected)"},
    {"keys", "quit_now", K_KEY, PO(quit_now), 0, 0, nullptr, "leave the port at once"},
    {"keys", "menu", K_KEY, PO(menu), 0, 0, nullptr,
     "in a level: open the port's pause menu (save / load / options / leave level); 'none' = the original's Esc (leave the level at once)"},
#define SLOT(n) \
    {"keys", "save" #n, K_KEY, PO(save_slot[n]), 0, 0, nullptr, n == 0 ? "save the state into the quick slot 0" : "save the state into slot " #n}, \
    {"keys", "load" #n, K_KEY, PO(load_slot[n]), 0, 0, nullptr, n == 0 ? "load the quick slot 0" : "load slot " #n}
    SLOT(0), SLOT(1), SLOT(2), SLOT(3), SLOT(4), SLOT(5), SLOT(6), SLOT(7), SLOT(8), SLOT(9),
#undef SLOT
    {"pad", "enabled", K_BOOL, PO(pad.enabled), 0, 1, nullptr, "use a game controller when one is connected"},
    {"pad", "deadzone_left", K_FLOAT, PO(pad.deadzone_left), 0, 0.9, nullptr, "left stick dead zone (fraction of full deflection)"},
    {"pad", "deadzone_right", K_FLOAT, PO(pad.deadzone_right), 0, 0.9, nullptr, "right stick dead zone"},
    {"pad", "stick_threshold", K_FLOAT, PO(pad.stick_threshold), 0.1, 0.95, nullptr, "left stick deflection that holds Up / Down / Left / Right"},
    {"pad", "trigger_threshold", K_FLOAT, PO(pad.trigger_threshold), 0.05, 0.95, nullptr, "trigger travel that counts as pressed"},
    {"pad", "steer_sensitivity", K_FLOAT, PO(pad.steer_sensitivity), 0.1, 4, nullptr, "right stick steering strength"},
    {"pad", "steer_curve", K_FLOAT, PO(pad.steer_curve), 1, 3, nullptr, "right stick response exponent (1 = linear)"},
    {"pad", "invert_y", K_BOOL, PO(pad.invert_y), 0, 1, nullptr, "right stick up = pointer down"},
    {"pad", "cursor_speed", K_FLOAT, PO(pad.cursor_speed), 50, 5000, nullptr, "spell book / menu pointer speed (640-space pixels per second)"},
#define BIND(i) {"pad", "bind_" #i, K_PAD, PO(pad.bind[PAD_##i]), 0, 0, nullptr, nullptr}
    {"pad", "bind_A", K_PAD, PO(pad.bind[PAD_A]), 0, 0, nullptr, "flight bindings of the buttons (a game key, or an action: see the top)"},
    BIND(B), BIND(X), BIND(Y), BIND(BACK), BIND(GUIDE), BIND(START), BIND(LSTICK), BIND(RSTICK),
    BIND(LB), BIND(RB), BIND(DPAD_UP), BIND(DPAD_DOWN), BIND(DPAD_LEFT), BIND(DPAD_RIGHT), BIND(LT), BIND(RT),
#undef BIND
};
#undef PS
#undef PO

std::string lower(std::string s) { for (char &c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string full_name(const Desc &d) { return std::string(d.section) + "." + lower(d.key); }

const Desc *find_desc(const std::string &name) {
    const std::string n = lower(trim(name));
    for (const Desc &d : k_desc) if (full_name(d) == n) return &d;
    return nullptr;
}

std::string fmt_double(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

bool parse_bool(const std::string &v, bool *out) {
    if (v == "1" || ieq(v.c_str(), "true") || ieq(v.c_str(), "yes") || ieq(v.c_str(), "on")) { *out = true; return true; }
    if (v == "0" || ieq(v.c_str(), "false") || ieq(v.c_str(), "no") || ieq(v.c_str(), "off")) { *out = false; return true; }
    return false;
}

std::string value_of(const Desc &d, const PortSettings &cs, const PlatformOptions &cp) {
    PortSettings &s = const_cast<PortSettings &>(cs);
    PlatformOptions &p = const_cast<PlatformOptions &>(cp);
    void *v = d.get(s, p);
    switch (d.kind) {
    case K_BOOL: return *static_cast<bool *>(v) ? "1" : "0";
    case K_INT: return std::to_string(*static_cast<int *>(v));
    case K_FLOAT: return fmt_double(*static_cast<float *>(v));
    case K_DOUBLE: return fmt_double(*static_cast<double *>(v));
    case K_STRING: return *static_cast<std::string *>(v);
    case K_KEY: return config_key_name(*static_cast<KeyChord *>(v));
    case K_PAD: return config_pad_action_name(*static_cast<PadAction *>(v));
    }
    return "";
}
} // namespace

std::vector<std::string> config_keys() {
    std::vector<std::string> r;
    for (const Desc &d : k_desc) r.push_back(full_name(d));
    return r;
}

std::string config_get(const char *key, const PortSettings &s, const PlatformOptions &p) {
    const Desc *d = find_desc(key ? key : "");
    return d ? value_of(*d, s, p) : std::string();
}

bool config_apply(const char *key, const char *value, PortSettings *s, PlatformOptions *p, std::string *warning) {
    const Desc *d = find_desc(key ? key : "");
    std::string v = trim(value ? value : "");
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    auto warn = [&](const std::string &w) { if (warning) *warning = w; };
    if (!d) { warn(std::string("unknown key '") + (key ? key : "") + "'"); return false; }
    void *dst = d->get(*s, *p);
    const std::string name = full_name(*d);
    switch (d->kind) {
    case K_BOOL: {
        bool b;
        if (!parse_bool(v, &b)) { warn(name + ": '" + v + "' is not 0 / 1"); return false; }
        *static_cast<bool *>(dst) = b;
        return true;
    }
    case K_INT: case K_FLOAT: case K_DOUBLE: {
        char *end = nullptr;
        double x = std::strtod(v.c_str(), &end);
        if (v.empty() || (end && *end) || std::isnan(x)) { warn(name + ": '" + v + "' is not a number"); return false; }
        if (x < d->lo || x > d->hi) {
            const double c = x < d->lo ? d->lo : d->hi;
            warn(name + ": " + v + " is outside " + fmt_double(d->lo) + ".." + fmt_double(d->hi) + ", using " + fmt_double(c));
            x = c;
        }
        if (d->kind == K_INT) *static_cast<int *>(dst) = (int)std::lround(x);
        else if (d->kind == K_FLOAT) *static_cast<float *>(dst) = (float)x;
        else *static_cast<double *>(dst) = x;
        return true;
    }
    case K_STRING:
        if (name == "audio.music") {
            std::string m = lower(v);
            if (m == "0") m = "off";
            if (m != "opl" && m != "midi" && m != "square" && m != "off") { warn(name + ": '" + v + "' is not opl / midi / square / off"); return false; }
            v = m;
        }
        *static_cast<std::string *>(dst) = v;
        return true;
    case K_KEY: {
        KeyChord k;
        if (!config_parse_key(v.c_str(), &k)) { warn(name + ": '" + v + "' is not a key"); return false; }
        *static_cast<KeyChord *>(dst) = k;
        return true;
    }
    case K_PAD: {
        PadAction a;
        if (!config_parse_pad_action(v.c_str(), &a)) { warn(name + ": '" + v + "' is not a pad action"); return false; }
        *static_cast<PadAction *>(dst) = a;
        return true;
    }
    }
    return false;
}

// ---- the file ------------------------------------------------------------------------------------------
bool config_read_file(const char *path, PortSettings *s, PlatformOptions *p, std::vector<std::string> *warnings,
                      std::vector<std::string> *missing_keys) {
    std::ifstream in(path);
    if (!in) return false;
    std::set<std::string> seen;
    std::string line, section;
    int ln = 0;
    while (std::getline(in, line)) {
        ln++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t[0] == ';') continue;
        if (t[0] == '#') {
            // "# key = value": a default left commented out still counts as present (not appended again)
            const std::string c = trim(t.substr(1));
            const size_t ceq = c.find('=');
            if (ceq != std::string::npos) {
                const std::string k = lower(trim(c.substr(0, ceq)));
                const std::string f = k.find('.') == std::string::npos ? section + "." + k : k;
                if (find_desc(f)) seen.insert(f);
            }
            continue;
        }
        if (t[0] == '[') {
            const size_t e = t.find(']');
            section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
            continue;
        }
        const size_t eq = t.find('=');
        auto where = [&] { return std::string(path) + ":" + std::to_string(ln) + ": "; };
        if (eq == std::string::npos) { if (warnings) warnings->push_back(where() + "not 'key = value', ignored"); continue; }
        std::string key = lower(trim(t.substr(0, eq)));
        std::string value = t.substr(eq + 1);
        const size_t hash = value.find(" #");                  // inline comment
        if (hash != std::string::npos) value = value.substr(0, hash);
        const std::string full = key.find('.') == std::string::npos ? section + "." + key : key;
        std::string w;
        if (!find_desc(full)) {
            if (warnings) warnings->push_back(where() + "unknown key '" + full + "' (kept in the file, ignored)");
            continue;
        }
        seen.insert(full);
        if (!config_apply(full.c_str(), value.c_str(), s, p, &w) || !w.empty())
            if (warnings) warnings->push_back(where() + w);
    }
    if (missing_keys) {
        missing_keys->clear();
        for (const Desc &d : k_desc) if (!seen.count(full_name(d))) missing_keys->push_back(full_name(d));
    }
    return true;
}

static void write_key(std::ostream &o, const Desc &d, const PortSettings &s, const PlatformOptions &p, bool commented) {
    std::string c = d.help ? d.help : "";
    if (d.kind == K_INT || d.kind == K_FLOAT || d.kind == K_DOUBLE)
        c += (c.empty() ? "" : " ") + std::string("(") + fmt_double(d.lo) + ".." + fmt_double(d.hi) + ")";
    if (d.env) c += std::string(" [env ") + d.env + "]";
    if (!c.empty()) o << "## " << c << "\n";
    o << (commented ? "# " : "") << lower(d.key) << " = " << value_of(d, s, p) << "\n";
}

static const char *k_header =
    "## Magic Carpet native port (mcport) settings, created on the first run with every key commented out at\n"
    "## its default ('# key = default'): remove the '# ' to change a value. Commented keys follow the defaults\n"
    "## of later versions.\n"
    "## Precedence: this file < environment variables (MC_TICK_HZ, MC_SOUND, ...) < command line\n"
    "## (mcport [game dir] [mode] --set section.key=value ... --faithful). Unknown keys are kept and ignored.\n"
    "## --faithful (or MC_FAITHFUL=1) runs every [render] [display] [game] [pacing] setting as the original\n"
    "## carpet.exe behaves: 40x21-cell view, 320x200 / 640x480 frame, 1000-slot Thing pool, 25 Hz motion.\n"
    "## Keys: F1..F12, A..Z, 0..9, Esc, Enter, Space, Tab, Up, Down, Left, Right, Home, End, PageUp, ..., with\n"
    "## Ctrl+ / Shift+ / Alt+ in front; 'none' unbinds. Pad actions: cast_left, cast_right, spell_next,\n"
    "## spell_prev, spell_next_right, spell_prev_right, save_quick, load_quick, none, or a game key (Enter, Ctrl+1).\n";

bool config_write_default_file(const char *path) {
    PortSettings s;
    config_playing_defaults(&s);
    return config_write_file(path, s, PlatformOptions{}, true);
}

bool config_write_file(const char *path, const PortSettings &s, const PlatformOptions &p, bool commented) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    o << k_header;
    std::string section;
    for (const Desc &d : k_desc) {
        if (section != d.section) {
            section = d.section;
            o << "\n[" << section << "]\n";
            if (section == "pad")
                o << "## Controller (SDL game controller layout: a b x y back guide start lstick rstick lb rb dpad_* lt rt).\n"
                     "## Flight: right stick steers (the mouse pointer's offset), left stick = Up / Down / Left / Right.\n"
                     "## Spell book / menus: sticks / d-pad move the pointer, a = left click, x = right click, b = back.\n";
        }
        write_key(o, d, s, p, commented);
    }
    return (bool)o;
}

bool config_append_keys(const char *path, const std::vector<std::string> &keys) {
    if (keys.empty()) return true;
    PortSettings s;
    config_playing_defaults(&s);
    PlatformOptions p;
    std::ofstream o(path, std::ios::binary | std::ios::app);
    if (!o) return false;
    o << "\n## ---- keys added by a newer mcport (commented out at their defaults) ----\n";
    for (const std::string &k : keys) {
        const Desc *d = find_desc(k);
        if (!d) continue;
        if (d->help) o << "## " << d->help << "\n";
        o << "# " << full_name(*d) << " = " << value_of(*d, s, p) << "\n";
    }
    return (bool)o;
}

bool config_set_keys(const char *path, const std::vector<std::pair<std::string, std::string>> &values) {
    if (values.empty()) return true;
    {
        std::ifstream probe(path);
        if (!probe && !config_write_default_file(path)) return false;
    }
    std::vector<std::string> lines;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    std::vector<std::string> names;
    for (const auto &kv : values) names.push_back(lower(trim(kv.first)));
    std::vector<bool> done(values.size(), false);
    std::string section;
    for (std::string &line : lines) {
        std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == '[') {
            const size_t e = t.find(']');
            section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
            continue;
        }
        if (t[0] == ';' || (t[0] == '#' && t.size() > 1 && t[1] == '#')) continue;   // "##" = help text
        if (t[0] == '#') t = trim(t.substr(1));
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = lower(trim(t.substr(0, eq)));
        const std::string f = k.find('.') == std::string::npos ? section + "." + k : k;
        for (size_t i = 0; i < values.size(); i++) {
            if (names[i] != f) continue;
            line = k + " = " + values[i].second;     // the line's own spelling of the key (short or full)
            done[i] = true;                          // later duplicates are rewritten as well
        }
    }
    bool header = false;
    for (size_t i = 0; i < values.size(); i++) {
        if (done[i]) continue;
        if (!header) { lines.push_back(""); lines.push_back("## ---- set from the in-game menu ----"); header = true; }
        lines.push_back(names[i] + " = " + values[i].second);   // full "section.key" names read anywhere
    }
    const std::string tmp = std::string(path) + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary);
        if (!o) return false;
        for (const std::string &l : lines) o << l << "\n";
        if (!o) return false;
    }
    std::remove(path);
    return std::rename(tmp.c_str(), path) == 0;
}

// ---- environment ---------------------------------------------------------------------------------------
void config_apply_env(PortSettings *s, PlatformOptions *p, std::vector<std::string> *warnings) {
    for (const Desc &d : k_desc) {
        if (!d.env) continue;
        const char *e = std::getenv(d.env);
        if (!e || !*e) continue;
        std::string w;
        if (!config_apply(full_name(d).c_str(), e, s, p, &w) || !w.empty())
            if (warnings) warnings->push_back(std::string(d.env) + ": " + w);
    }
    if (const char *f = std::getenv("MC_FAITHFUL"); f && std::strcmp(f, "1") == 0) {
        *s = PortSettings{};
        p->faithful = true;
    }
}

static void set_env(const char *name, const std::string &v) {
#ifdef _WIN32
    _putenv_s(name, v.c_str());
#else
    setenv(name, v.c_str(), 1);
#endif
}

void config_export_env(const PlatformOptions &p) {
    set_env("MC_SOUND", p.sound ? "1" : "0");
    set_env("MC_SOUND_VOLUME", std::to_string(p.sound_volume));
    set_env("MC_MUSIC", p.music == "off" ? "0" : p.music);
    set_env("MC_MUSIC_VOLUME", std::to_string(p.music_volume));
    set_env("MC_TICK_HZ", fmt_double(p.tick_hz));
    if (!p.movie_dir.empty()) set_env("MC_MOVIE_DIR", p.movie_dir);
}

// ---- everything ----------------------------------------------------------------------------------------
bool config_load(const char *save_dir, int *argc, char **argv, PortSettings *s, PlatformOptions *p) {
    config_playing_defaults(s);
    *p = PlatformOptions{};
    bool ok = true;

    // Command line, pass 1: which file.
    std::string ini;
    if (save_dir && *save_dir) {
        ini = save_dir;
        if (ini.back() != '/' && ini.back() != '\\') ini += '/';
        ini += "mcport.ini";
    }
    for (int i = 1; i < *argc; i++)
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < *argc) ini = argv[i + 1];

    // 2. the file (created when missing; keys of a newer version appended)
    if (!ini.empty()) {
        std::vector<std::string> missing;
        if (config_read_file(ini.c_str(), s, p, &p->warnings, &missing)) {
            p->ini_path = ini;
            if (!missing.empty() && !config_append_keys(ini.c_str(), missing))
                p->warnings.push_back(ini + ": could not add the new keys");
        } else if (config_write_default_file(ini.c_str())) {
            p->ini_path = ini;
            std::printf("config: wrote the default settings to %s\n", ini.c_str());
        } else {
            p->warnings.push_back(ini + ": cannot be read or created; using the defaults");
        }
    }
    // 3. environment
    config_apply_env(s, p, &p->warnings);
    // 4. command line, left to right; recognised options are removed from argv
    int out = 1;
    bool run_options = false;       // after a run mode with its own options (`rts`): unknown --x are its options
    for (int i = 1; i < *argc; i++) {
        const char *a = argv[i];
        if (std::strcmp(a, "--faithful") == 0) { *s = PortSettings{}; p->faithful = true; continue; }
        if (std::strcmp(a, "--config") == 0) { i++; continue; }
        const char *kv = nullptr;
        if (std::strcmp(a, "--set") == 0) {
            if (i + 1 >= *argc) { p->warnings.push_back("--set needs section.key=value"); ok = false; continue; }
            kv = argv[++i];
        } else if (std::strncmp(a, "--set=", 6) == 0) kv = a + 6;
        if (kv) {
            const char *eq = std::strchr(kv, '=');
            std::string w;
            if (!eq) { p->warnings.push_back(std::string("--set ") + kv + ": needs section.key=value"); ok = false; continue; }
            if (!config_apply(std::string(kv, eq).c_str(), eq + 1, s, p, &w) || !w.empty())
                p->warnings.push_back(std::string("--set ") + kv + ": " + w);
            continue;
        }
        if (a[0] == '-' && a[1] == '-' && !run_options) { p->warnings.push_back(std::string("unknown option ") + a); ok = false; continue; }
        if (std::strcmp(a, "rts") == 0) run_options = true;
        argv[out++] = argv[i];                                  // positional (and the old "-network")
    }
    *argc = out;
    argv[out] = nullptr;
    for (const std::string &w : p->warnings) std::fprintf(stderr, "config: %s\n", w.c_str());
    config_export_env(*p);
    return ok;
}

PortKeyAction config_key_action(const PlatformOptions &p, int sc, uint8_t mods, int *slot) {
    const KeyChord k{sc, (uint8_t)(mods & (KEYMOD_CTRL | KEYMOD_SHIFT | KEYMOD_ALT))};
    if (!sc) return PORT_KEY_NONE;
    for (int i = 0; i < 10; i++) {
        if (p.save_slot[i] == k) { if (slot) *slot = i; return PORT_KEY_SAVE; }
        if (p.load_slot[i] == k) { if (slot) *slot = i; return PORT_KEY_LOAD; }
    }
    if (p.quit_now == k) return PORT_KEY_QUIT;
    if (p.menu == k) return PORT_KEY_MENU;
    return PORT_KEY_NONE;
}

std::string config_summary(const PortSettings &s) {
    PlatformOptions p;
    std::string r;
    for (const Desc &d : k_desc) {
        if (d.get == nullptr) continue;
        const std::string sec = d.section;
        if (sec != "render" && sec != "display" && sec != "game" && sec != "pacing") continue;
        if (sec == "game" && std::strcmp(d.key, "tick_hz") == 0) continue;
        if (sec == "game" && std::strcmp(d.key, "movie_dir") == 0) continue;
        r += (r.empty() ? "" : " ") + full_name(d) + "=" + value_of(d, s, p);
    }
    return r;
}
