// In-level pause menu (port round 9): see game_menu.h.
#include "game_menu.h"
#include "savegame.h"
#include "settings.h"
#include "ui_draw.h"
#include "world_set.h"
#include "world.h"
#include <SDL_scancode.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

// One options row: a config key (config.h) shown as `label`. `choices` = "value:Text|value:Text|..." (Enter
// / Right step forward, Left back, both wrap), or null for a number lo..hi in `step`s with `suffix`.
struct Opt {
    const char *label;
    const char *key;
    const char *choices;
    int lo, hi, step;
    const char *suffix;
    const char *note;            // shown after the value (when it does not apply at once)
};

const Opt k_opts[] = {
    {"Renderer", "render.extended", "0:Original|1:Extended", 0, 0, 0, nullptr, nullptr},
    {"Draw distance", "render.draw_distance", nullptr, 8, 255, 8, " cells", nullptr},   // option_hi: 127 at 256 cells
    {"Fog starts at", "render.fog_start_pct", nullptr, 0, 100, 5, " %", nullptr},
    {"Far detail", "render.lod", "0:Full|1:Quality|2:Fast", 0, 0, 0, nullptr, nullptr},
    {"Fog", "render.fog_mode", "0:Auto|1:Darken|2:Haze", 0, 0, 0, nullptr, nullptr},
    {"HUD scaling", "display.hud_scale_mode", "0:Integer|1:Fit|2:Fit, filtered", 0, 0, 0, nullptr, nullptr},
    {"HUD in the corners", "display.hud_corners", "0:Off|1:On", 0, 0, 0, nullptr, nullptr},
    {"Radar zoom", "display.radar_zoom_pct", nullptr, 50, 200, 10, " %", nullptr},
    {"Fullscreen", "video.fullscreen", "0:Off|1:On", 0, 0, 0, nullptr, nullptr},
    {"Smooth motion", "pacing.interpolate", "0:Off|1:On", 0, 0, 0, nullptr, nullptr},
    {"Frame rate cap", "pacing.fps_cap", "0:None|30:30|60:60|75:75|120:120|144:144|165:165|240:240", 0, 0, 0, nullptr, nullptr},
    {"Possession range", "game.possession_range_pct", nullptr, 100, 200, 10, " %", nullptr},
    {"Thing pool", "game.thing_slots", "1000:1000 (original)|2000:2000|4000:4000|8192:8192|16384:16384|32768:32768", 0, 0, 0, nullptr, "next level"},
    {"W A S D flight", "keys.wasd", "0:Off|1:On", 0, 0, 0, nullptr, nullptr},
    {"Tab opens the book", "keys.book_tab", "0:Off|1:On", 0, 0, 0, nullptr, nullptr},
};
constexpr int k_opt_count = (int)(sizeof k_opts / sizeof k_opts[0]);

struct Choice { int value; std::string text; };
std::vector<Choice> parse_choices(const char *c) {
    std::vector<Choice> out;
    while (c && *c) {
        const char *bar = std::strchr(c, '|');
        const std::string item = bar ? std::string(c, bar) : std::string(c);
        const size_t colon = item.find(':');
        out.push_back({std::atoi(item.substr(0, colon).c_str()), item.substr(colon + 1)});
        c = bar ? bar + 1 : nullptr;
    }
    return out;
}

// The upper end of a number option. Draw distance: 127 in a 256-cell world (the campaign, Hidden Worlds - the map
// wraps at 128 cells each way), 255 in a wider one (a generated Conquest world; render_ext clamps the same way).
int option_hi(const Opt &o) {
    if (std::strcmp(o.key, "render.draw_distance") == 0) return world_wide() ? 255 : 127;
    return o.hi;
}

enum Action { A_NONE, A_RESUME, A_SAVE_PAGE, A_LOAD_PAGE, A_OPTIONS_PAGE, A_LEAVE, A_QUIT, A_BACK, A_SLOT, A_OPTION };

}  // namespace

struct GameMenu::Row {
    std::string label, value;
    bool enabled = true;
    Action action = A_NONE;
    int arg = 0;                 // slot / option index
};

void GameMenu::open(const GameMenuEnv &env, PortSettings *s, PlatformOptions *p) {
    open_ = true;
    env_ = env;
    s_ = s;
    p_ = p;
    changed_.clear();
    message_.clear();
    go(PAGE_MAIN);
}

std::vector<std::pair<std::string, std::string>> GameMenu::close() {
    open_ = false;
    std::vector<std::pair<std::string, std::string>> out;
    out.swap(changed_);
    return out;
}

void GameMenu::go(Page p) {
    page_ = p;
    sel_ = 0;
    scroll_ = 0;
    confirm_slot_ = -1;
    if (p == PAGE_SAVE || p == PAGE_LOAD) refresh_slots();
}

void GameMenu::refresh_slots() {
    for (int i = 0; i < 10; i++) {
        SlotInfo &si = slots_[i];
        si = SlotInfo{};
        char path[1024];
        SaveStateHeader h{};
        if (!savestate_slot_path(i, path, sizeof path) || !savestate_read_header(path, &h)) continue;
        si.used = true;
        si.level = h.level;
        si.tick = h.tick;
        si.time = h.time;
        si.name = h.name;
    }
}

std::string GameMenu::option_value(int opt) const {
    const Opt &o = k_opts[opt];
    const std::string v = config_get(o.key, *s_, *p_);
    std::string text;
    if (o.choices) {
        const int cur = std::atoi(v.c_str());
        text = v;
        for (const Choice &c : parse_choices(o.choices)) if (c.value == cur) text = c.text;
    } else {
        text = v + (o.suffix ? o.suffix : "");
    }
    if (std::strcmp(o.key, "game.possession_range_pct") == 0 && gameplay_rules_forced()) {
        char b[48];
        std::snprintf(b, sizeof b, " (this level %d %%)", gameplay_rules().possession_range_pct);
        text += b;
    }
    if (std::strcmp(o.key, "render.draw_distance") == 0 && std::atoi(v.c_str()) > option_hi(o)) {
        char b[48];
        std::snprintf(b, sizeof b, " (%d in this world)", option_hi(o));
        text += b;
    }
    if (o.note) text += std::string(" (") + o.note + ")";
    return text;
}

void GameMenu::step_option(int opt, int dir) {
    const Opt &o = k_opts[opt];
    const int cur = std::atoi(config_get(o.key, *s_, *p_).c_str());
    int next = cur;
    if (o.choices) {
        const std::vector<Choice> cs = parse_choices(o.choices);
        const int m = (int)cs.size();
        int idx = -1;
        for (int i = 0; i < m; i++) if (cs[i].value == cur) idx = i;
        if (idx >= 0) {
            idx = (idx + (dir == 0 ? 1 : dir) + m) % m;
        } else if (dir >= 0) {                                                         // a value from the ini
            idx = 0;                                                                   // between two: the next up
            while (idx < m && cs[idx].value < cur) idx++;
            if (idx == m) idx = 0;
        } else {
            idx = m - 1;
            while (idx >= 0 && cs[idx].value > cur) idx--;
            if (idx < 0) idx = m - 1;
        }
        next = cs[idx].value;
    } else {
        const int hi = option_hi(o);
        next = cur + (dir == 0 ? 1 : dir) * o.step;
        if (dir == 0 && cur >= hi) next = o.lo;                                       // Enter wraps
        next = std::max(o.lo, std::min(hi, next));
    }
    if (next == cur) return;
    const std::string v = std::to_string(next);
    std::string warning;
    if (!config_apply(o.key, v.c_str(), s_, p_, &warning)) return;
    const std::string now = config_get(o.key, *s_, *p_);
    for (auto &kv : changed_) if (kv.first == o.key) { kv.second = now; return; }
    changed_.push_back({o.key, now});
}

std::vector<GameMenu::Row> GameMenu::rows() const {
    std::vector<Row> r;
    auto add = [&](const std::string &label, Action a, int arg = 0, bool en = true, const std::string &value = "") {
        Row row;
        row.label = label; row.action = a; row.arg = arg; row.enabled = en; row.value = value;
        r.push_back(row);
    };
    const bool states = !env_.network && env_.can_save;
    switch (page_) {
    case PAGE_MAIN:
        add("Resume", A_RESUME);
        add("Save state", A_SAVE_PAGE, 0, states, env_.network ? "not in a network game" : "");
        add("Load state", A_LOAD_PAGE, 0, states);
        add("Options", A_OPTIONS_PAGE);
        add(env_.network ? "Leave the game" : "Leave level", A_LEAVE);
        add("Quit Magic Carpet", A_QUIT);
        break;
    case PAGE_SAVE:
    case PAGE_LOAD:
        for (int k = 0; k < 10; k++) {
            const int slot = k == 9 ? 0 : k + 1;          // 1..9, then the quick slot 0
            const SlotInfo &si = slots_[slot];
            char label[32], value[96];
            std::snprintf(label, sizeof label, slot == 0 ? "Quick slot 0" : "Slot %d", slot);
            if (!si.used) std::snprintf(value, sizeof value, "empty");
            else {
                char when[32] = "";
                const std::time_t t = (std::time_t)si.time;
                std::tm tmv{};
#ifdef _WIN32
                if (localtime_s(&tmv, &t) == 0)
#else
                if (localtime_r(&t, &tmv))
#endif
                    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tmv);
                if (world_is_hidden_level(si.level))
                    std::snprintf(value, sizeof value, "hidden %d  tick %u  %s", si.level - HW_LEVEL_BASE + 1, si.tick, when);
                else
                    std::snprintf(value, sizeof value, "level %d  tick %u  %s", si.level + 1, si.tick, when);
            }
            std::string v = value;
            if (page_ == PAGE_SAVE && confirm_slot_ == slot) v = "Enter again to overwrite";
            add(label, A_SLOT, slot, page_ == PAGE_SAVE || si.used, v);
        }
        add("Back", A_BACK);
        break;
    case PAGE_OPTIONS:
        for (int i = 0; i < k_opt_count; i++) {
            const bool forced = std::strcmp(k_opts[i].key, "game.possession_range_pct") == 0 && env_.network;
            add(k_opts[i].label, A_OPTION, i, !forced, option_value(i));
        }
        add("Back", A_BACK);
        break;
    }
    return r;
}

int GameMenu::row_count() const { return (int)rows().size(); }

std::string GameMenu::row_text(int i) const {
    if (i < 0) {
        switch (page_) {
        case PAGE_SAVE: return "Save state";
        case PAGE_LOAD: return "Load state";
        case PAGE_OPTIONS: return "Options";
        default: return env_.network ? "Menu (the game goes on)" : "Paused";
        }
    }
    const std::vector<Row> r = rows();
    if (i >= (int)r.size()) return "";
    return r[(size_t)i].value.empty() ? r[(size_t)i].label : r[(size_t)i].label + ": " + r[(size_t)i].value;
}

GameMenuResult GameMenu::activate(int i, int dir) {
    GameMenuResult res;
    const std::vector<Row> r = rows();
    if (i < 0 || i >= (int)r.size() || !r[(size_t)i].enabled) return res;
    const Row &row = r[(size_t)i];
    if (dir != 0 && row.action != A_OPTION) return res;
    switch (row.action) {
    case A_RESUME: res.cmd = GameMenuCmd::RESUME; break;
    case A_SAVE_PAGE: go(PAGE_SAVE); break;
    case A_LOAD_PAGE: go(PAGE_LOAD); break;
    case A_OPTIONS_PAGE: go(PAGE_OPTIONS); break;
    case A_LEAVE: res.cmd = GameMenuCmd::LEAVE_LEVEL; break;
    case A_QUIT: res.cmd = GameMenuCmd::QUIT; break;
    case A_BACK: { const Page from = page_; go(PAGE_MAIN); sel_ = from == PAGE_SAVE ? 1 : from == PAGE_LOAD ? 2 : 3; break; }
    case A_SLOT:
        if (page_ == PAGE_SAVE) {
            if (slots_[row.arg].used && confirm_slot_ != row.arg) { confirm_slot_ = row.arg; break; }
            res.cmd = GameMenuCmd::SAVE;
        } else {
            res.cmd = GameMenuCmd::LOAD;
        }
        res.slot = row.arg;
        confirm_slot_ = -1;
        break;
    case A_OPTION:
        step_option(row.arg, dir);
        if (std::strcmp(k_opts[row.arg].key, "video.fullscreen") == 0) {
            res.cmd = GameMenuCmd::FULLSCREEN;
            res.on = p_->fullscreen != 0;
        }
        break;
    default: break;
    }
    return res;
}

GameMenuResult GameMenu::key(int sc) {
    GameMenuResult res;
    if (!open_) return res;
    message_.clear();
    const std::vector<Row> r = rows();
    const int n = (int)r.size();
    auto move = [&](int d, int count) {
        for (int k = 0; k < count; k++) {
            int i = sel_;
            for (int guard = 0; guard < n; guard++) {
                i = (i + d + n) % n;
                if (r[(size_t)i].enabled) break;
            }
            sel_ = i;
        }
        if (sel_ != confirm_slot_) confirm_slot_ = -1;
    };
    switch (sc) {
    case SDL_SCANCODE_UP: move(-1, 1); break;
    case SDL_SCANCODE_DOWN: move(+1, 1); break;
    case SDL_SCANCODE_PAGEUP: move(-1, 5); break;
    case SDL_SCANCODE_PAGEDOWN: move(+1, 5); break;
    case SDL_SCANCODE_HOME: sel_ = 0; break;
    case SDL_SCANCODE_END: sel_ = n - 1; break;
    case SDL_SCANCODE_LEFT: res = activate(sel_, -1); break;
    case SDL_SCANCODE_RIGHT: res = activate(sel_, +1); break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: res = activate(sel_, 0); break;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE:
        if (page_ == PAGE_MAIN) res.cmd = GameMenuCmd::RESUME;
        else { for (int i = 0; i < n; i++) if (r[(size_t)i].action == A_BACK) res = activate(i, 0); }
        break;
    default: break;
    }
    return res;
}

// Panel geometry in the 640-wide virtual screen.
void GameMenu::layout(int vh, int *x, int *y, int *w, int *row_h, int *visible) const {
    ui_set_font(1);
    const int lh = ui_font_line_height();
    *w = 480;
    *x = (640 - *w) / 2;
    *row_h = lh + 6;
    const int n = row_count();
    const int max_rows = std::max(3, (vh - 4 * *row_h - 40) / *row_h);
    *visible = std::min(n, max_rows);
    const int h = (*visible + 4) * *row_h;                      // title, gap, rows, message line, margin
    *y = std::max(8, (vh - h) / 2);
}

GameMenuResult GameMenu::pointer(int x, int y, bool click, bool back, int vh) {
    GameMenuResult res;
    if (!open_) return res;
    if (back) return key(SDL_SCANCODE_ESCAPE);
    int px, py, w, rh, vis;
    layout(vh, &px, &py, &w, &rh, &vis);
    const int top = py + 2 * rh;
    if (x < px || x >= px + w || y < top || y >= top + vis * rh) return res;
    const int i = scroll_ + (y - top) / rh;
    const std::vector<Row> r = rows();
    if (i >= (int)r.size() || !r[(size_t)i].enabled) return res;
    sel_ = i;
    if (!click) return res;
    message_.clear();
    if (r[(size_t)i].action == A_OPTION) {
        const int value_x = px + w / 2;
        if (x >= value_x) return activate(i, x < value_x + (w / 2) / 2 ? -1 : +1);
    }
    return activate(i, 0);
}

void GameMenu::wheel(int dy) {
    if (!open_ || dy == 0) return;
    key(dy > 0 ? SDL_SCANCODE_UP : SDL_SCANCODE_DOWN);
}

void GameMenu::draw(int vh, int ptr_x, int ptr_y) {
    if (!open_) return;
    int x, y, w, rh, vis;
    layout(vh, &x, &y, &w, &rh, &vis);
    const std::vector<Row> r = rows();
    const int n = (int)r.size();
    if (sel_ >= n) sel_ = n - 1;
    if (sel_ < scroll_) scroll_ = sel_;
    if (sel_ >= scroll_ + vis) scroll_ = sel_ - vis + 1;
    const int h = (vis + 4) * rh;
    const uint8_t white = ui_col_white(), grey = ui_colour(8, 8, 9), gold = ui_colour(15, 12, 3);
    const uint8_t dim = ui_colour(10, 10, 11), bar = ui_colour(3, 2, 7), edge = ui_colour(9, 7, 2);

    // shade rows (ui_shade_rect): 0x20 = unchanged, above darker (0x2c ~ 65 %, 0x34 ~ 35 %), below = fog
    ui_shade_rect(0, 0, 640, vh, 0x2c);                         // the game behind, darker
    ui_shade_rect(x, y, w, h, 0x34);
    vga_draw_box(x, y, w, h, edge);
    const int pad = 12;
    const std::string title = row_text(-1);
    ui_draw_text(title.c_str(), x + (w - ui_text_width(title.c_str())) / 2, y + rh / 2, gold);
    const int top = y + 2 * rh;
    for (int k = 0; k < vis && scroll_ + k < n; k++) {
        const int i = scroll_ + k;
        const Row &row = r[(size_t)i];
        const int ry = top + k * rh;
        const bool sel = i == sel_;
        if (sel) gfx_fill_rect(x + 2, ry, w - 4, rh, bar);
        const uint8_t c = !row.enabled ? grey : sel ? gold : white;
        ui_draw_text(row.label.c_str(), x + pad, ry + 3, c);
        if (!row.value.empty()) {
            std::string v = row.value;
            if (row.action == A_OPTION && sel) v = "< " + v + " >";
            const int vw = ui_text_width(v.c_str());
            ui_draw_text(v.c_str(), x + w - pad - vw, ry + 3, row.enabled ? (sel ? gold : dim) : grey);
        }
    }
    if (scroll_ > 0) ui_draw_text("^", x + w - pad, top - rh + 3, dim);
    if (scroll_ + vis < n) ui_draw_text("v", x + w - pad, top + vis * rh, dim);
    const std::string foot = !message_.empty() ? message_
                           : page_ == PAGE_OPTIONS ? "Left / Right change - saved to mcport.ini on leaving the menu"
                           : "Enter select - Esc back";
    ui_draw_text(foot.c_str(), x + (w - ui_text_width(foot.c_str())) / 2, top + vis * rh + rh / 2, !message_.empty() ? gold : grey);
    if (ptr_x >= 0) {
        const UiSprite *s = ui_sprite(g_ui_pointers, 1);
        if (s && s->data && s->h) ui_draw_sprite(ptr_x, ptr_y, s);
    }
}
