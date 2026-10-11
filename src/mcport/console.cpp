// The in-game debug console (console.h; round 10 task B, docs/analysis/port_console.md).
#define _CRT_SECURE_NO_WARNINGS
#include "console.h"
#include "mclog.h"
#include "debug_cmd.h"
#include "player.h"
#include "ui_draw.h"
#include "mc_globals.h"
#include <SDL_scancode.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
constexpr size_t kHistoryMax = 500;         // lines kept in the history file
constexpr size_t kScrollMax = 1000;         // scrollback lines
constexpr int kLevelInfo = MCLOG_INFO;

std::mutex s_stdin_mutex;
std::deque<std::string> s_stdin_lines;
bool s_stdin_started = false;
bool s_stdin_eof = false;

bool env_on(const char *name) {
    const char *e = std::getenv(name);
    return e && *e && std::strcmp(e, "0") != 0;
}
} // namespace

char console_key_ascii(int sc, bool shift) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return (char)((shift ? 'A' : 'a') + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return shift ? "!@#$%^&*("[sc - SDL_SCANCODE_1] : (char)('1' + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return (char)('1' + (sc - SDL_SCANCODE_KP_1));
    switch (sc) {
    case SDL_SCANCODE_0: return shift ? ')' : '0';
    case SDL_SCANCODE_KP_0: return '0';
    case SDL_SCANCODE_SPACE: return ' ';
    case SDL_SCANCODE_MINUS: return shift ? '_' : '-';
    case SDL_SCANCODE_EQUALS: return shift ? '+' : '=';
    case SDL_SCANCODE_LEFTBRACKET: return shift ? '{' : '[';
    case SDL_SCANCODE_RIGHTBRACKET: return shift ? '}' : ']';
    case SDL_SCANCODE_BACKSLASH: return shift ? '|' : '\\';
    case SDL_SCANCODE_SEMICOLON: return shift ? ':' : ';';
    case SDL_SCANCODE_APOSTROPHE: return shift ? '"' : '\'';
    case SDL_SCANCODE_COMMA: return shift ? '<' : ',';
    case SDL_SCANCODE_PERIOD: return shift ? '>' : '.';
    case SDL_SCANCODE_SLASH: return shift ? '?' : '/';
    case SDL_SCANCODE_GRAVE: return shift ? '~' : '`';
    case SDL_SCANCODE_KP_DIVIDE: return '/';
    case SDL_SCANCODE_KP_MULTIPLY: return '*';
    case SDL_SCANCODE_KP_MINUS: return '-';
    case SDL_SCANCODE_KP_PLUS: return '+';
    case SDL_SCANCODE_KP_PERIOD: return '.';
    default: return 0;
    }
}

ConsoleOptions console_parse_args(int *argc, char **argv) {
    ConsoleOptions o;
    o.stdin_reader = env_on("MC_CONSOLE_STDIN");
    if (const char *e = std::getenv("MC_SCENARIO")) o.scenario = e;
    int w = 1;
    for (int i = 1; i < *argc; i++) {
        if (!std::strcmp(argv[i], "--console-stdin")) { o.stdin_reader = true; continue; }
        if (!std::strcmp(argv[i], "--scenario") && i + 1 < *argc) { o.scenario = argv[++i]; continue; }
        argv[w++] = argv[i];
    }
    *argc = w;
    argv[w] = nullptr;
    return o;
}

// ---- history -------------------------------------------------------------------------------------------------
void Console::init(const std::string &save_dir) {
    save_dir_ = save_dir;
    hist_.clear();
    hist_path_.clear();
    if (save_dir.empty()) return;
    hist_path_ = (std::filesystem::path(save_dir) / "console_history.txt").string();
    std::ifstream f(hist_path_);
    std::string l;
    while (std::getline(f, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
        if (!l.empty()) hist_.push_back(l);
    }
    f.close();
    if (hist_.size() > kHistoryMax) {           // keep the file short: rewrite with the last lines
        hist_.erase(hist_.begin(), hist_.end() - (long)kHistoryMax);
        std::ofstream o(hist_path_, std::ios::trunc);
        for (const std::string &h : hist_) o << h << "\n";
    }
}

void Console::add_history(const std::string &l) {
    if (l.empty() || (!hist_.empty() && hist_.back() == l)) return;
    hist_.push_back(l);
    if (hist_.size() > kHistoryMax * 2) hist_.erase(hist_.begin(), hist_.end() - (long)kHistoryMax);
    if (!hist_path_.empty()) {
        std::ofstream o(hist_path_, std::ios::app);
        o << l << "\n";
    }
}

// ---- open / keys ---------------------------------------------------------------------------------------------
void Console::open() {
    open_ = true;
    scroll_off_ = 0;
    pull_log();
}
void Console::close() {
    open_ = false;
    hist_pos_ = -1;
}

void Console::key(int sc, uint8_t mods) {
    if (!open_) return;
    const bool shift = (mods & KEYMOD_SHIFT) != 0, ctrl = (mods & KEYMOD_CTRL) != 0;
    const int n = (int)line_.size();
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: close(); return;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: {
        const std::string l = line_;
        line_.clear();
        cur_ = 0;
        hist_pos_ = -1;
        scroll_off_ = 0;
        add_history(l);
        execute(l);
        return;
    }
    case SDL_SCANCODE_BACKSPACE:
        if (cur_ > 0) { line_.erase((size_t)cur_ - 1, 1); cur_--; }
        return;
    case SDL_SCANCODE_DELETE:
        if (cur_ < n) line_.erase((size_t)cur_, 1);
        return;
    case SDL_SCANCODE_LEFT: if (cur_ > 0) cur_--; return;
    case SDL_SCANCODE_RIGHT: if (cur_ < n) cur_++; return;
    case SDL_SCANCODE_HOME: cur_ = 0; return;
    case SDL_SCANCODE_END: cur_ = n; return;
    case SDL_SCANCODE_UP:
        if (hist_.empty()) return;
        if (hist_pos_ < 0) { draft_ = line_; hist_pos_ = (int)hist_.size() - 1; }
        else if (hist_pos_ > 0) hist_pos_--;
        line_ = hist_[(size_t)hist_pos_];
        cur_ = (int)line_.size();
        return;
    case SDL_SCANCODE_DOWN:
        if (hist_pos_ < 0) return;
        if (hist_pos_ + 1 < (int)hist_.size()) { hist_pos_++; line_ = hist_[(size_t)hist_pos_]; }
        else { hist_pos_ = -1; line_ = draft_; }
        cur_ = (int)line_.size();
        return;
    case SDL_SCANCODE_PAGEUP: scroll_off_ += 8; return;
    case SDL_SCANCODE_PAGEDOWN: scroll_off_ = std::max(0, scroll_off_ - 8); return;
    case SDL_SCANCODE_TAB: {
        // complete the first word
        if (line_.find(' ') != std::string::npos) return;
        const std::vector<std::string> c = cmd_complete(line_);
        if (c.size() == 1) { line_ = c[0] + " "; cur_ = (int)line_.size(); }
        else if (c.size() > 1) {
            std::string all;
            for (const std::string &s : c) all += s + " ";
            print(all);
            // the common prefix
            std::string p = c[0];
            for (const std::string &s : c) while (s.compare(0, p.size(), p) != 0) p.pop_back();
            line_ = p;
            cur_ = (int)line_.size();
        }
        return;
    }
    default: break;
    }
    if (ctrl) {
        if (sc == SDL_SCANCODE_U || sc == SDL_SCANCODE_C) { line_.clear(); cur_ = 0; hist_pos_ = -1; }
        else if (sc == SDL_SCANCODE_L) { scroll_.clear(); scroll_off_ = 0; }
        return;
    }
    if (mods & KEYMOD_ALT) return;
    const char ch = console_key_ascii(sc, shift);
    if (ch && line_.size() < 200) {
        line_.insert(line_.begin() + cur_, ch);
        cur_++;
    }
}

// ---- executing -------------------------------------------------------------------------------------------------
void Console::print(const std::string &text, int level) {
    mclog_str(level, text.c_str());
    pull_log();
}

void Console::pull_log() {
    std::vector<MclogLine> lines;
    log_seq_ = mclog_since(log_seq_, &lines);
    for (const MclogLine &l : lines) {
        scroll_.push_back(Out{l.text, l.level});
        if (scroll_.size() > kScrollMax) scroll_.pop_front();
    }
}

std::vector<std::string> Console::scrollback_text() const {
    std::vector<std::string> r;
    for (const Out &o : scroll_) r.push_back(o.text);
    return r;
}

std::string Console::resolve_path(const std::string &p) const {
    std::error_code ec;
    std::vector<std::filesystem::path> cand = {p, p + ".scn"};
    if (!save_dir_.empty()) {
        cand.push_back(std::filesystem::path(save_dir_) / "scenarios" / p);
        cand.push_back(std::filesystem::path(save_dir_) / "scenarios" / (p + ".scn"));
    }
    for (const auto &c : cand)
        if (std::filesystem::is_regular_file(c, ec)) return c.string();
    return p;
}

void Console::execute(const std::string &l) {
    const ParsedCommand c = cmd_parse(l);
    if (c.kind == CmdKind::EMPTY) return;
    print("> " + c.source);
    switch (c.kind) {
    case CmdKind::INVALID: print(c.error, MCLOG_WARN); break;
    case CmdKind::TEXT: print(c.text); break;
    case CmdKind::QUERY: print(cmd_query(c)); break;
    case CmdKind::ASSERT: {
        std::string d;
        const bool ok = cmd_check(c.check, &d);
        print(std::string(ok ? "ok: " : "FAILED: ") + d, ok ? kLevelInfo : MCLOG_WARN);
        break;
    }
    case CmdKind::PACKET: {
        std::string why;
        if (debug_cmd_queue(c.packet, &why)) {
            const int pend = debug_cmd_pending();
            print("queued: " + debug_cmd_describe(c.packet) + (pend > 1 ? " (" + std::to_string(pend) + " waiting)" : ""));
        } else {
            print("refused: " + why, MCLOG_WARN);
        }
        break;
    }
    case CmdKind::HOST:
        if (c.host.op == HostOp::ECHO) print(c.host.text);
        else if (c.host.op == HostOp::RUN) {
            std::string err;
            if (!run_file(c.host.text, false, &err)) print(err, MCLOG_WARN);
        } else {
            host_.push_back(c.host);
        }
        break;
    case CmdKind::WAIT: case CmdKind::INPUT:
        print("'" + c.verb + "' works in scenario files only", MCLOG_WARN);
        break;
    default: break;
    }
}

std::vector<HostCmd> Console::take_host_commands() {
    std::vector<HostCmd> h;
    h.swap(host_);
    return h;
}

// ---- ticks and scenarios -------------------------------------------------------------------------------------
void Console::after_tick() {
    pull_log();
    ScenarioRunner &r = scenario_runner();
    if (!r.running()) return;
    const ScenarioStep st = r.step(++scn_tick_);
    for (const std::string &o : st.output) print(o);
    for (const std::string &f : st.failures) print(f, MCLOG_WARN);
    for (const HostCmd &h : st.host) host_.push_back(h);
    if (st.done) {
        print("scenario " + r.summary(), r.failures() ? MCLOG_WARN : kLevelInfo);
        if (input_installed_) {
            g_hook_player_local_input = saved_input_;
            input_installed_ = false;
        }
        if (r.failures()) exit_code_ = 1;
        if (quit_when_done_) {
            HostCmd q;
            q.op = HostOp::QUIT;
            host_.push_back(q);
        }
    }
}

bool Console::run_file(const std::string &path, bool quit_when_done, std::string *err) {
    Scenario s;
    const std::string p = resolve_path(path);
    if (!scenario_load(p, &s)) { *err = s.error; return false; }
    if (scenario_runner().running()) stop_scenario();
    if (!quit_when_done && (s.level >= 0 || s.rts))
        print(s.name + ": its level / rts header is ignored (the scenario runs from now on in the current game)");
    quit_when_done_ = quit_when_done;
    scn_tick_ = 0;
    if (!s.inputs.empty()) {
        saved_input_ = g_hook_player_local_input;
        g_hook_player_local_input = scenario_local_input;
        input_installed_ = true;
    }
    scenario_runner().start(s);
    print("scenario " + s.name + (s.title.empty() ? "" : " (" + s.title + ")") + ": " + std::to_string(s.items.size()) + " lines" +
          (s.stop > 0 ? ", stop " + std::to_string(s.stop) : ""));
    const ScenarioStep st = scenario_runner().step(0);
    for (const std::string &o : st.output) print(o);
    for (const std::string &f : st.failures) print(f, MCLOG_WARN);
    for (const HostCmd &h : st.host) host_.push_back(h);
    return true;
}

bool Console::scenario_running() const { return scenario_runner().running(); }

void Console::stop_scenario() {
    if (!scenario_runner().running()) return;
    scenario_runner().stop();
    print("scenario stopped: " + scenario_runner().summary(), MCLOG_WARN);
    if (input_installed_) {
        g_hook_player_local_input = saved_input_;
        input_installed_ = false;
    }
}

// ---- stdin ---------------------------------------------------------------------------------------------------
void Console::start_stdin_reader() {
    if (s_stdin_started) return;
    s_stdin_started = true;
    std::thread([] {
        std::string l;
        while (std::getline(std::cin, l)) {
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
            std::lock_guard<std::mutex> g(s_stdin_mutex);
            s_stdin_lines.push_back(l);
        }
        std::lock_guard<std::mutex> g(s_stdin_mutex);
        s_stdin_eof = true;
    }).detach();
    mclog(MCLOG_INFO, "console: reading commands from stdin");
}

void Console::poll_stdin() {
    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> g(s_stdin_mutex);
        lines.swap(s_stdin_lines);
    }
    for (const std::string &l : lines) {
        add_history(l);
        execute(l);
    }
}

// ---- drawing -------------------------------------------------------------------------------------------------
// The HUD font (font 1) has no glyph for | \ ` ~ { }: drawn as look-alikes (display only).
static std::string displayable(std::string s) {
    for (char &c : s) {
        switch (c) {
        case '|': case '\\': c = '/'; break;
        case '`': c = '\''; break;
        case '~': c = '-'; break;
        case '{': c = '('; break;
        case '}': c = ')'; break;
        default: break;
        }
    }
    return s;
}

void Console::draw(int vh, const uint8_t *menu_pal6) {
    pull_log();
    if (!open_) return;
    const UiSpriteTable *font = g_ui_font;
    ui_set_font(1);
    const int lh = ui_font_line_height() + 2;
    const int h = vh / 2;
    // Over the menus (menu_pal6: the palette on screen, 256 x 6-bit RGB) the game's shade table and colour cube do
    // not apply: a solid panel and the nearest colours of that palette.
    auto pick = [&](int r, int g, int b) -> uint8_t {       // 4-bit components, as ui_colour
        if (!menu_pal6) return ui_colour(r, g, b);
        int best = 0, best_d = 1 << 30;
        for (int i = 0; i < 256; i++) {
            const int dr = menu_pal6[i * 3] - r * 63 / 15, dg = menu_pal6[i * 3 + 1] - g * 63 / 15, db = menu_pal6[i * 3 + 2] - b * 63 / 15;
            const int d = dr * dr + dg * dg + db * db;
            if (d < best_d) { best = i; best_d = d; }
        }
        return (uint8_t)best;
    };
    if (menu_pal6) gfx_fill_rect(0, 0, 640, h, pick(1, 1, 2));
    else ui_shade_rect(0, 0, 640, h, 0x38);
    const uint8_t white = menu_pal6 ? pick(15, 15, 15) : ui_col_white(), gold = pick(15, 12, 3), warn = pick(15, 6, 4), grey = pick(9, 9, 10);
    const uint8_t edge = pick(9, 7, 2);
    vga_fill_line(0, h, 640, h, edge);
    // the input line at the bottom of the panel
    const int y_in = h - lh - 2;
    const std::string shown = "] " + displayable(line_);
    ui_draw_text(shown.c_str(), 4, y_in, gold);
    const std::string before = "] " + displayable(line_.substr(0, (size_t)cur_));
    const int cx = 4 + ui_text_width(before.c_str());
    gfx_fill_rect(cx, y_in + lh - 3, 6, 2, gold);
    // the scrollback above it, newest at the bottom
    const int rows = (y_in - 4) / lh;
    const int total = (int)scroll_.size();
    if (scroll_off_ > total - rows) scroll_off_ = std::max(0, total - rows);
    int y = y_in - lh;
    for (int i = total - 1 - scroll_off_, k = 0; i >= 0 && k < rows; i--) {
        const Out &o = scroll_[(size_t)i];
        const uint8_t c = o.level <= MCLOG_WARN ? warn : o.level >= MCLOG_DEBUG ? grey
                        : o.text.rfind("> ", 0) == 0 ? gold : white;
        // wrapped at the panel's width (continuation lines indented), drawn bottom-up
        std::vector<std::string> parts;
        std::string rest = displayable(o.text);
        while (!rest.empty()) {
            size_t n = rest.size();
            while (n > 1 && ui_text_width(rest.substr(0, n).c_str()) > 640 - 8 - (parts.empty() ? 0 : 16)) n--;
            if (n < rest.size()) {                          // break at a space when there is one
                const size_t sp = rest.rfind(' ', n);
                if (sp != std::string::npos && sp > n / 2) n = sp + 1;
            }
            parts.push_back(rest.substr(0, n));
            rest = rest.substr(n);
        }
        for (size_t p = parts.size(); p-- > 0 && k < rows; k++, y -= lh)
            ui_draw_text(parts[p].c_str(), p ? 20 : 4, y, c);
    }
    if (scroll_off_ > 0) {
        const std::string m = "-- " + std::to_string(scroll_off_) + " more below (Page Down) --";
        ui_draw_text(m.c_str(), 640 - 4 - ui_text_width(m.c_str()), y_in, grey);
    }
    g_ui_font = font;
}
