// Data sets (base / Hidden Worlds): see world_set.h.
#define _CRT_SECURE_NO_WARNINGS
#include "world_set.h"
#include "../mcdata/mcfile.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

void (*g_hook_world_set_changed)(int set) = nullptr;
void (*g_hook_world_set_sim)(int set) = nullptr;

namespace {
std::string s_game_dir;
std::string s_hidden_dir;
int s_set = 0;

// HIDDEN.EXE's file list (resource lists 0x9934e.., level paths 0xa9304..) against the base names the
// port's loaders ask for (docs/analysis/hidden_worlds_re.md section 6). search.dat, fonts, pointers,
// sounds, music and the front-end screens are the same files in both sets; the title flics
// title-03 / title-04 are not redirected because the port keeps the base game's front end.
const char *const k_hidden_files[] = {
    "data/block16.dat",   "data/blk1-0.dat",
    "data/block32.dat",   "data/blk1-1.dat",
    "data/palette.dat",   "data/pal1-0.dat",
    "data/building.tab",  "data/build1-0.tab",
    "data/building.dat",  "data/build1-0.dat",
    "data/sky.dat",       "data/sky1-0.dat",
    "data/mspr0-0.tab",   "data/mspr1-0.tab",
    "data/mspr0-0.dat",   "data/mspr1-0.dat",
    "data/hspr0-0.tab",   "data/hspr1-0.tab",
    "data/hspr0-0.dat",   "data/hspr1-0.dat",
    "data/tmaps.tab",     "data/tmaps1-0.tab",
    "data/tmaps.dat",     "data/tmaps1-0.dat",
    "data/tables.dat",    "data/dtables.dat",     // shade / blend tables of pal1-0 (stored unpacked)
    "levels/levels.tab",  "levels/ddlevels.tab",
    "levels/levels.dat",  "levels/ddlevels.dat",
    nullptr, nullptr,
};

bool file_exists(const std::string &p) {
    if (FILE *f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; }
    return false;
}

// Level names of HIDDEN.EXE ("1. Goyaan" .. "25. Abnasur"): NUL-terminated strings that follow each
// other in the data object; the numbered run is found by scanning for "1. " .. "25. ".
bool s_names_loaded = false;
std::string s_names[HW_CAMPAIGN_LEVELS];

void load_names() {
    if (s_names_loaded) return;
    s_names_loaded = true;
    mc_blob exe;
    const std::string path = s_hidden_dir + "/hidden.exe";
    if (!mc_read_file(path.c_str(), &exe)) return;
    const char *d = reinterpret_cast<const char *>(exe.data);
    const size_t n = exe.len;
    for (size_t at = 0; at + 4 < n; at++) {
        if (std::memcmp(d + at, "1. ", 3) != 0 || (at > 0 && d[at - 1] >= '0' && d[at - 1] <= '9')) continue;
        std::string got[HW_CAMPAIGN_LEVELS];
        size_t p = at;
        int k = 0;
        for (; k < HW_CAMPAIGN_LEVELS && p < n; k++) {
            char want[8];
            std::snprintf(want, sizeof want, "%d. ", k + 1);
            const size_t wl = std::strlen(want);
            // the string starts here (k == 0) or within the next 8 bytes after the previous NUL padding
            size_t q = p;
            while (q < n && q < p + 8 && std::memcmp(d + q, want, wl) != 0) q++;
            if (q >= n || std::memcmp(d + q, want, wl) != 0) break;
            size_t e = q;
            while (e < n && d[e] != 0) e++;
            got[k].assign(d + q, e - q);
            p = e;
            while (p < n && d[p] == 0) p++;
        }
        if (k == HW_CAMPAIGN_LEVELS) {
            for (int i = 0; i < HW_CAMPAIGN_LEVELS; i++) s_names[i] = got[i];
            break;
        }
    }
    mc_blob_free(&exe);
}

}  // namespace

bool g_engine1995_force = false;

void world_set_init(const char *game_dir, const char *hidden_dir) {
    s_game_dir = game_dir ? game_dir : "";
    const char *e95 = std::getenv("MC_ENGINE1995");
    g_engine1995_force = e95 && e95[0] == '1';
    const char *env = std::getenv("MC_HIDDEN_DIR");
    if (hidden_dir && *hidden_dir) s_hidden_dir = hidden_dir;
    else if (env && *env) s_hidden_dir = env;
    else s_hidden_dir = s_game_dir + "/hidden";
    s_names_loaded = false;
    if (s_set != 0) {
        mc_set_data_redirect(nullptr, nullptr);
        s_set = 0;
    }
}

const char *world_set_hidden_dir() { return s_hidden_dir.c_str(); }

bool world_set_hidden_available() {
    return !s_hidden_dir.empty() && file_exists(s_hidden_dir + "/levels/ddlevels.tab") &&
           file_exists(s_hidden_dir + "/levels/ddlevels.dat") && file_exists(s_hidden_dir + "/data/blk1-0.dat");
}

int world_set() { return s_set; }

bool world_set_select(int set) {
    set = set == 1 ? 1 : 0;
    if (set == s_set) return true;
    if (set == 1 && !world_set_hidden_available()) {
        std::fprintf(stderr, "world_set: Hidden Worlds data not found in %s\n", s_hidden_dir.c_str());
        return false;
    }
    if (set == 1) mc_set_data_redirect(s_hidden_dir.c_str(), k_hidden_files);
    else mc_set_data_redirect(nullptr, nullptr);
    s_set = set;
    if (g_hook_world_set_sim) g_hook_world_set_sim(set);
    if (g_hook_world_set_changed) g_hook_world_set_changed(set);
    return true;
}

int world_campaign_next(int won_level) {
    const int next = won_level + 1;
    if (next == BASE_CAMPAIGN_LEVELS && world_set_hidden_available()) return HW_LEVEL_BASE;
    return next;
}

bool world_campaign_complete(int level) {
    if (world_is_hidden_level(level)) return level >= HW_LEVEL_BASE + HW_CAMPAIGN_LEVELS;
    return level == BASE_CAMPAIGN_LEVELS;
}

const char *world_hidden_level_name(int index) {
    if (!world_is_hidden_level(index) || index - HW_LEVEL_BASE >= HW_CAMPAIGN_LEVELS) return nullptr;
    load_names();
    const std::string &s = s_names[index - HW_LEVEL_BASE];
    return s.empty() ? nullptr : s.c_str();
}
