// Game-side sound manager (see sound.h). Translated from carpet.exe: sound_request_49720,
// sound_update_494b0, sound_priority_ok_49c20, sound_fade_player_sound_49c40, sound_playing_count_49d50,
// sound_sample_done_49d80, the fades 0x4dfc0..0x4e400, the sample layer 0x4f6f0..0x4f8a0 / 0x5cbb0 /
// 0x5cd60 / 0x5cea0 / 0x5c040 / 0x627d0 / 0x628a4, the bank loader 0x5c990 / 0x5ca58 and the music
// plumbing 0x1f6d0 / 0x1f800 / 0x1f960 / 0x5c0a0 / 0x5cf40 / 0x49ca0..0x49d10 plus the level-start
// track choice of game_main_32a00. The HMI calls are the SoundBackend.
#include "sound.h"
#include "thing.h"
#include "sndbank.h"
#include <cstring>

// ---- backend ---------------------------------------------------------------------------------------
int  SoundBackend::start(int, int, const uint8_t *, uint32_t, int, int, int, int, int) { return -1; }
void SoundBackend::stop(int) {}
void SoundBackend::set_volume(int, int) {}
bool SoundBackend::is_playing(int) { return false; }
void SoundBackend::flush() {}
bool SoundBackend::music_play(int, const uint8_t *, uint32_t) { return false; }
void SoundBackend::music_stop() {}
bool SoundBackend::music_done() { return true; }
void SoundBackend::music_set_volume(int) {}
void SoundBackend::music_set_layer_volume(int) {}
void SoundBackend::music_timer(MusicTimer, int) {}

static SoundBackend  g_null_backend;
static SoundBackend *g_backend = &g_null_backend;
void          sound_set_backend(SoundBackend *b) { g_backend = b ? b : &g_null_backend; }
SoundBackend *sound_backend()                    { return g_backend; }

// ---- state -----------------------------------------------------------------------------------------
uint8_t  g_sound_quality = 1;                       // DAT_0009e328
static SoundRequest g_requests[MC_SOUND_IDS];       // 0xb7ac0
static SoundChannel g_chan[MC_SOUND_CHANNELS];      // DAT_0012e070 / 0e0f0 / e270 / e2b0 / e2f0 / e330 / e350
static uint16_t     g_fadein_request_target = 0;    // DAT_0009e3c0
static int          g_last_handle = -1;             // DAT_0012e1c4

static mc_sndbank        g_bank_storage;            // DAT_0012e1b0 (data) / DAT_0012e1d4 (tab)
static bool              g_bank_loaded = false;
static const mc_sndbank *g_bank = nullptr;
static mc_sndbank        g_music_bank_storage;      // DAT_0012dfe8 / music tab
static bool              g_music_bank_loaded = false;

uint8_t  g_music_device = 0;                        // DAT_0012e06e
uint16_t g_music_track = 0;                         // DAT_0009e312
uint16_t g_music_track_count = 0;                   // DAT_0009e316
uint16_t g_music_volume = 0;                        // DAT_0009e310
static uint8_t g_mood_level = 0;                    // DAT_000938f8 combat-layer volume 0..0x7e
static uint8_t g_mood_cur = 1;                      // DAT_000938f9
static uint8_t g_mood_active = 0;                   // DAT_000938fa timer event installed
static int8_t  g_mood_dir = -2;                     // DAT_000938fb
static uint8_t g_fadeout_volume = 0x7f;             // DAT_000943c8
static uint8_t g_fadeout_active = 0;                // DAT_000943c9
static int     g_timer_rate[2] = { 0, 0 };          // the two HMI timer events (handles at 0x80000 / 0x80020)

static inline bool chan_done(int ch) { return !g_backend->is_playing(g_chan[ch].handle); }   // hmi_sample_done_6291e
static inline void chan_stop(int ch) { g_backend->stop(g_chan[ch].handle); }                 // hmi_stop_sample_65485
static inline void chan_set_volume(int ch, int vol) { g_backend->set_volume(g_chan[ch].handle, vol); }   // hmi_set_sample_volume_64d38

static inline const uint8_t *sample_data(int sample, uint32_t *len) {
    return g_bank ? mc_sndbank_sample(g_bank, sample, len) : (*len = 0, nullptr);
}
static inline int sample_rate() { return g_bank ? g_bank->rate : 22050; }

// hmi_start_sample_64e7c with the fields the game fills in the shared start structure (0x9e32c). The
// structure is a global that each path only partially rewrites: the pan word (+0x32) and the loop count
// (+0xc) keep a stale value when the path does not set them, but the HMI flags word decides what is
// honoured - 0x200 (_PANNING) for the pan, 0x4000 (_LOOPING) for the loop count - so the stale values
// are dead and the port passes centre / 0 instead.
static int hmi_start(int ch, int owner, int sample, int volume, int pan, int loop_count, int flags) {
    uint32_t len;
    const uint8_t *data = sample_data(sample, &len);        // tab[sample].+0x12, +0x1a - 0x10
    if (!(flags & 0x200)) pan = 0x7fff;
    if (!(flags & 0x4000)) loop_count = 0;
    g_chan[ch].handle = g_backend->start(owner, sample, data, len, sample_rate(), volume, pan, loop_count, flags);
    g_last_handle = g_chan[ch].handle;
    return g_last_handle;
}

void sound_reset() {
    std::memset(g_requests, 0, sizeof g_requests);
    std::memset(g_chan, 0, sizeof g_chan);
    for (auto &c : g_chan) c.handle = -1;
    g_fadein_request_target = 0;
    g_last_handle = -1;
}

const SoundRequest *sound_request_slot(int sound) { return (sound >= 0 && sound < MC_SOUND_IDS) ? &g_requests[sound] : nullptr; }
const SoundChannel *sound_channel(int ch)         { return (ch >= 0 && ch < MC_SOUND_CHANNELS) ? &g_chan[ch] : nullptr; }
int sound_last_handle()                           { return g_last_handle; }

// ---- bank ------------------------------------------------------------------------------------------
// sound_load_bank_5c990 + sound_bank_relocate_5ca58
bool sound_load_bank(const char *game_dir, int set) {
    if (!g_sound_available) return false;
    // Port: the voices of the old bank point into its memory (HMI kept reading it); drop them first.
    if (g_bank_loaded || g_bank) g_backend->flush();
    if (g_bank_loaded) { mc_sndbank_free(&g_bank_storage); g_bank_loaded = false; }
    if (g_bank == &g_bank_storage) g_bank = nullptr;
    if (!mc_sndbank_load(game_dir, set, g_sound_quality, &g_bank_storage)) return false;
    g_bank_loaded = true;
    g_bank = &g_bank_storage;
    return true;
}
void sound_set_bank(const mc_sndbank *bank) { g_bank = bank; }
const mc_sndbank *sound_bank()              { return g_bank; }
int sound_sample_count()                    { return g_bank ? mc_sndbank_count(g_bank) : 0; }   // DAT_0012e246

// ---- request side ------------------------------------------------------------------------------------
// sound_priority_ok_49c20
bool sound_priority_ok(int new_volume, int cur_volume) { return new_volume - cur_volume >= -8; }

// The positional part of sound_request_49720 (0x4974c..0x4993b). `owner` receives Thing.owner.
static bool locate(int thing, int *volume, int *pan, uint16_t *owner) {
    *owner = 0;
    int idx = (int16_t)thing;
    if (idx <= 0) { *volume = 0x7fff; *pan = 0x7fff; return true; }
    const Thing *t = thing_at((unsigned)idx);
    if (t->flags & 0x80) return false;
    const Thing *listener = thing_at(thing_wrap(g_state->players[g_state->local_player].thing));
    // 48 cells. Round 11: pos_dist_sq_xy is int64 (the original's int32 wrap in a 256-cell world, thing.h),
    // so a source 128+ cells away in a wider world is culled instead of wrapping negative; past the cull
    // dist <= 0x3000 and the volume / pan arithmetic below stays in int32.
    if (pos_dist_sq_xy(thing_pos(listener), thing_pos(t)) > 0x9000000) return false;
    *owner = t->owner;
    int dist  = pos_dist_xy(thing_pos(listener), thing_pos(t));
    int angle = pos_angle_to(thing_pos(listener), thing_pos(t)) & 0xffff;
    int diff  = angle_diff(listener->yaw, angle) & 0xffff;              // 0..0x400
    int dir   = angle_turn_dir(listener->yaw, angle);                   // -1 / 0 / +1
    // hearing range by direction: 12 * (0x400 - diff / 2) = 0x3000 ahead .. 0x1800 behind
    int range = 12 * (0x400 - (diff >> 1));
    int vol = (range - dist) * 0x7fff / range;                          // idiv: toward zero
    if (vol < 0x200) return false;
    if (vol > 0x7fff) vol = 0x7fff;
    int p;
    if (dist <= 0x140) {
        p = 0x7fff;
    } else {
        int d = diff > 0x200 ? 0x400 - diff : diff;
        p = ((d << 15) * dir) / 512 + 0x7fff;                           // 0x7fff +- 64 * d
        if (p < 0) p = 0;
        if (p > 0xffff) p = 0xffff;
    }
    *volume = vol; *pan = p;
    return true;
}

bool sound_locate(int thing, int *volume, int *pan) {
    uint16_t owner;
    return locate(thing, volume, pan, &owner);
}

// How sound_request_49720 treats each sound id (the jump table at 0x49664, index id - 1).
enum SoundKind : uint8_t {
    SK_NONE,            // 6 (NULL.RAW)
    SK_RESTART,         // mode 1 request, owner = thing's owner
    SK_IF_IDLE,         // mode 3 request
    SK_LOCAL_RESTART,   // mode 1: local player -> owner 0; player -1 -> thing's owner; else dropped
    SK_LOCAL_IF_IDLE,   // mode 3 variant of the above (0x11)
    SK_FADE_IN_CARD,    // local-player loop, fade in to kind_arg; not with g_sound_quality 3 (1, 2 waves / wind, 0x1f market)
    SK_FADE_IN,         // local-player loop, fade in to kind_arg (5 fire)
};
struct SoundKindEntry { SoundKind kind; uint8_t arg; };
static const SoundKindEntry g_sound_kind[MC_SOUND_IDS] = {
    /* 0x00 */ {SK_NONE, 0},
    /* 0x01 */ {SK_FADE_IN_CARD, 0x46}, {SK_FADE_IN_CARD, 0x46}, {SK_RESTART, 0}, {SK_LOCAL_RESTART, 0},
    /* 0x05 */ {SK_FADE_IN, 0x78}, {SK_NONE, 0}, {SK_IF_IDLE, 0}, {SK_IF_IDLE, 0},
    /* 0x09 */ {SK_RESTART, 0}, {SK_IF_IDLE, 0}, {SK_IF_IDLE, 0}, {SK_IF_IDLE, 0},
    /* 0x0d */ {SK_IF_IDLE, 0}, {SK_LOCAL_RESTART, 0}, {SK_RESTART, 0}, {SK_RESTART, 0},
    /* 0x11 */ {SK_LOCAL_IF_IDLE, 0}, {SK_RESTART, 0}, {SK_RESTART, 0}, {SK_RESTART, 0},
    /* 0x15 */ {SK_RESTART, 0}, {SK_RESTART, 0}, {SK_RESTART, 0}, {SK_RESTART, 0},
    /* 0x19 */ {SK_RESTART, 0}, {SK_RESTART, 0}, {SK_RESTART, 0}, {SK_RESTART, 0},
    /* 0x1d */ {SK_LOCAL_RESTART, 0}, {SK_RESTART, 0}, {SK_FADE_IN_CARD, 0x55}, {SK_IF_IDLE, 0},
    /* 0x21 */ {SK_IF_IDLE, 0}, {SK_IF_IDLE, 0}, {SK_IF_IDLE, 0}, {SK_IF_IDLE, 0},
    /* 0x25 */ {SK_IF_IDLE, 0}, {SK_IF_IDLE, 0}, {SK_IF_IDLE, 0}, {SK_RESTART, 0},
    /* 0x29 */ {SK_IF_IDLE, 0}, {SK_RESTART, 0}, {SK_RESTART, 0}, {SK_RESTART, 0},
    /* 0x2d */ {SK_RESTART, 0}, {SK_NONE, 0},
};

static inline void store_request(int sound, int mode, int pan, int volume, int owner) {
    SoundRequest &r = g_requests[sound];
    r.volume = (uint16_t)volume;
    r.pan = (uint16_t)pan;
    r.owner = (uint16_t)owner;
    r.mode = (uint16_t)mode;
}

// sound_request_49720(thing, player, sound)
void sound_request_play(int thing, int player, int sound) {
    if (!g_sound_on || !g_sound_available) return;
    int volume, pan;
    uint16_t owner;
    if (!locate(thing, &volume, &pan, &owner)) return;
    unsigned id = (unsigned)(uint16_t)sound;
    if (id - 1 > 0x2c) return;                                    // jump table 1..0x2d
    int16_t pl = (int16_t)player;
    bool local = pl == g_state->local_player;
    const SoundKindEntry &k = g_sound_kind[id];
    switch (k.kind) {
    case SK_FADE_IN_CARD:
        if (local && g_sound_quality != 3) sound_request_fade_in(0, (int)id, 3, k.arg);
        break;
    case SK_FADE_IN:
        if (local) sound_request_fade_in(0, (int)id, 3, k.arg);
        break;
    case SK_RESTART:
    case SK_IF_IDLE:
        if (sound_priority_ok(volume, g_requests[id].volume))
            store_request((int)id, k.kind == SK_RESTART ? 1 : 3, pan, volume, owner);
        break;
    case SK_LOCAL_RESTART:
    case SK_LOCAL_IF_IDLE: {
        if (!sound_priority_ok(volume, g_requests[id].volume)) return;
        int mode = k.kind == SK_LOCAL_RESTART ? 1 : 3;
        if (local) store_request((int)id, mode, pan, volume, 0);
        else if (pl == -1) store_request((int)id, mode, pan, volume, owner);
        break;
    }
    default:
        break;
    }
}

// sound_fade_player_sound_49c40(thing, player, sound)
void sound_fade_player_sound(int /*thing*/, int player, int sound) {
    if (!g_sound_on || !g_sound_available) return;
    uint16_t s = (uint16_t)sound;
    if (s < 5) {
        if (s < 1 || s > 2) return;
        if (g_sound_quality == 3) return;
    } else if (s == 0x1f) {
        if (g_sound_quality == 3) return;
    } else if (s != 5) {
        return;
    }
    if ((int16_t)player != g_state->local_player) return;
    sound_start_fade_out(0, s, 0);
}

// ---- per tick ----------------------------------------------------------------------------------------
// sound_update_494b0
void sound_update() {
    if (!g_sound_on || !g_sound_available) return;
    if (g_cfg->paused & 1) return;
    sound_update_fadeout();
    sound_update_fadein();
    for (int id = 0; id < MC_SOUND_IDS; id++) {
        SoundRequest &r = g_requests[id];
        switch (r.mode) {
        case 1:
            sound_restart_sample(r.owner, id, r.volume, r.pan);
            r.mode = 0; r.played = 2;
            break;
        case 2:
            sound_play_simple(r.owner, id, r.volume, r.pan);
            r.mode = 0; r.played = 2;
            break;
        case 3:
            sound_play_if_idle(r.owner, id, r.volume, r.pan);
            r.mode = 0; r.played = 2;
            break;
        case 4:
            if (r.volume == 0x200) {
                sound_stop_sample(0, id);
            } else {
                sound_play_sample(0, id, -1);
                sound_set_sample_volume(0, id, (int)r.volume >> 8);
            }
            r.mode = 0; r.played = 2;
            break;
        default:
            break;
        }
        r.volume = 0;
    }
}

// sound_playing_count_49d50
int sound_playing_count() {
    if (!g_sound_available || !g_sound_on) return 0;
    return sound_count_playing();
}

// sound_sample_done_49d80(owner, sample)
bool sound_sample_done(int owner, int sample) {
    if (!g_sound_available || !g_sound_on) return true;
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++)
        if (g_chan[ch].owner == (uint16_t)owner && g_chan[ch].sample == (uint16_t)sample)
            return chan_done(ch);
    return true;
}

// ---- fades -------------------------------------------------------------------------------------------
// sound_update_fadein_4dfc0
void sound_update_fadein() {
    if (!g_sound_available || !g_sound_on) {
        for (auto &c : g_chan) c.fadein = 0;
        return;
    }
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++) {
        SoundChannel &c = g_chan[ch];
        if (!c.fadein) continue;
        bool done = false;
        if (chan_done(ch)) {
            done = true;
        } else {
            // add byte ptr [volume + 1], 8: +0x800 on the high byte, no carry out
            unsigned vol = (c.volume & 0xff) | ((((c.volume >> 8) + 8) & 0xff) << 8);
            if (vol >= 0x7fff) { vol = 0x7fff; done = true; }
            int target = ((int)c.fadein_target << 8) - 1;
            if ((int)vol >= target) { vol = (uint16_t)target; done = true; }
            c.volume = (uint16_t)vol;
            chan_set_volume(ch, c.volume);
        }
        if (done) c.fadein = 0;
    }
}

// sound_update_fadeout_4e2e0
void sound_update_fadeout() {
    if (!g_sound_available || !g_sound_on) {
        for (auto &c : g_chan) c.fadeout = 0;
        return;
    }
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++) {
        SoundChannel &c = g_chan[ch];
        if (!c.fadeout) continue;
        bool done = false;
        if (chan_done(ch)) {
            done = true;
        } else {
            c.fadeout_volume = (uint16_t)(c.fadeout_volume - 0x800);
            if ((int16_t)c.fadeout_volume <= 0x1000) {
                chan_stop(ch);
                c.fadeout_volume = 0;
                c.volume = 0;
                done = true;
            } else {
                c.volume = c.fadeout_volume;
                chan_set_volume(ch, c.volume);
            }
        }
        if (done) c.fadeout = 0;
    }
}

// sound_request_fade_in_4e0f0(owner, sample, mode, target)
void sound_request_fade_in(int owner, int sample, int mode, int target) {
    g_fadein_request_target = (uint16_t)target;
    if ((uint8_t)mode == 3) sound_play_fade_in((int16_t)owner, (int16_t)sample);
}

// sound_play_fade_in_4e120(owner, sample): raise an already playing (owner, sample) to the target, or
// start it looping at volume 0 (flags 0x4300) and let sound_update_fadein bring it up.
void sound_play_fade_in(int owner, int sample) {
    if (!g_sound_available || !g_sound_on) return;
    if ((int16_t)sample > sound_sample_count()) return;
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++) {
        SoundChannel &c = g_chan[ch];
        if (c.owner != (uint16_t)owner || c.sample != (uint16_t)sample || chan_done(ch)) continue;
        if (c.fadein) return;
        if (c.fadeout) c.fadeout = 0;
        c.fadein = 1;
        c.fadein_target = g_fadein_request_target;
        return;
    }
    int ch = 0;
    for (; ch < MC_SOUND_CHANNELS; ch++) if (chan_done(ch)) break;
    if (ch == MC_SOUND_CHANNELS) return;
    SoundChannel &c = g_chan[ch];
    c.owner = (uint16_t)owner;
    c.sample = (uint16_t)sample;
    c.volume = 0;
    hmi_start(ch, owner, sample, 0, 0x7fff, -1, 0x4300);
    if (c.fadein && c.fadeout) c.fadeout = 0;
    c.fadein = 1;
    c.fadein_target = g_fadein_request_target;
}

// sound_start_fade_out_4e400(owner, sample, arg)
void sound_start_fade_out(int owner, int sample, int arg) {
    if (!g_sound_available || !g_sound_on) return;
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++) {
        SoundChannel &c = g_chan[ch];
        if (c.owner != (uint16_t)owner || c.sample != (uint16_t)sample || chan_done(ch)) continue;
        if (c.fadeout) return;
        if (c.fadein) c.fadein = 0;
        c.fadeout_arg = (uint16_t)arg;
        c.fadeout = 1;
        c.fadeout_volume = c.volume;
        return;
    }
}

// ---- sample layer --------------------------------------------------------------------------------------
// sound_start_sample_4f8a0(owner, sample, volume, pan): first free channel, flags 0x300, one shot.
int sound_start_sample(int owner, int sample, int volume, int pan) {
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++) {
        if (!chan_done(ch)) continue;
        SoundChannel &c = g_chan[ch];
        c.owner = (uint16_t)owner;
        c.sample = (uint16_t)sample;
        c.volume = (uint16_t)((int16_t)volume >> 8);                // sic: the fades expect 0..0x7fff here
        hmi_start(ch, owner, sample, (uint16_t)volume, (uint16_t)pan, 0, 0x300);
        return 1;
    }
    return 0;
}

// sound_restart_sample_4f6f0(owner, sample, volume, pan): stop the first playing (owner, sample), then start.
void sound_restart_sample(int owner, int sample, int volume, int pan) {
    if (!g_sound_available || !g_sound_on) return;
    if ((int16_t)sample > sound_sample_count()) return;
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++) {
        if (g_chan[ch].owner == (uint16_t)owner && g_chan[ch].sample == (uint16_t)sample && !chan_done(ch)) {
            chan_stop(ch);
            break;
        }
    }
    sound_start_sample((int16_t)owner, (int16_t)sample, (int16_t)volume, (uint16_t)pan);
}

// sound_play_if_idle_4f7a0(owner, sample, volume, pan)
void sound_play_if_idle(int owner, int sample, int volume, int pan) {
    if (!g_sound_available || !g_sound_on) return;
    if ((int16_t)sample > sound_sample_count()) return;
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++)
        if (g_chan[ch].owner == (uint16_t)owner && g_chan[ch].sample == (uint16_t)sample && !chan_done(ch)) return;
    sound_start_sample((int16_t)owner, (int16_t)sample, (int16_t)volume, (uint16_t)pan);
}

// sound_play_simple_4f850(owner, sample, volume, pan)
void sound_play_simple(int owner, int sample, int volume, int pan) {
    if (!g_sound_available || !g_sound_on) return;
    if ((int16_t)sample > sound_sample_count()) return;
    sound_start_sample((int16_t)owner, (int16_t)sample, (int16_t)volume, (uint16_t)pan);
}

// sound_play_sample_5cbb0(owner, sample, loop_count): full volume, flags 0x4100, no duplicate.
void sound_play_sample(int owner, int sample, int loop_count) {
    if (!g_sound_available || !g_sound_on) return;
    if ((int16_t)sample > sound_sample_count()) return;
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++)
        if (g_chan[ch].owner == (uint16_t)owner && g_chan[ch].sample == (uint16_t)sample && !chan_done(ch)) return;
    int ch = 0;
    for (; ch < MC_SOUND_CHANNELS; ch++) if (chan_done(ch)) break;
    if (ch == MC_SOUND_CHANNELS) return;
    SoundChannel &c = g_chan[ch];
    c.owner = (uint16_t)owner;
    c.sample = (uint16_t)sample;
    c.volume = 0x7fff;
    hmi_start(ch, owner, sample, 0x7fff, 0x7fff, (int16_t)loop_count, 0x4100);
}

// sound_play_sample_loud_5cd60(owner, sample): cue-script sample, flags 0x100, no duplicate check.
void sound_play_sample_loud(int owner, int sample) {
    if (!g_sound_available || !g_sound_on) return;
    if ((int16_t)sample > sound_sample_count()) return;
    int ch = 0;
    for (; ch < MC_SOUND_CHANNELS; ch++) if (chan_done(ch)) break;
    if (ch == MC_SOUND_CHANNELS) return;
    SoundChannel &c = g_chan[ch];
    c.owner = (uint16_t)owner;
    c.sample = (uint16_t)sample;
    c.volume = 0x7fff;
    hmi_start(ch, owner, sample, 0x7fff, 0x7fff, 0, 0x100);
}

// sound_stop_sample_5cea0(owner, sample)
void sound_stop_sample(int owner, int sample) {
    if (!g_sound_available) return;
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++) {
        if (g_chan[ch].owner == (uint16_t)owner && g_chan[ch].sample == (uint16_t)sample && !chan_done(ch)) {
            chan_stop(ch);
            return;
        }
    }
}

// sound_stop_all_5c040
void sound_stop_all() {
    if (!g_sound_available) return;
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++)
        for (int guard = 0; !chan_done(ch) && guard < MC_SOUND_CHANNELS; guard++) chan_stop(ch);
}

// sound_set_sample_volume_627d0(owner, sample, volume 0..0x80)
void sound_set_sample_volume(int owner, int sample, int volume) {
    if (!g_sound_available) return;
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++) {
        if (g_chan[ch].owner != (uint16_t)owner || g_chan[ch].sample != (uint16_t)sample || chan_done(ch)) continue;
        int16_t v = (int16_t)volume;
        if (v < 0 || v > 0x80) continue;
        chan_set_volume(ch, (v << 8) - 1);
        g_chan[ch].volume = (uint16_t)((v << 8) - 1);
        return;
    }
}

// sound_count_playing_628a4
int sound_count_playing() {
    int n = 0;
    for (int ch = 0; ch < MC_SOUND_CHANNELS; ch++) if (!chan_done(ch)) n++;
    return n;
}

// ---- music -------------------------------------------------------------------------------------------
static void timer_set(MusicTimer which, int rate) {          // hmi_timer_add_event_5d093 / hmi_timer_remove_event_5d3a9
    g_timer_rate[which] = rate;
    g_backend->music_timer(which, rate);
}
int music_fade_rate(MusicTimer which) { return g_timer_rate[which]; }
MusicMoodState music_mood_state() { return { g_mood_level, g_mood_cur, g_mood_active, g_mood_dir }; }

// music_load_bank_5c870 + music_bank_relocate_5c924
bool music_load_bank(const char *game_dir, int set) {
    if (!g_music_available) return false;
    if (g_music_bank_loaded) { mc_sndbank_free(&g_music_bank_storage); g_music_bank_loaded = false; }
    g_music_track_count = 0;
    if (!mc_sndbank_load_named(game_dir, "music", set, g_music_device, 0, &g_music_bank_storage)) return false;
    g_music_bank_loaded = true;
    g_music_track_count = (uint16_t)mc_sndbank_count(&g_music_bank_storage);
    return true;
}
const mc_sndbank *music_bank() { return g_music_bank_loaded ? &g_music_bank_storage : nullptr; }

static bool music_backend_play(int track) {                 // snd_midi_init_song_5e53a + snd_midi_start_song_5eab0
    uint32_t len = 0;
    // the whole record: the HMI gets only the pointer (tab+0x12) and some songs use the last 16 bytes
    const uint8_t *song = g_music_bank_loaded ? mc_sndbank_record_data(&g_music_bank_storage, track, &len) : nullptr;
    return g_backend->music_play(track, song, len);
}

// music_song_done_5cf40
bool music_song_done() { return g_backend->music_done(); }

// music_update_1f800(mood): restarts the song when it ended (calm), otherwise ramps the combat layer
// (MIDI channels 3..5) up at 60 Hz for mood 2 and down at 20 Hz for mood 1 through music_fade_tick.
void music_update(int mood) {
    uint8_t m = (uint8_t)mood;
    if (!g_music_available) return;
    if (g_music_track == 0) {
        if (g_mood_active) { timer_set(MUSIC_TIMER_MOOD, 0); g_mood_active = 0; }
        return;
    }
    if (!g_music_on) return;
    if (music_song_done()) {
        if (g_mood_active) timer_set(MUSIC_TIMER_MOOD, 0);
        g_mood_dir = -2;
        g_mood_level = 0;
        g_mood_cur = 1;
        g_mood_active = 0;
        music_backend_play(g_music_track);
        g_music_volume = 100;
        return;
    }
    if (m == g_mood_cur) return;
    if (g_mood_active) timer_set(MUSIC_TIMER_MOOD, 0);
    g_mood_cur = m;
    g_mood_active = 1;
    g_mood_dir = (int8_t)-g_mood_dir;
    if (m == 1) timer_set(MUSIC_TIMER_MOOD, 0x14);
    else if (m == 2) timer_set(MUSIC_TIMER_MOOD, 0x3c);
}

// music_fade_out_end_49d10
static void music_fade_out_end() {
    if (!g_fadeout_active) return;
    timer_set(MUSIC_TIMER_FADE_OUT, 0);
    music_stop();
    g_backend->music_set_volume(0x7f);
    g_fadeout_volume = 0x7f;
    g_fadeout_active = 0;
}

// music_fade_out_begin_49cd0
void music_fade_out_begin() {
    if (!g_music_available || !g_music_on) return;
    g_fadeout_active = 1;
    timer_set(MUSIC_TIMER_FADE_OUT, 0x3c);
}

// music_mood_fade_cb_1f6d0 (MUSIC_TIMER_MOOD) / music_fade_timer_cb_49ca0 (MUSIC_TIMER_FADE_OUT)
void music_fade_tick(MusicTimer which) {
    if (which == MUSIC_TIMER_FADE_OUT) {
        g_fadeout_volume--;
        g_backend->music_set_volume(g_fadeout_volume);
        if (g_fadeout_volume == 0) music_fade_out_end();
        return;
    }
    if (g_music_track == 0 && g_mood_active) {
        timer_set(MUSIC_TIMER_MOOD, 0);
        g_mood_active = 0;
        return;
    }
    g_mood_level = (uint8_t)(g_mood_level + g_mood_dir);
    g_backend->music_set_layer_volume(g_mood_level);         // controller 7 on channels 3, 4, 5
    if (g_mood_level == 0x7e && g_mood_cur == 2) {
        timer_set(MUSIC_TIMER_MOOD, 0);
        g_mood_active = 0;
    }
    if (g_mood_level == 0 && g_mood_cur == 1) {
        timer_set(MUSIC_TIMER_MOOD, 0);
        g_mood_level = 0;
        g_mood_active = 0;
    }
}

// music_stop_1f960
void music_stop() {
    if (!g_music_available || !g_music_on || g_music_track == 0) return;
    g_backend->music_stop();                                  // stop song (if not done), clear slot, all notes off
    g_music_track = 0;
}

// music_play_track_5c0a0(track)
void music_play_track(int track) {
    int16_t t = (int16_t)track;
    if (!g_music_available || !g_music_on) return;
    if (t > (int)g_music_track_count || (int)g_music_track == (int)t) return;
    if (g_music_track != 0) {
        g_backend->music_stop();
        g_music_track = 0;
    }
    if (music_backend_play(t)) {
        g_music_volume = 100;
        g_music_track = (uint16_t)t;
    } else {
        g_music_available = 0;                                // "\nError : %s", snd_midi_uninit_*
    }
}

// game_main_32a00 (0x32c83..0x32c9c): the level's track, (lcg16(level) % 3) + 1 with an unsigned modulo
// of the sign-extended 16-bit LCG result.
int music_track_for_level(int level) {
    int16_t si = (int16_t)(level * 0x24a1 + 0x24df);
    uint32_t v = (uint32_t)(int32_t)si;
    return (int)(v % 3) + 1;
}

static int32_t *level_track_slot() { return &g_state->level_music_track; }   // GameState+0x240
int music_level_track() { int32_t v; std::memcpy(&v, level_track_slot(), 4); return (int16_t)v; }

void music_start_level(int level) {
    if (!g_music_available || !g_music_on || g_music_track_count == 0) return;
    int32_t track = music_track_for_level(level);
    std::memcpy(level_track_slot(), &track, 4);
    music_play_track((int16_t)track);
}

// ---- platform requests from input.cpp -------------------------------------------------------------------
bool sound_handle_input_request(InputPlatformRequest what, int arg) {
    switch (what) {
    case INPUT_REQ_SOUND_STOP_ALL: sound_stop_all(); return true;
    case INPUT_REQ_MUSIC_STOP:     music_stop(); return true;
    case INPUT_REQ_MUSIC_PLAY:     music_play_track(arg); return true;
    default: return false;
    }
}
static void input_platform_hook(InputPlatformRequest what, int arg) { sound_handle_input_request(what, arg); }

void sound_register_handlers() {
    sound_reset();
    g_hook_sound_request = sound_request_play;
    g_hook_sound_fade = sound_fade_player_sound;
    if (!g_hook_input_platform) g_hook_input_platform = input_platform_hook;
}
