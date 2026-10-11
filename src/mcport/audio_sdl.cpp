// SDL audio output + MIDI output (round 5, task A). See audio_sdl.h.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "audio_sdl.h"
#include "audio_mixer.h"
#include "opl_driver.h"
#include "sound.h"
#include "mcfile.h"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {

#ifdef _WIN32
// The Windows MIDI mapper (normally the Microsoft GS Wavetable Synth): General MIDI, like the MPU-401
// path of the original (HMI device 0xa001 -> bank suffix 2).
struct WinMidiOut : MidiOut {
    HMIDIOUT h = nullptr;
    bool open() { return midiOutOpen(&h, MIDI_MAPPER, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR; }
    void close() {
        if (!h) return;
        midiOutReset(h);
        midiOutClose(h);
        h = nullptr;
    }
    void send(uint8_t st, uint8_t d1, uint8_t d2) override {
        if (h) midiOutShortMsg(h, (DWORD)st | ((DWORD)d1 << 8) | ((DWORD)d2 << 16));
    }
    void panic() override { if (h) midiOutReset(h); }
};
#endif

struct AudioState {
    AudioEngine engine;
    SquareSynth square;
    OplMidiOut opl;                                           // HMI OPL2 driver on the software YM3812
#ifdef _WIN32
    WinMidiOut winmidi;
#endif
    SDL_AudioDeviceID dev = 0;
    bool sdl_audio = false;
    std::thread midi_thread;
    std::atomic<bool> midi_run{false};
    std::chrono::steady_clock::time_point last_update;
    bool inited = false;
};
AudioState *g_audio = nullptr;

void SDLCALL audio_callback(void *user, Uint8 *stream, int len) {
    AudioState *a = (AudioState *)user;
    a->engine.render((int16_t *)stream, len / 4);                 // AUDIO_S16SYS, 2 channels
}

// The HMI timer drove the song at its own rate (120 Hz); here a thread advances it by real time.
void midi_thread_main(AudioState *a) {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    auto last = std::chrono::steady_clock::now();
    while (a->midi_run.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto now = std::chrono::steady_clock::now();
        uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now - last).count();
        last = now;
        a->engine.advance_music_us(us);
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

bool file_exists(const char *game_dir, const char *rel) {
    char full[1024];
    mc_path_join(full, sizeof full, game_dir, rel);
    FILE *f = std::fopen(full, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

const char *env(const char *name) {
    const char *v = std::getenv(name);
    return v ? v : "";
}

// What music_init_hmi_4d550 does for device 0xa002: data/inst.bnk + data/drum.bnk (RNC) into the driver.
bool load_opl_banks(const char *game_dir, OplMidiOut &opl) {
    char p1[1024], p2[1024];
    mc_path_join(p1, sizeof p1, game_dir, "data/inst.bnk");
    mc_path_join(p2, sizeof p2, game_dir, "data/drum.bnk");
    mc_blob inst{}, drum{};
    bool ok = mc_read_unpacked(p1, &inst) && mc_read_unpacked(p2, &drum) &&
              opl.load_banks(inst.data, inst.len, drum.data, drum.len);
    mc_blob_free(&inst);
    mc_blob_free(&drum);
    return ok;
}

} // namespace

bool audio_init(const char *game_dir) {
    if (g_audio) return g_sound_available || g_music_available;
    g_audio = new AudioState();
    AudioState &a = *g_audio;
    g_sound_available = g_sound_on = 0;
    g_music_available = g_music_on = 0;

    // ---- digital sound: what sound_digital_init_4da10 + mem_init_pools_59500 decided ----
    g_sound_quality = 1;                                      // >= 5 MB free: the 22050 Hz bank snds<set>-1
    bool want_sound = std::strcmp(env("MC_SOUND"), "0") != 0 && file_exists(game_dir, "data/snds0-1.dat");
    std::string music_env = env("MC_MUSIC");
    bool want_music = music_env != "0";
    bool square = music_env == "square";
    bool midi_first = music_env == "midi";

    if (want_sound || want_music) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            SDL_AudioSpec want{}, have{};
            want.freq = 44100;
            want.format = AUDIO_S16SYS;
            want.channels = 2;
            want.samples = 512;
            want.callback = audio_callback;
            want.userdata = &a;
            a.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
            if (a.dev) {
                a.engine.set_output_rate(have.freq);
                if (*env("MC_SOUND_VOLUME")) a.engine.mixer().set_master(std::atoi(env("MC_SOUND_VOLUME")));
                a.sdl_audio = true;
            } else {
                std::fprintf(stderr, "audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
                SDL_QuitSubSystem(SDL_INIT_AUDIO);
            }
        } else {
            std::fprintf(stderr, "audio: SDL audio init failed: %s\n", SDL_GetError());
        }
    }
    if (want_sound && a.sdl_audio) { g_sound_available = 1; g_sound_on = 1; }

    // ---- music ----
    // MC_MUSIC=opl (default): the FM bank music<set>-0 through the original's OPL2 driver on the software
    // YM3812, rendered into the SDL mix (HMI device 0xa002, g_music_device 0, as on a Sound Blaster).
    // MC_MUSIC=midi: the General MIDI bank music<set>-2 through the Windows MIDI mapper (device 0xa001,
    // g_music_device 2); falls back to OPL2 when no MIDI device opens. MC_MUSIC=square: the GM bank
    // through the square-wave stand-in. MC_MUSIC=0: no music. MC_MUSIC_VOLUME=n: OPL output gain (0x100 =
    // one channel at full scale = -18 dBFS, the default).
    const char *music_what = "off";
    if (want_music) {
        bool fm_files = file_exists(game_dir, "data/music0-0.dat");
        bool gm_files = file_exists(game_dir, "data/music0-2.dat");
        auto use_opl = [&]() {
            if (square || !fm_files || !a.sdl_audio || !load_opl_banks(game_dir, a.opl)) return false;
            if (*env("MC_MUSIC_VOLUME")) a.opl.set_gain(std::atoi(env("MC_MUSIC_VOLUME")));
            g_music_device = 0;                               // HMI 0xa002 -> "*.HMP"
            a.engine.set_music_device(MC_HMP_DEVICE_OPL2);
            a.engine.set_midi(&a.opl, AudioEngine::CLOCK_RENDER);
            music_what = "OPL2 (FM bank)";
            return true;
        };
        auto use_midi = [&]() {
#ifdef _WIN32
            if (square || !gm_files || !a.winmidi.open()) return false;
            g_music_device = 2;                               // HMI 0xa001 (MPU-401) / 0xa008 -> "*.GEN"
            a.engine.set_music_device(MC_HMP_DEVICE_MPU401);
            a.engine.set_midi(&a.winmidi, AudioEngine::CLOCK_EXTERNAL);
            a.midi_run = true;
            a.midi_thread = std::thread(midi_thread_main, &a);
            music_what = "MIDI mapper (GM bank)";
            return true;
#else
            return false;
#endif
        };
        auto use_square = [&]() {
            if (!gm_files || !a.sdl_audio) return false;      // no MIDI device: the square synth in the mix
            g_music_device = 2;
            a.engine.set_music_device(MC_HMP_DEVICE_MPU401);
            a.engine.set_midi(&a.square, AudioEngine::CLOCK_RENDER);
            music_what = "square synth (GM bank)";
            return true;
        };
        bool ok = midi_first ? (use_midi() || use_opl() || use_square())
                             : (use_opl() || use_midi() || use_square());
        if (ok) { g_music_available = 1; g_music_on = 1; }
    }

    sound_set_backend(&a.engine);
    if (a.sdl_audio) SDL_PauseAudioDevice(a.dev, 0);
    a.last_update = std::chrono::steady_clock::now();
    a.inited = true;
    std::printf("audio: sound %s (%d Hz), music %s\n", g_sound_available ? "on" : "off", a.engine.output_rate(), music_what);
    return g_sound_available || g_music_available;
}

void audio_set_volumes(int sound_q8, int music_q8) {
    if (!g_audio) return;
    g_audio->engine.mixer().set_master(sound_q8);
    g_audio->opl.set_gain(music_q8);
}

void audio_update() {
    if (!g_audio) return;
    auto now = std::chrono::steady_clock::now();
    uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now - g_audio->last_update).count();
    g_audio->last_update = now;
    g_audio->engine.pump_fade_timers(us);
}

void audio_shutdown() {
    if (!g_audio) return;
    AudioState &a = *g_audio;
    sound_stop_all();
    music_stop();
    if (a.midi_thread.joinable()) {
        a.midi_run = false;
        a.midi_thread.join();
    }
    if (a.sdl_audio) {
        SDL_CloseAudioDevice(a.dev);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
#ifdef _WIN32
    a.winmidi.close();
#endif
    sound_set_backend(nullptr);
    g_sound_available = g_sound_on = 0;
    g_music_available = g_music_on = 0;
    delete g_audio;
    g_audio = nullptr;
}
