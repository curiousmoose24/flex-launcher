#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <SDL.h>
#include "launcher.h"
#include <launcher_config.h>
#include "sound.h"
#include "util.h"
#include "debug.h"

extern Config config;

static SDL_AudioDeviceID device = 0;
static SDL_AudioSpec device_spec;
static Uint8 *sound_buffers[NUM_SOUNDS] = {NULL};
static Uint32 sound_lengths[NUM_SOUNDS] = {0};
static const char *default_sounds[NUM_SOUNDS] = {"move.wav", "select.wav", "back.wav"};

// A function to find a default sound file in the assets directory
static char *find_default_sound(const char *file)
{
    const char *prefixes[2];
    char sounds_exe_buffer[MAX_PATH_CHARS + 1];
    prefixes[0] = join_paths(sounds_exe_buffer, sizeof(sounds_exe_buffer), 3, config.exe_path, PATH_ASSETS_EXE, PATH_SOUNDS_EXE);
#ifdef __unix__
    prefixes[1] = PATH_SOUNDS_SYSTEM;
#else
    prefixes[1] = PATH_SOUNDS_RELATIVE;
#endif
    return find_file(file, 2, prefixes);
}

// A function to load a WAV file and convert it to the device format at the configured volume
static void load_sound(SoundType type, const char *path)
{
    SDL_AudioSpec spec;
    Uint8 *wav_buffer;
    Uint32 wav_length;
    if (SDL_LoadWAV(path, &spec, &wav_buffer, &wav_length) == NULL) {
        log_error("Could not load sound '%s'\n%s", path, SDL_GetError());
        return;
    }

    SDL_AudioCVT cvt;
    int ret = SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                  device_spec.format, device_spec.channels, device_spec.freq);
    if (ret < 0) {
        log_error("Could not convert sound '%s'\n%s", path, SDL_GetError());
        SDL_FreeWAV(wav_buffer);
        return;
    }
    cvt.len = (int) wav_length;
    cvt.buf = SDL_malloc((size_t) wav_length * (size_t) cvt.len_mult);
    SDL_memcpy(cvt.buf, wav_buffer, wav_length);
    SDL_FreeWAV(wav_buffer);
    if (ret > 0 && SDL_ConvertAudio(&cvt) < 0) {
        log_error("Could not convert sound '%s'\n%s", path, SDL_GetError());
        SDL_free(cvt.buf);
        return;
    }

    // Apply the volume by mixing the sound into silence (the device format is signed, so silence is 0)
    Uint32 length = (Uint32) cvt.len_cvt;
    Uint8 *buffer = SDL_calloc(1, length);
    SDL_MixAudioFormat(buffer, cvt.buf, device_spec.format, length, config.sound_volume);
    SDL_free(cvt.buf);
    sound_buffers[type] = buffer;
    sound_lengths[type] = length;
}

// A function to open the audio device and load the navigation sounds
void init_sounds()
{
    if (!config.sounds_enabled || device != 0)
        return;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        log_error("Could not initialize audio, disabling sounds\n%s", SDL_GetError());
        config.sounds_enabled = false;
        return;
    }
    SDL_AudioSpec desired = {
        .freq = 48000,
        .format = AUDIO_S16SYS,
        .channels = 2,
        .samples = 1024,
        .callback = NULL
    };
    device = SDL_OpenAudioDevice(NULL, 0, &desired, &device_spec,
                 SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (device == 0) {
        log_error("Could not open audio device, disabling sounds\n%s", SDL_GetError());
        config.sounds_enabled = false;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }

    for (int i = 0; i < NUM_SOUNDS; i++) {
        char *path = config.sound_paths[i] != NULL ? strdup(config.sound_paths[i]) : find_default_sound(default_sounds[i]);
        if (path == NULL)
            log_error("Could not find default sound '%s'", default_sounds[i]);
        else {
            load_sound((SoundType) i, path);
            free(path);
        }
    }
    SDL_PauseAudioDevice(device, 0);
}

// A function to play a navigation sound, interrupting any sound that is still playing
void play_sound(SoundType type)
{
    if (!config.sounds_enabled || device == 0 || sound_buffers[type] == NULL)
        return;
    SDL_ClearQueuedAudio(device);
    SDL_QueueAudio(device, sound_buffers[type], sound_lengths[type]);
}

// A function to pause the audio device while an application is running
void pause_sounds(bool pause)
{
    if (device == 0)
        return;
    if (pause)
        SDL_ClearQueuedAudio(device);
    SDL_PauseAudioDevice(device, pause ? 1 : 0);
}

// A function to turn sounds on or off, and save the choice to the config file
void toggle_sounds()
{
    config.sounds_enabled = !config.sounds_enabled;
    if (config.sounds_enabled) {
        init_sounds();
        play_sound(SOUND_SELECT);
    }
    else if (device != 0)
        SDL_ClearQueuedAudio(device);
    log_debug("Sounds %s", config.sounds_enabled ? "enabled" : "disabled");

    if (config.config_file_path != NULL &&
    !save_config_setting(config.config_file_path, "Sounds", SETTING_SOUNDS_ENABLED, config.sounds_enabled ? "true" : "false"))
        log_error("Could not save the sound setting to the config file");
}

// A function to close the audio device and free the sounds
void quit_sounds()
{
    if (device != 0) {
        SDL_CloseAudioDevice(device);
        device = 0;
    }
    for (int i = 0; i < NUM_SOUNDS; i++) {
        SDL_free(sound_buffers[i]);
        sound_buffers[i] = NULL;
        free(config.sound_paths[i]);
        config.sound_paths[i] = NULL;
    }
}
