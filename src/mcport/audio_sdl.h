// SDL audio output (round 5, task A): the platform half of the sound system. Opens an SDL audio device
// for AudioEngine (audio_mixer.h, the SoundBackend of mcengine/sound.h) and a MIDI output for the music:
// by default the FM bank music<set>-0 through the original's OPL2 driver on a software YM3812 in the SDL
// mix (opl_driver.h, round 6); MC_MUSIC=midi: the Windows MIDI mapper (winmm, General MIDI bank
// music<set>-2) driven by a 1 ms timer thread (falls back to OPL2 when no MIDI device opens);
// MC_MUSIC=square: the GM bank through the built-in square-wave stand-in.
// Sets what the original's sound set-up (sound_initialise_34140 / sndsetup.inf) set: g_sound_available,
// g_sound_on, g_sound_quality, g_music_available, g_music_on, g_music_device.
//
// Environment: MC_SOUND=0 disables the digital sound, MC_MUSIC=0 the music, MC_MUSIC=opl|midi|square
// selects the music output (default opl), MC_SOUND_VOLUME=n sets the mix gain of the samples (0x100 =
// unity, default 0x60 = 96), MC_MUSIC_VOLUME=n the OPL2 output gain (0x100 default).
// Reports: docs/analysis/port_audio.md (integration lines for mcport/main.cpp), docs/analysis/port_opl.md.
#pragma once

// Open the audio device and the MIDI output, install the backend (sound_set_backend) and set the
// availability / on flags. `game_dir` is the game directory (checked for data/snds0-<q> and
// data/music0-<device>). Returns true when sound or music is available. Safe to call once; call it after
// engine_init (which resets the sound tables) and before the level's banks are loaded.
bool audio_init(const char *game_dir);
// Stop everything, close the devices, put the null backend back.
void audio_shutdown();
// Once per frame on the game thread: runs the music fade timers (music_fade_tick at music_fade_rate Hz)
// for the real time elapsed since the last call.
void audio_update();
// The options menu's sliders, applied at once: the sample mix gain (256 = unity, default 96) and the OPL2 music's
// output gain (256 = default). The Windows MIDI output (music = midi) has no gain here: its volume is the system's.
void audio_set_volumes(int sound_q8, int music_q8);
