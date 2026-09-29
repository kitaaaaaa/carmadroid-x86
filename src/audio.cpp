#include "audio.h"
#include "cpu.h"
#include "elf_loader.h"
#include "hle/jni.h"
#include <SDL.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_map>

namespace audio {
namespace {

enum { INFO_SAMPLERATE = 0, INFO_DSPBUFFERLENGTH = 1, INFO_DSPNUMBUFFERS = 2, INFO_MIXERRUNNING = 3 };

std::atomic<bool> g_running{false};
std::thread g_thread;

void run() {
    Cpu cpu(900, 512 * 1024);
    Cpu::current = &cpu;
    const u32 get_info = loader::find_symbol("Java_org_fmod_FMODAudioDevice_fmodGetInfo");
    const u32 process = loader::find_symbol("Java_org_fmod_FMODAudioDevice_fmodProcess");
    if (!get_info || !process) { LOGE("audio: FMOD JNI entry points missing"); return; }
    const u32 env = jni::env();
    const u32 self = jni::new_object("org/fmod/FMODAudioDevice");

    SDL_AudioDeviceID dev = 0;
    u32 buffer = 0, buffer_bytes = 0, jbuffer = 0;
    u32 queue_limit = 0;

    while (g_running) {
        if (!dev) {
            int rate = (int)cpu.call(get_info, {env, self, INFO_SAMPLERATE});
            if (rate <= 0) { SDL_Delay(100); continue; }
            u32 dsp_len = cpu.call(get_info, {env, self, INFO_DSPBUFFERLENGTH});
            u32 dsp_num = cpu.call(get_info, {env, self, INFO_DSPNUMBUFFERS});
            buffer_bytes = dsp_len * 2 * 2;  // stereo, 16-bit
            buffer = mem::calloc(1, buffer_bytes);
            jbuffer = jni::new_direct_buffer(buffer, buffer_bytes);
            SDL_AudioSpec want{}, have{};
            want.freq = rate;
            want.format = AUDIO_S16LSB;
            want.channels = 2;
            want.samples = 1024;
            dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
            if (!dev) { LOGE("audio: SDL_OpenAudioDevice failed: %s", SDL_GetError()); return; }
            // Keep roughly FMOD's own buffering (dsp_num blocks) queued ahead.
            queue_limit = buffer_bytes * std::max<u32>(dsp_num, 2);
            SDL_PauseAudioDevice(dev, 0);
            LOGI("audio: %d Hz, DSP buffer %u samples x %u", rate, dsp_len, dsp_num);
        }
        if (cpu.call(get_info, {env, self, INFO_MIXERRUNNING}) != 1) { SDL_Delay(10); continue; }
        while (g_running && SDL_GetQueuedAudioSize(dev) > queue_limit) SDL_Delay(1);
        cpu.call(process, {env, self, jbuffer});
        SDL_QueueAudio(dev, mem::ptr(buffer), buffer_bytes);
    }
    if (dev) SDL_CloseAudioDevice(dev);
}

}  // namespace

int g_sample_rate = 48000;

static void limit_skid_sound_rate() {
    // Sound_Play_SkidMark(VehicleSounds*, Lump*, float intensity, Vehicle*) runs once per frame. When
    // skidding eases off it fades the squeal by a fixed factor per call, and when skidding resumes it
    // restarts the sample at a new random pitch. At unlocked frame rates the fade completes almost
    // instantly, so the squeal restarts many times a second: warbling pitch and digital-sounding
    // clicks/beeps. Running it at most 60 times a second per car restores the phone behaviour.
    static u32 orig = 0;
    const u32 fn = loader::find_symbol("_Z19Sound_Play_SkidMarkP13VehicleSoundsPN2BZ4LumpEfP7Vehicle");
    if (!fn) { LOGE("audio: Sound_Play_SkidMark not found"); return; }
    orig = hle::hook_function(fn, "Sound_Play_SkidMark", [](Cpu& c) {
        static std::mutex m;
        static std::unordered_map<u64, u64> last_ms;  // (VehicleSounds*, Lump*) -> last update time
        const u64 now = SDL_GetTicks64();
        const u64 key = ((u64)c.r(0) << 32) | c.r(1);
        {
            std::lock_guard<std::mutex> l(m);
            u64& t = last_ms[key];
            if (now - t < 16) return;  // skip this frame (void function)
            t = now;
        }
        c.call(orig, {c.r(0), c.r(1), c.r(2), c.r(3)});
    });
}

void apply_patches() {
    limit_skid_sound_rate();
    // The game leaves FMOD at its Android default (24 kHz, linear resampling). Just before FMOD
    // initialises, request a higher mix rate and spline resampling.
    if (g_sample_rate <= 0) return;
    static u32 init_orig = 0;
    const u32 set_format = loader::find_symbol("_ZN4FMOD6System17setSoftwareFormatEi17FMOD_SOUND_FORMATii18FMOD_DSP_RESAMPLER");
    const u32 init = loader::find_symbol("_ZN4FMOD6System4initEijPv");
    if (!set_format || !init) { LOGE("audio: FMOD functions not found"); return; }
    init_orig = hle::hook_function(init, "FMOD::System::init", [set_format](Cpu& c) {
        const u32 self = c.r(0), maxch = c.r(1), flags = c.r(2), extra = c.r(3);
        // setSoftwareFormat(samplerate, FMOD_SOUND_FORMAT_PCM16, numoutputchannels=0 (default),
        //                   maxinputchannels=6, FMOD_DSP_RESAMPLER_SPLINE)
        u32 r = c.call(set_format, {self, (u32)g_sample_rate, 2, 0, 6, 3});
        LOGI("audio: FMOD mix rate %d Hz, spline resampling (result %u)", g_sample_rate, r);
        c.ret(c.call(init_orig, {self, maxch, flags, extra}));
    });
}

void start() {
    if (g_running.exchange(true)) return;
    g_thread = std::thread(run);
}

void stop() {
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace audio
