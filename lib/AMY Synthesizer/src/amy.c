// DAn Ellis and Brian Whitman
// brian@variogr.am / dan.ellis@gmail.com

#include "amy.h"
#include "delay.h"

#ifdef AMY_DEBUG

const char* profile_tag_name(enum itags tag) {
    switch (tag) {
        case RENDER_OSC_WAVE: return "RENDER_OSC_WAVE";
        case COMPUTE_BREAKPOINT_SCALE: return "COMPUTE_BREAKPOINT_SCALE";
        case HOLD_AND_MODIFY: return "HOLD_AND_MODIFY";
        case FILTER_PROCESS: return "FILTER_PROCESS";
        case FILTER_PROCESS_STAGE0: return "FILTER_PROCESS_STAGE0";
        case FILTER_PROCESS_STAGE1: return "FILTER_PROCESS_STAGE1";
        case ADD_DELTA_TO_QUEUE: return "ADD_DELTA_TO_QUEUE";
        case AMY_ADD_DELTA: return "AMY_ADD_DELTA";
        case PLAY_DELTA: return "PLAY_DELTA";
        case MIX_WITH_PAN: return "MIX_WITH_PAN";
        case AMY_RENDER: return "AMY_RENDER";
        case AMY_EXECUTE_DELTAS: return "AMY_EXECUTE_DELTAS";
        case AMY_FILL_BUFFER: return "AMY_FILL_BUFFER";
        case RENDER_LUT_FM: return "RENDER_LUT_FM";
        case RENDER_LUT_FB: return "RENDER_LUT_FB";
        case RENDER_LUT: return "RENDER_LUT";
        case RENDER_LUT_CUB: return "RENDER_LUT_CUB";
        case RENDER_LUT_FM_FB: return "RENDER_LUT_FM_FB";
        case RENDER_LPF_LUT: return "RENDER_LPF_LUT";
        case DSPS_BIQUAD_F32_ANSI_SPLIT_FB: return "DSPS_BIQUAD_F32_ANSI_SPLIT_FB";
        case DSPS_BIQUAD_F32_ANSI_SPLIT_FB_TWICE: return "DSPS_BIQUAD_F32_ANSI_SPLIT_FB_TWICE";
        case DSPS_BIQUAD_F32_ANSI_COMMUTED: return "DSPS_BIQUAD_F32_ANSI_COMMUTED";
        case PARAMETRIC_EQ_PROCESS: return "PARAMETRIC_EQ_PROCESS";
        case HPF_BUF: return "HPF_BUF";
        case SCAN_MAX: return "SCAN_MAX";
        case DSPS_BIQUAD_F32_ANSI: return "DSPS_BIQUAD_F32_ANSI";
        case BLOCK_NORM: return "BLOCK_NORM";
        case CALIBRATE: return "CALIBRATE";
        case AMY_ESP_FILL_BUFFER: return "AMY_ESP_FILL_BUFFER";
        case NO_TAG: return "NO_TAG";
   }
   return "ERROR";
}
struct profile profiles[NO_TAG];
uint64_t profile_start_us = 0;

#ifdef ESP_PLATFORM
#include "esp_timer.h"
int64_t amy_get_us() { return esp_timer_get_time(); }
#elif defined PICO_ON_DEVICE
int64_t amy_get_us() { return to_us_since_boot(get_absolute_time()); }
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
int64_t amy_get_us() {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (int64_t)(count.QuadPart * 1000000LL / freq.QuadPart);
}
#else
#include <sys/time.h>
int64_t amy_get_us() { struct timeval tv; gettimeofday(&tv,NULL); return tv.tv_sec*(uint64_t)1000000+tv.tv_usec; }
#endif

void amy_profiles_init() { 
    for(uint8_t i=0;i<NO_TAG;i++) { AMY_PROFILE_INIT(i) } 
} 
void amy_profiles_print() { for(uint8_t i=0;i<NO_TAG;i++) { AMY_PROFILE_PRINT(i) } amy_profiles_init(); }
#else
void amy_profiles_init()  {}
void amy_profiles_print() {}
#endif


// Use the tiny built-in PCM set by default across platforms.
#include "pcm_tiny.h"

#include "clipping_lookup_table.h"


// Set up the mutex for accessing the queue during rendering (for multicore)

#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#include <emscripten/wasm_worker.h>
emscripten_lock_t amy_queue_lock = EMSCRIPTEN_LOCK_T_STATIC_INITIALIZER;
void amy_grab_lock() {
    emscripten_lock_busyspin_wait_acquire(&amy_queue_lock, 100);
}
void amy_release_lock() {
    emscripten_lock_release(&amy_queue_lock);
}
void amy_init_lock() {
}

#elif defined _WIN32
CRITICAL_SECTION amy_queue_lock;
void amy_grab_lock() {
    EnterCriticalSection(&amy_queue_lock);
}
void amy_release_lock() {
    LeaveCriticalSection(&amy_queue_lock);
}
void amy_init_lock() {
    InitializeCriticalSection(&amy_queue_lock);
}
#elif defined _POSIX_THREADS
pthread_mutex_t amy_queue_lock;
void amy_grab_lock() {
    pthread_mutex_lock(&amy_queue_lock);
}
void amy_release_lock() {
    pthread_mutex_unlock(&amy_queue_lock);
}
void amy_init_lock() {
    pthread_mutex_init(&amy_queue_lock, NULL);
}
#elif defined ESP_PLATFORM

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
SemaphoreHandle_t amy_queue_lock;

void amy_grab_lock() {
    xSemaphoreTake(amy_queue_lock, portMAX_DELAY);
}
void amy_release_lock() {
    xSemaphoreGive( amy_queue_lock );
}
void amy_init_lock() {
    amy_queue_lock = xSemaphoreCreateMutex();
}
#else

void amy_grab_lock() {
}
void amy_release_lock() {
}
void amy_init_lock() {
}

#endif



// Global state 
global_state_t amy_global;
// set of deltas for the fifo to be played
struct delta * deltas;
// state per osc as multi-channel synthesizer that the scheduler renders into
struct synthinfo ** synth;
// envelope-modified per-osc state
struct mod_synthinfo ** msynth;

// Two mixing blocks, one per core of rendering
SAMPLE *fbl[AMY_MAX_CORES][AMY_NUM_BUSES];
SAMPLE *per_osc_fb[AMY_MAX_CORES][AMY_NUM_BUSES];
SAMPLE core_max[AMY_MAX_CORES];

// Wavefolder for T303 bus (bus 1): 1.0=dry, up to ~8.0 for multiple folds.
// Set from audio_engine.cpp via audioT303Wavefold(); applied before final bus mix.
float amy_wavefold_gain = 1.0f;
// When > 0.5, fold only the positive half (TRI2 asymmetric fold for bass retention).
float amy_wavefold_pos_only = 0.0f;
// Global wavefolder on bus 0 (REP FX): 1.0=dry. gain = 1/threshold. Applied after bus merge.
float amy_wavefold_gain_bus0 = 1.0f;

// Global resonant LPF on bus 0 (FILT FX, Typ=LADDER). Set from audio_engine.cpp via
// audioSetLadderFilter(); applied after the bus-0 wavefolder, before per-bus FX
// (EQ/chorus/reverb/delay). AMY's own filter types (FILTER_LPF/HPF/BPF/LPF24) are
// plain linear biquads (LPF24 is just the LPF biquad run twice) with no resonant
// peak or saturation modeling — this is a separate, custom two-cascade design (see
// the bus-0 ladder block below, amy_ladder_on > 0.5f) with a feedforward-only
// resonance bump instead, chosen specifically because it has no feedback loop and
// so cannot self-oscillate or get stuck ringing regardless of amy_ladder_resonance.
float amy_ladder_on        = 0.0f;
float amy_ladder_cutoff    = 20000.0f;
float amy_ladder_resonance = 0.0f;
static float amy_ladder_stage[AMY_NCHANS][4];

// Global bus-0 ring modulator (RINGMOD FX): multiplies the signal by a sine carrier.
// Set from audio_engine.cpp via audioSetRingmod(); applied right after the bus-0
// wavefolder (same "texture" family) — mix blends carrier-multiplied signal back
// with the dry signal so low mix values give a subtle metallic sheen rather than
// full inharmonic ring-mod clang.
float amy_ringmod_on    = 0.0f;
float amy_ringmod_freq  = 200.0f;   // carrier Hz
float amy_ringmod_mix   = 0.5f;     // 0=dry, 1=fully ring-modulated
static float amy_ringmod_phase = 0.0f;

// Global bus-0 compressor (COMPRESSOR FX): feedforward peak-envelope-follower gain
// reduction, applied last (after the ladder filter) so it acts as a final glue/
// limiting stage over everything else on the bus. Set from audio_engine.cpp via
// audioSetCompressor(). Threshold is linear (matches this bus's own headroom, no
// dBFS calibration needed since nothing else on this bus is calibrated to dBFS
// either); ratio 1.0=no compression, higher=more limiting.
float amy_comp_on        = 0.0f;
float amy_comp_threshold = 0.5f;
float amy_comp_ratio     = 4.0f;
static float amy_comp_env = 0.0f;

// DRIVE FX: consolidated bus-0 distortion (GrvEP), replacing separate OVERDRIVE/
// DISTORT FX that were each just a resonant filter sweep with no actual
// waveshaping at all ("ressemble juste à un LPF"). One engine, 4 selectable
// algorithms sharing the same drive/tone/level trio (type: 0=Overdrive gentle
// tanh, 1=Distort harder saturating exponential, 2=Fuzz hard clip, 3=Destrukt
// foldback) plus a shared post-saturation brightness tilt (tone). Set from
// audio_engine.cpp via audioSetDrive().
float amy_drive_on    = 0.0f;
float amy_drive_amt   = 0.0f;   // 0..1
float amy_drive_tone  = 0.5f;   // 0..1, 0=bright, 1=dark
float amy_drive_level = 1.0f;   // 0..~1.5
float amy_drive_type  = 0.0f;   // 0=Overdrive,1=Distort,2=Fuzz,3=Destrukt
static float amy_drive_tone_lp[AMY_NCHANS] = {0};


// RESECHO FX: bespoke delay + filter-IN-THE-FEEDBACK-PATH engine, ported from a sibling
// project's design. This is a SEPARATE mono delay line (not shared with DELAY) with a
// Chamberlin state-variable filter inside the feedback loop and feedback allowed much
// closer to unity, so it can genuinely "ring" for a long time and build an evolving
// texture — a hard clamp on the fed-back sample keeps that bounded/stable instead of
// truly blowing up even at extreme settings. Mono in, mono wet added equally to both
// channels (matches the ported design, which used one delay buffer).
// The feedback tap used to be the SVF's BANDPASS output (`band`), which concentrates
// all resonant buildup at one specific frequency (`amy_resoecho_centre`, "Tone" in the
// UI) — reported as sounding like it only rings at one arbitrary pitch rather than
// resonating across the spectrum. It now taps the SVF's LOWPASS output (`low`) instead:
// broadband/comb-filter-style resonance across the whole passband below the (now
// renamed) damping cutoff, only progressively darkened above it — see
// amy_apply_resoecho()'s own comment.
float    amy_resoecho_on            = 0.0f;
float    amy_resoecho_level         = 0.0f;    // wet mix amount
float    amy_resoecho_feedback      = 0.0f;    // 0..~0.97
// Damping cutoff, Hz (was a resonant bandpass CENTRE frequency — the feedback tap is now
// the SVF's lowpass output, not its bandpass, see amy_apply_resoecho()'s own comment: the
// resonance is broadband/comb-filter-like across the whole passband below this cutoff,
// not concentrated at one pitch). Higher = brighter/more of the spectrum resonates.
float    amy_resoecho_centre        = 800.0f;
float    amy_resoecho_reso          = 2.0f;    // higher = more resonant peak
uint32_t amy_resoecho_delay_samples = 4410;    // BPM-synced, set from audio_engine.cpp
static SAMPLE*  amy_resoecho_buf     = NULL;
static uint32_t amy_resoecho_buf_len = 0;      // power of 2
static uint32_t amy_resoecho_write   = 0;
static float    amy_resoecho_svf_low  = 0.0f;
static float    amy_resoecho_svf_band = 0.0f;

// FILT FX (GrvEP), Typ=LPF/HPF/BPF: a bus-0 Chamberlin state-variable filter (same
// stable, no-feedback-loop-overflow design already used for RESECHO's own resonant
// filter). This — together with LADDER (amy_ladder_*, above/below) — is what makes
// FILT a genuine bus-0 chain participant: previously LPF/HPF/BPF were per-oscillator
// only (applied at the very source, upstream of every bus effect), which meant they
// could never be positioned relative to RESECHO/DRIVE/etc. by activation order the
// way the user wanted ("je veux juste que les FX se placent les uns à la suite des
// autres selon l'ordre d'activation"). Set from audio_engine.cpp via
// audioSetFiltBus(); dispatched by activation order in amy_apply_ordered_fx() below,
// not a fixed position.
float amy_filt_on      = 0.0f;
float amy_filt_type    = 0.0f;  // 0=LPF, 1=HPF, 2=BPF (LADDER uses amy_ladder_* instead)
float amy_filt_cutoff  = 2000.0f;
float amy_filt_reso    = 1.0f;
static float amy_filt_svf_low[AMY_NCHANS]  = {0};
static float amy_filt_svf_band[AMY_NCHANS] = {0};

// Public pointer to recently-emitted waveform block.
output_sample_type * amy_out_block = NULL;
// Audio input blocks. Filled by the audio implementation before rendering.
// For live audio input from a codec, AUDIO_IN0 / 1
output_sample_type * amy_in_block;
// For generated audio streams, AUDIO_EXT0 / 1
output_sample_type * amy_external_in_block;
// output_block -- what gets sent to the dac -- -32768...32767 (int16 le)
output_sample_type * output_block_0;
output_sample_type * output_block_1;
output_sample_type * output_block;




#ifndef MALLOC_CAPS_DEFINED
  #define MALLOC_CAPS_DEFINED
  void * malloc_caps(uint32_t size, uint32_t flags) {
    void *result;
  #ifdef ESP_PLATFORM
    result = heap_caps_malloc(size, flags);
  #else
    // ignore flags
    result = malloc(size);
  #endif
    //if (size > 400000) abort();
    //fprintf(stderr, "malloc(%ld) @0x%lx\n", size, result);
    return result;
  }
#endif


void dealloc_echo_delay_lines(uint8_t bus) {
    for (int c = AMY_NCHANS - 1; c >= 0; --c) {
        if (amy_global.bus[bus]->echo.echo_delay_lines[c])
            free_delay_line(amy_global.bus[bus]->echo.echo_delay_lines[c]);
        amy_global.bus[bus]->echo.echo_delay_lines[c] = NULL;
    }
}

bool alloc_echo_delay_lines(uint8_t bus, uint32_t max_delay_samples) {
    bool success = true;
    for (int c = 0; c < AMY_NCHANS; ++c) {
        delay_line_t *delay_line = new_delay_line(max_delay_samples, 0, amy_global.config.ram_caps_delay);
        if (delay_line) {
            amy_global.bus[bus]->echo.echo_delay_lines[c] = delay_line;
        } else {
            success = false;
            break;
        }
    }
    if (!success) {
        fprintf(stderr, "unable to alloc echo of %d samples\n", (int)max_delay_samples);
        dealloc_echo_delay_lines(bus);
        return false;
    }
    return true;
}

uint32_t enclosing_power_of_2(uint32_t n) {
    uint32_t result = 1;
    while (result < n)  result <<= 1;
    return result;
}

// RESECHO FX config — see amy_resoecho_* declarations above. Lazily allocates a
// fixed-size buffer on first use (matches config_echo's own 700ms/32768-sample
// headroom, so no realloc is ever needed again as delay_ms changes with BPM — only
// the read offset amy_resoecho_delay_samples moves).
void amy_resoecho_config(float level, float feedback, float centre_hz, float resonance,
                          float delay_ms, int on) {
    if (amy_resoecho_buf == NULL) {
        amy_resoecho_buf_len = enclosing_power_of_2((uint32_t)(0.7f * AMY_SAMPLE_RATE) + 1);
        amy_resoecho_buf = (SAMPLE*)malloc_caps(amy_resoecho_buf_len * sizeof(SAMPLE), amy_global.config.ram_caps_delay);
        if (amy_resoecho_buf == NULL) { amy_resoecho_buf_len = 0; return; }
        for (uint32_t i = 0; i < amy_resoecho_buf_len; i++) amy_resoecho_buf[i] = F2S(0.0f);
    }
    amy_resoecho_level    = level;
    amy_resoecho_feedback = feedback;
    amy_resoecho_centre   = centre_hz;
    amy_resoecho_reso     = resonance;
    uint32_t ds = (uint32_t)(delay_ms * (AMY_SAMPLE_RATE / 1000.0f));
    if (ds < 1) ds = 1;
    if (ds >= amy_resoecho_buf_len) ds = amy_resoecho_buf_len - 1;
    amy_resoecho_delay_samples = ds;
    amy_resoecho_on = on ? 1.0f : 0.0f;
}

void config_echo(uint8_t bus, float level, float delay_ms, float max_delay_ms, float feedback, float filter_coef) {
    if (AMY_IS_UNSET(level)) level = S2F(amy_global.bus[bus]->echo.level);
    if (AMY_IS_UNSET(delay_ms)) delay_ms = (amy_global.bus[bus]->echo.delay_samples + 0.5f) / (AMY_SAMPLE_RATE / 1000.f);
    if (AMY_IS_UNSET(max_delay_ms)) max_delay_ms = (amy_global.bus[bus]->echo.max_delay_samples + 0.5f) / (AMY_SAMPLE_RATE / 1000.f);
    if (AMY_IS_UNSET(feedback)) feedback = S2F(amy_global.bus[bus]->echo.feedback);
    if (AMY_IS_UNSET(filter_coef)) filter_coef = S2F(amy_global.bus[bus]->echo.filter_coef);

    uint32_t delay_samples = (uint32_t)(delay_ms / 1000.f * AMY_SAMPLE_RATE);
    uint32_t max_delay_samples = enclosing_power_of_2((uint32_t)(max_delay_ms / 1000.f * AMY_SAMPLE_RATE));
    // Remember this value for max_delay_samples even if we don't allocate on this call, so a later UNSET call will pick it up.
    amy_global.bus[bus]->echo.max_delay_samples = max_delay_samples;
    //fprintf(stderr, "config_echo: bus %d delay=%.3f ms / %d samps max_delay=%.3f ms / %d samps echo.max_delay_samples=%d\n", bus, delay_ms, delay_samples, max_delay_ms, max_delay_samples, amy_global.bus[bus]->echo.max_delay_samples);

    if (level > 0) {
        if (amy_global.bus[bus]->echo.echo_delay_lines[0] == NULL) {
            // Delay line len must be power of 2.
            if (!alloc_echo_delay_lines(bus, max_delay_samples)) return;
            //fprintf(stderr, "config_echo: max_delay_samples=%d\n", max_delay_samples);
        }
        // Apply delay.  We have to stay 1 sample less than delay line length for FIR EQ delay.
        if (delay_samples > amy_global.bus[bus]->echo.max_delay_samples - 1) delay_samples = amy_global.bus[bus]->echo.max_delay_samples - 1;
        for (int c = 0; c < AMY_NCHANS; ++c) {
            amy_global.bus[bus]->echo.echo_delay_lines[c]->fixed_delay = delay_samples;
        }
    }
    amy_global.bus[bus]->echo.level = F2S(level);
    amy_global.bus[bus]->echo.delay_samples = delay_samples;
    // Filter is IIR [1, filter_coef] normalized for filter_coef > 0 (LPF), or FIR [1, filter_coef] normalized for filter_coef < 0 (HPF).
    if (filter_coef > 0.99)  filter_coef = 0.99;  // Avoid unstable filters.
    amy_global.bus[bus]->echo.filter_coef = F2S(filter_coef);
    // FIR filter potentially has gain > 1 for high frequencies, so discount the loop feedback to stop things exploding.
    if (filter_coef < 0)  feedback /= 1.f - filter_coef;
    amy_global.bus[bus]->echo.feedback = F2S(feedback);
    //fprintf(stderr, "config_echo: delay_samples=%d level=%.3f feedback=%.3f filter_coef=%.3f fc0=%.3f\n", delay_samples, level, feedback, filter_coef, S2F(echo.filter_coef));
}

void dealloc_chorus_delay_lines(uint8_t bus) {
    for(int c = AMY_NCHANS - 1; c >= 0; --c) {
        if (amy_global.bus[bus]->chorus.chorus_delay_lines[c]) free_delay_line(amy_global.bus[bus]->chorus.chorus_delay_lines[c]);
        amy_global.bus[bus]->chorus.chorus_delay_lines[c] = NULL;
    }
    free(amy_global.bus[bus]->chorus.delay_mod);
    amy_global.bus[bus]->chorus.delay_mod = NULL;
}

void alloc_chorus_delay_lines(uint8_t bus) {
    amy_global.bus[bus]->chorus.delay_mod = (SAMPLE *)malloc_caps(sizeof(SAMPLE) * AMY_BLOCK_SIZE, amy_global.config.ram_caps_delay);
    bool success = true;
    for(int c = 0; c < AMY_NCHANS; ++c) {
        delay_line_t *delay_line = new_delay_line(DELAY_LINE_LEN, DELAY_LINE_LEN / 2, amy_global.config.ram_caps_delay);
        if (delay_line) {
            amy_global.bus[bus]->chorus.chorus_delay_lines[c] = delay_line;
        } else {
            success = false;
            break;
        }
    }
    if (!success) {
        fprintf(stderr, "unable to alloc chorus of %d samples\n", (int)DELAY_LINE_LEN);
        dealloc_chorus_delay_lines(bus);
    }
}

void config_chorus(uint8_t bus, float level, uint16_t max_delay, float lfo_freq, float depth) {
    if (AMY_IS_UNSET(level)) level = S2F(amy_global.bus[bus]->chorus.level);
    if (AMY_IS_UNSET(max_delay)) max_delay = amy_global.bus[bus]->chorus.max_delay;
    if (AMY_IS_UNSET(lfo_freq)) lfo_freq = amy_global.bus[bus]->chorus.lfo_freq;
    if (AMY_IS_UNSET(depth)) depth = amy_global.bus[bus]->chorus.depth;
    //fprintf(stderr, "config_chorus: osc %d level %.3f max_del %d lfo_freq %.3f depth %.3f\n",
    //        CHORUS_MOD_SOURCE + bus, level, max_delay, lfo_freq, depth);
    if (level > 0) {
        // only allocate delay lines if chorus is more than inaudible.
        if (amy_global.bus[bus]->chorus.chorus_delay_lines[0] == NULL) {
            alloc_chorus_delay_lines(bus);
        }
        // apply max_delay.
        for (int chan=0; chan<AMY_NCHANS; ++chan) {
            //chorus_delay_lines[chan]->max_delay = max_delay;
            amy_global.bus[bus]->chorus.chorus_delay_lines[chan]->fixed_delay = (int)max_delay / 2;
        }
        // Configure the LFO osc.
        ensure_osc_allocd(CHORUS_MOD_SOURCE + bus, NULL);
        // if we're turning on for the first time, start the oscillator.
        if (synth[CHORUS_MOD_SOURCE + bus]->status == SYNTH_OFF) {  //chorus.level == 0) {
            // Setup chorus oscillator.
            synth[CHORUS_MOD_SOURCE + bus]->logfreq_coefs[COEF_NOTE] = 0;  // Turn off default.
            synth[CHORUS_MOD_SOURCE + bus]->logfreq_coefs[COEF_BEND] = 0;  // Turn off default.
            synth[CHORUS_MOD_SOURCE + bus]->amp_coefs[COEF_VEL] = 0;  // Turn off default.
            synth[CHORUS_MOD_SOURCE + bus]->amp_coefs[COEF_EG0] = 0;  // Turn off default.
            synth[CHORUS_MOD_SOURCE + bus]->wave = TRIANGLE;
            osc_note_on(CHORUS_MOD_SOURCE + bus, lfo_freq);
            // Stop us from doing this again.
            synth[CHORUS_MOD_SOURCE + bus]->status = SYNTH_IS_MOD_SOURCE;
        }
        // apply depth, lfo_freq
        synth[CHORUS_MOD_SOURCE + bus]->amp_coefs[COEF_CONST] = depth;
        synth[CHORUS_MOD_SOURCE + bus]->logfreq_coefs[COEF_CONST] = logfreq_of_freq(lfo_freq);
    }
    amy_global.bus[bus]->chorus.max_delay = max_delay;
    amy_global.bus[bus]->chorus.level = F2S(level);
    amy_global.bus[bus]->chorus.lfo_freq = lfo_freq;
    amy_global.bus[bus]->chorus.depth = depth;
}

bool alloc_reverb_delay_lines(uint8_t bus) {
    if (amy_global.bus[bus]->reverb.rev == NULL)
        amy_global.bus[bus]->reverb.rev = new_reverb();
    return init_stereo_reverb(amy_global.bus[bus]->reverb.rev);
}

void dealloc_reverb_delay_lines(uint8_t bus) {
    if (amy_global.bus[bus]->reverb.rev != NULL) {
        deinit_stereo_reverb(amy_global.bus[bus]->reverb.rev);
        delete_reverb(amy_global.bus[bus]->reverb.rev);
    }
}

void config_reverb(uint8_t bus, float level, float liveness, float damping, float xover_hz) {
    if (AMY_IS_UNSET(level)) level = S2F(amy_global.bus[bus]->reverb.level);
    if (AMY_IS_UNSET(liveness)) liveness = amy_global.bus[bus]->reverb.liveness;
    if (AMY_IS_UNSET(damping)) damping = amy_global.bus[bus]->reverb.damping;
    if (AMY_IS_UNSET(xover_hz)) xover_hz = amy_global.bus[bus]->reverb.xover_hz;
    if (level > 0) {
        //printf("config_reverb: level %f liveness %f xover %f damping %f\n",
        //      level, liveness, xover_hz, damping);
        if (amy_global.bus[bus]->reverb.level == 0) {
            if (!alloc_reverb_delay_lines(bus)) {
                amy_global.bus[bus]->reverb.level = 0;
                return;
            }
        }
        config_stereo_reverb(amy_global.bus[bus]->reverb.rev, liveness, xover_hz, damping);
    }
    amy_global.bus[bus]->reverb.level = F2S(level);
    amy_global.bus[bus]->reverb.liveness = liveness;
    amy_global.bus[bus]->reverb.damping = damping;
    amy_global.bus[bus]->reverb.xover_hz = xover_hz;
}


int8_t check_init(amy_err_t (*fn)(), const char *name) {
    //fprintf(stderr,"starting %s: ", name);
    const amy_err_t ret = (*fn)();
    if(ret != AMY_OK) {
        fprintf(stderr,"[error:%i]\n", ret);
        return -1;
    }
    //fprintf(stderr,"[ok]\n");
    return 0;
}

#ifdef DEBUG_STACK

static uint8_t *stack_baseline = NULL;

uint8_t *get_sp() {
    uint8_t d[64];
    return (uint8_t *)&d;
    //return NULL;
}

int peek_stack(const char *tag) {
    if (stack_baseline == NULL) {
        stack_baseline = get_sp();
    }
    int stack_depth = (int)(stack_baseline - get_sp());
    fprintf(stderr, "stack: '%s': %d\n", tag, stack_depth);
    return stack_depth;
}

#else  // !DEBUG_STACK

int peek_stack(const char *tag)  { return 0; }

#endif


void config_eq(uint8_t bus, SAMPLE eq_l, SAMPLE eq_m, SAMPLE eq_h) {
    amy_global.bus[bus]->eq.eq[0] = eq_l;
    amy_global.bus[bus]->eq.eq[1] = eq_m;
    amy_global.bus[bus]->eq.eq[2] = eq_h;
}


void bus_reset(uint8_t bus) {
    config_eq(bus, F2S(1.0f), F2S(1.0f), F2S(1.0f));
    filters_init(bus);
    reset_parametric(bus);

    if (AMY_HAS_CHORUS) config_chorus(bus, CHORUS_DEFAULT_LEVEL, CHORUS_DEFAULT_MAX_DELAY, CHORUS_DEFAULT_LFO_FREQ, CHORUS_DEFAULT_MOD_DEPTH);
    if (AMY_HAS_REVERB) config_reverb(bus, REVERB_DEFAULT_LEVEL, REVERB_DEFAULT_LIVENESS, REVERB_DEFAULT_DAMPING, REVERB_DEFAULT_XOVER_HZ);
    if (AMY_HAS_ECHO)   config_echo(bus, S2F(ECHO_DEFAULT_LEVEL), ECHO_DEFAULT_DELAY_MS, ECHO_DEFAULT_MAX_DELAY_MS, S2F(ECHO_DEFAULT_FEEDBACK), S2F(ECHO_DEFAULT_FILTER_COEF));
}

void buses_reset() {
    for (int i = 0; i < AMY_NUM_BUSES; ++i) {
        bus_reset(i);
    }
}

int8_t global_init(amy_config_t c) {
    peek_stack("init");
    amy_global.config = c;
    amy_global.i2s_is_in_background = 0;
    amy_global.delta_queue = NULL;
    amy_global.delta_qsize = 0;
    for (int bus = 0; bus < AMY_NUM_BUSES; ++bus)
        amy_global.volume[bus] = 1.0f;
    amy_global.pitch_bend = 0;
    amy_global.latency_ms = 0;
    amy_global.tempo = 108.0; 
    amy_global.pitch_bend = 0;
    amy_global.transfer_flag = AMY_TRANSFER_TYPE_NONE;
    amy_global.transfer_storage = NULL;
    amy_global.transfer_length_bytes = 0;
    amy_global.transfer_stored_bytes = 0;
    amy_global.transfer_file_handle = 0;
    amy_global.transfer_filename[0] = '\0';
    amy_global.debug_flag = 0;
    amy_global.highest_bus = 0;
    amy_global.hpf_state = 0;
    amy_global.sequencer_tick_count = 0;
    amy_global.next_amy_tick_us = 0;
    amy_global.us_per_tick = 0;
    amy_global.sequence_entry_ll_start = NULL;

    struct bus_state *bus_configs = malloc_caps(sizeof(struct bus_state) * AMY_NUM_BUSES,
                                                amy_global.config.ram_caps_synth);
    bzero(bus_configs, sizeof(struct bus_state) * AMY_NUM_BUSES);
    for (int i = 0; i < AMY_NUM_BUSES; ++i) {
        amy_global.bus[i] = bus_configs;
        ++bus_configs;
    }
    buses_reset();
    
    amy_init_lock();

    return 0;
}

void global_deinit(void) {
    for (int bus = 0; bus < AMY_NUM_BUSES; ++bus)  filters_deinit(bus);
    free(amy_global.bus[0]);
}

// Convert to and from the log-frequency scale.
// A log-frequency scale is good for summing control inputs.
float logfreq_of_freq(float freq) {
    // logfreq is defined as log_2(freq / 8.18 Hz)
    //if (freq==0) return ZERO_HZ_LOG_VAL;
    // Actually, special-case zero to mean middle C, for convenience.
    if (freq==0) return 0;  // i.e. == logfreq_of_freq(ZERO_LOGFREQ_IN_HZ).
    return log2f(freq / ZERO_LOGFREQ_IN_HZ);
}

float freq_of_logfreq(float logfreq) {
    if (logfreq==ZERO_HZ_LOG_VAL) return 0;
    return ZERO_LOGFREQ_IN_HZ * exp2f(logfreq);
}

float freq_for_midi_note(float midi_note) {
    return 440.0f*powf(2.f, (midi_note - 69.0f) / 12.0f);
}

float logfreq_for_midi_note(float midi_note) {
    // TODO: Precompensate for EPS_FOR_LOG
    return (midi_note - ZERO_MIDI_NOTE) / 12.0f;
}

float midi_note_for_logfreq(float logfreq) {
    return 12.0f * logfreq + ZERO_MIDI_NOTE;
}



void add_delta_to_queue(struct delta *d, struct delta **queue) {
    AMY_PROFILE_START(ADD_DELTA_TO_QUEUE)
    amy_grab_lock();

    // hack.  Update the (decorative) global queue size if we're adding to the global queue.
    if (queue == &amy_global.delta_queue)
        amy_global.delta_qsize++;

    struct delta *new_d = delta_get(d);

    // insert it into the sorted list for fast playback
    struct delta **pptr = queue;
    while(*pptr && d->time >= (*pptr)->time)
        pptr = &(*pptr)->next;
    new_d->next = *pptr;
    *pptr = new_d;

    amy_release_lock();
    AMY_PROFILE_STOP(ADD_DELTA_TO_QUEUE)

}

float map_60dB_to_01f(float lin) {
    // Map .001 to 0, 1 to 1 logarithmically.
    if (lin == 0) return -10.0f;
    float result = 1.0f + 0.10034333188799373f * log2f(lin);  // 0.100343 = 1 / (3 * log2(10))
    return result;
}

float map_01_to_60dBf(float log) {
    // Inverse of map_60dB_to_01f - Map (0, 1) to (.001, 1) exponentially
    if (log <= -10.0f) return 0;
    float result = exp2f((log - 1.0f) / 0.10034333188799373f);
    return result;
}

#define EVENT_TO_DELTA_F(FIELD, FLAG) if(AMY_IS_SET(e->FIELD)) { d.param=FLAG; d.data.f = e->FIELD; add_delta_to_queue(&d, queue); }
#define EVENT_TO_DELTA_I(FIELD, FLAG) if(AMY_IS_SET(e->FIELD)) { d.param=FLAG; d.data.i = e->FIELD; add_delta_to_queue(&d, queue); }
#define EVENT_TO_DELTA_WITH_BASEOSC(FIELD, FLAG)    if(AMY_IS_SET(e->FIELD)) { d.param=FLAG; d.data.i = e->FIELD + base_osc; if (FLAG != RESET_OSC && d.data.i < (uint32_t)AMY_OSCS + AMY_NUM_BUSES) ensure_osc_allocd(d.data.i, NULL); add_delta_to_queue(&d, queue);}
#define EVENT_TO_DELTA_LOG(FIELD, FLAG)             if(AMY_IS_SET(e->FIELD)) { d.param=FLAG; d.data.f = log2f(e->FIELD); add_delta_to_queue(&d, queue);}
#define EVENT_TO_DELTA_COEFS(FIELD, FLAG)  \
    for (int i = 0; i < NUM_COMBO_COEFS; ++i) \
        EVENT_TO_DELTA_F(FIELD[i], FLAG + i)
// Const freq coef is in Hz, rest are linear.
#define EVENT_TO_DELTA_COEFS_COEF0_SPECIAL(FIELD, FLAG, COEF0_FN)        \
    for (int i = 0; i < NUM_COMBO_COEFS; ++i) {      \
        if (AMY_IS_SET(e->FIELD[i]))  {              \
            d.param = FLAG + i;  \
            if (i == COEF_CONST)  \
                d.data.f = COEF0_FN(e->FIELD[i]);  \
            else \
                d.data.f = e->FIELD[i]; \
            add_delta_to_queue(&d, queue); \
        }    \
    }
#define EVENT_TO_DELTA_FREQ_COEFS(FIELD, FLAG) \
    EVENT_TO_DELTA_COEFS_COEF0_SPECIAL(FIELD, FLAG, logfreq_of_freq)

// Add a API facing event, convert into delta directly
void amy_event_to_deltas_queue(amy_event *e, uint16_t base_osc, struct delta **queue) {
    AMY_PROFILE_START(AMY_ADD_DELTA)
    struct delta d;
    peek_stack("event_to_deltas");
    // Synth defaults if not set, these are required for the delta struct
    d.time = e->time;
    d.osc = e->osc;
    if(AMY_IS_UNSET(e->osc)) { d.osc = 0; } 
    if(AMY_IS_UNSET(e->time)) { d.time = 0; } 

    // If this is a bus-directed event, use d->osc to store the bus number instead.
    bool bus_directed_command = false;
    if (AMY_IS_SET(e->eq_l) || AMY_IS_SET(e->eq_m) || AMY_IS_SET(e->eq_h)
        || AMY_IS_SET(e->echo_level) || AMY_IS_SET(e->echo_delay_ms) || AMY_IS_SET(e->echo_max_delay_ms) || AMY_IS_SET(e->echo_feedback) || AMY_IS_SET(e->echo_filter_coef)
        || AMY_IS_SET(e->chorus_level) || AMY_IS_SET(e->chorus_max_delay) || AMY_IS_SET(e->chorus_lfo_freq) || AMY_IS_SET(e->chorus_depth) 
        || AMY_IS_SET(e->reverb_level) || AMY_IS_SET(e->reverb_liveness) || AMY_IS_SET(e->reverb_damping) || AMY_IS_SET(e->reverb_xover_hz)) {
        if (AMY_IS_SET(e->osc))  fprintf(stderr, "** osc %d specific for bus-directed command, ignoring\n", e->osc);  // Can't at this moment be more specific about which command.
        // Store the target bus in d.osc.
        d.osc = AMY_IS_SET(e->bus) ? e->bus : AMY_DEFAULT_BUS;
        bus_directed_command = true;
        if (d.osc > amy_global.highest_bus) amy_global.highest_bus = d.osc;
    } else {
        // d.osc refers to an osc
        // First, adapt the osc in this event with base_osc offsets for voices
        d.osc += base_osc;
        // Ensure this osc has its synthinfo allocated.
        ensure_osc_allocd(d.osc, NULL);
    }

    // Voices / patches gets set up here 
    // you must set both voices & load_patch together to load a patch 
    if (AMY_IS_SET(e->voices[0]) || AMY_IS_SET(e->synth)) {
        if (AMY_IS_SET(e->patch_number) || AMY_IS_SET(e->num_voices) || AMY_IS_SET(e->oscs_per_voice)) {
            amy_execute_deltas();
            patches_load_patch(e);
        }
        // Execute any other commands in this event.
        patches_event_has_voices(e, queue);
        goto end;
    }

    // Is this, in fact, a non-load_patch or store_patch event that has patch_number set?
    // If so, add the event to the stored patch queue, not the execution queue.
    if (AMY_IS_SET(e->patch_number)) {
        queue = queue_for_patch_number(e->patch_number);
        //fprintf(stderr, "event added to patch %d: osc %d wave %d...\n", e->patch_number, e->osc, e->wave);
        if (queue == NULL) {
            // The patch number was invalid (e.g., not in user range 1024+), so we got no queue, so ignore the event.
            fprintf(stderr, "event ignored\n");
            goto end;
        }            
    }

    // Everything else only added to queue if set
    if (!bus_directed_command) {
        EVENT_TO_DELTA_I(bus, BUS)
        if (AMY_IS_SET(e->bus) && e->bus > amy_global.highest_bus)
            amy_global.highest_bus = e->bus;
    }
    EVENT_TO_DELTA_I(wave, WAVE)
    EVENT_TO_DELTA_I(preset, PRESET)
    EVENT_TO_DELTA_F(midi_note, MIDI_NOTE)
    EVENT_TO_DELTA_COEFS(amp_coefs, AMP)
    EVENT_TO_DELTA_FREQ_COEFS(freq_coefs, FREQ)
    EVENT_TO_DELTA_FREQ_COEFS(filter_freq_coefs, FILTER_FREQ)
    EVENT_TO_DELTA_COEFS(duty_coefs, DUTY)
    EVENT_TO_DELTA_COEFS(pan_coefs, PAN)
    EVENT_TO_DELTA_F(feedback, FEEDBACK)
    EVENT_TO_DELTA_F(trigger_phase, PHASE)
    EVENT_TO_DELTA_F(pitch_bend, PITCH_BEND)
    EVENT_TO_DELTA_I(latency_ms, LATENCY)
    EVENT_TO_DELTA_F(tempo, TEMPO)
    EVENT_TO_DELTA_LOG(ratio, RATIO)
    EVENT_TO_DELTA_F(resonance, RESONANCE)
    EVENT_TO_DELTA_I(portamento_ms, PORTAMENTO)
    EVENT_TO_DELTA_WITH_BASEOSC(chained_osc, CHAINED_OSC)
    EVENT_TO_DELTA_WITH_BASEOSC(reset_osc, RESET_OSC)
    EVENT_TO_DELTA_WITH_BASEOSC(mod_source, MOD_SOURCE)
    EVENT_TO_DELTA_I(note_source, NOTE_SOURCE)
    EVENT_TO_DELTA_I(filter_type, FILTER_TYPE)
    EVENT_TO_DELTA_I(algorithm, ALGORITHM)
    EVENT_TO_DELTA_F(eq_l, EQ_L)
    EVENT_TO_DELTA_F(eq_m, EQ_M)
    EVENT_TO_DELTA_F(eq_h, EQ_H)
    EVENT_TO_DELTA_F(echo_max_delay_ms, ECHO_MAX_DELAY_MS)  // set MAX_DELAY first
    EVENT_TO_DELTA_F(echo_level, ECHO_LEVEL)
    EVENT_TO_DELTA_F(echo_delay_ms, ECHO_DELAY_MS)
    EVENT_TO_DELTA_F(echo_feedback, ECHO_FEEDBACK)
    EVENT_TO_DELTA_F(echo_filter_coef, ECHO_FILTER_COEF)
    EVENT_TO_DELTA_F(chorus_max_delay, CHORUS_MAX_DELAY)   // set MAX_DELAY first
    EVENT_TO_DELTA_F(chorus_level, CHORUS_LEVEL)
    EVENT_TO_DELTA_F(chorus_lfo_freq, CHORUS_LFO_FREQ)
    EVENT_TO_DELTA_F(chorus_depth, CHORUS_DEPTH)
    EVENT_TO_DELTA_F(reverb_level, REVERB_LEVEL)
    EVENT_TO_DELTA_F(reverb_liveness, REVERB_LIVENESS)
    EVENT_TO_DELTA_F(reverb_damping, REVERB_DAMPING)
    EVENT_TO_DELTA_F(reverb_xover_hz, REVERB_XOVER_HZ)
    EVENT_TO_DELTA_I(eg_type[0], EG0_TYPE)
    EVENT_TO_DELTA_I(eg_type[1], EG1_TYPE)

    for (int bus = 0; bus < AMY_NUM_BUSES; ++bus)
        EVENT_TO_DELTA_F(volume[bus], VOLUME_BASE + bus)

    bool algo_ops_set = false;
    for (int i = 0; i < MAX_ALGO_OPS; ++i) {
        if(AMY_IS_SET(e->algo_source[i])) {
            algo_ops_set = true;
            break;
        }
    }
    if (algo_ops_set) {
        for(uint8_t i = 0; i < MAX_ALGO_OPS; i++) {
            d.param = ALGO_SOURCE_START + i;
            if (AMY_IS_SET(e->algo_source[i])) {
                d.data.i = e->algo_source[i] + base_osc;
            } else{
                d.data.i = e->algo_source[i];
            }
            add_delta_to_queue(&d, queue);
        }
    }

    uint32_t *bp_times_ms[MAX_BREAKPOINT_SETS] = {e->eg0_times, e->eg1_times};
    float *bp_values[MAX_BREAKPOINT_SETS] = {e->eg0_values, e->eg1_values};
    for (uint8_t i = 0; i < MAX_BREAKPOINT_SETS; i++) {
        // amy_parse_message sets bp_is_set for anything including an empty bp string.
        // Direct API calls can set typed breakpoint arrays without touching bp_is_set.
        bool bp_arrays_set = AMY_IS_SET(bp_times_ms[i][0]) || AMY_IS_SET(bp_values[i][0]);
        if(AMY_IS_SET(e->bp_is_set[i]) || bp_arrays_set) {
            int num_bps = 0;
            for (int j = 0; j < MAX_BREAKPOINTS; ++j) {
                if (AMY_IS_SET(bp_times_ms[i][j]) || AMY_IS_SET(bp_values[i][j])) {
                    num_bps = j + 1;
                }
            }
            for(uint8_t j = 0; j < num_bps; j++) {
                if(AMY_IS_SET(bp_times_ms[i][j])) {
                    d.param = BP_START + (j * 2) + (i * MAX_BREAKPOINTS * 2);
                    d.data.i = ms_to_samples((uint32_t)bp_times_ms[i][j]);
                    add_delta_to_queue(&d, queue);
                }
                if(AMY_IS_SET(bp_values[i][j])) {
                    d.param = BP_START + (j * 2 + 1) + (i * MAX_BREAKPOINTS * 2);
                    d.data.f = bp_values[i][j];
                    add_delta_to_queue(&d, queue);
                }
            }
            // Send an unset value as the last + 1 breakpoint time to indicate the end of the BP set.
            //if (num_bps < MAX_BREAKPOINTS) {
            //    d.param = BP_START + (num_bps * 2) + (i * MAX_BREAKPOINTS * 2);
            //    d.data.i = AMY_UNSET_VALUE(d.data.i);
            //    add_delta_to_queue(&d, queue);
            //}
            // If we do this, then you can't set one value in the middle of a BP set without
            // setting all the rest, because at this point we can't distinguish trailing
            // commas from a truncated list.
            // If we *don't* do this, you can't truncate an existing BP list, because the
            // trailing part you don't overwrite never gets removed.  However, it seems like
            // we don't have any use-cases where someone wants to curtail a BP list - but
            // we *do* have use-cases for wanting to change the sustain level without
            // re-specifying the release, so *don't* do this wins for now.
        }
    }

    // add this last -- this is a trigger, that if sent alongside osc setup parameters, you want to run after those

    EVENT_TO_DELTA_F(velocity, VELOCITY)

    if (AMY_IS_SET(e->patch_number)) {
        // If this was an event with a patch number, maybe we increased the number of oscs for this patch, update it.
         update_num_oscs_for_patch_number(e->patch_number);
    }


end:
    AMY_PROFILE_STOP(AMY_ADD_DELTA);

}


void reset_modosc(struct mod_synthinfo *pmsynth) {
        if (pmsynth != NULL) {
        pmsynth->last_duty = 0.5f;
        pmsynth->amp = 0;  // This matters for wave=PARTIAL, where msynth amp is effectively 1-frame delayed.
        pmsynth->last_amp = 0;
        pmsynth->logfreq = 0;
        pmsynth->filter_logfreq = 0;
        AMY_UNSET(pmsynth->last_filter_logfreq);
        pmsynth->duty = 0.5f;
        pmsynth->pan = 0.5f;
        pmsynth->feedback = F2S(0); //.996; todo ks feedback is v different from fm feedback
        pmsynth->resonance = 0.7f;
    }
}

void reset_osc_params(struct synthinfo *psynth) {
    // osc params are the things set through the amy_event API
    // Event-derived config
    psynth->bus = AMY_DEFAULT_BUS;
    psynth->wave = SINE;
    AMY_UNSET(psynth->preset);
    AMY_UNSET(psynth->note_source);
    AMY_UNSET(psynth->midi_note);
    psynth->velocity = 0;
    for (int j = 0; j < NUM_COMBO_COEFS; ++j)  psynth->amp_coefs[j] = 0;
    psynth->amp_coefs[COEF_CONST] = 1.0f;  // Default overall gain
    psynth->amp_coefs[COEF_VEL] = 1.0f;    // Default sensitive to vel
    psynth->amp_coefs[COEF_EG0] = 1.0f;    // Default gate by eg0
    for (int j = 0; j < NUM_COMBO_COEFS; ++j)  psynth->logfreq_coefs[j] = 0;
    psynth->logfreq_coefs[COEF_NOTE] = 1.0;
    psynth->logfreq_coefs[COEF_BEND] = 1.0;
    for (int j = 0; j < NUM_COMBO_COEFS; ++j)  psynth->filter_logfreq_coefs[j] = 0;
    for (int j = 0; j < NUM_COMBO_COEFS; ++j)  psynth->duty_coefs[j] = 0;
    psynth->duty_coefs[COEF_CONST] = 0.5f;
    for (int j = 0; j < NUM_COMBO_COEFS; ++j)  psynth->pan_coefs[j] = 0;
    psynth->pan_coefs[COEF_CONST] = 0.5f;
    psynth->feedback = F2S(0); //.996; todo ks feedback is v different from fm feedback
    AMY_UNSET(psynth->trigger_phase);
    AMY_UNSET(psynth->logratio);
    psynth->portamento_alpha = 0;
    psynth->resonance = 0.7f;
    psynth->filter_type = FILTER_NONE;
    AMY_UNSET(psynth->chained_osc);
    AMY_UNSET(psynth->mod_source);
    psynth->algorithm = 0;
    for(uint8_t j=0;j<MAX_ALGO_OPS;j++) AMY_UNSET(psynth->algo_source[j]);
    for(uint8_t j=0;j<MAX_BREAKPOINT_SETS;j++) {
        // max_num_breakpoints describes the alloc for this synthinfo, and is *not* reset.
        for(uint8_t k=0;k<psynth->max_num_breakpoints[j];k++) {
            AMY_UNSET(psynth->breakpoint_times[j][k]);
            AMY_UNSET(psynth->breakpoint_values[j][k]);
        }
        psynth->eg_type[j] = ENVELOPE_NORMAL;  // ENVELOPE_LINEAR;  // no_amp_001: was ENVELOPE_NORMAL
    }
    // Default EG0 setup to be key gate
    psynth->breakpoint_times[0][0] = 0;
    psynth->breakpoint_values[0][0] = 1.0f;
    psynth->breakpoint_times[0][1] = 0;
    psynth->breakpoint_values[0][1] = 0;
    // Not part of event-driven config, but still stable through the evolution of a note.
    psynth->terminate_on_silence = 1;  // This is what we do, *except* for PCM.
    psynth->lut = NULL;
}

void reset_osc_state(struct synthinfo *psynth) {
    // osc state are the internal values that keep track of the osc evolution in time.
    psynth->status = SYNTH_OFF;
    psynth->phase = F2P(0);
    psynth->step = 0;
    psynth->substep = 0;
    AMY_UNSET(psynth->render_clock);
    AMY_UNSET(psynth->note_on_clock);
    psynth->note_off_clock = 0;  // Used to check that last event seen by note was off.
    AMY_UNSET(psynth->mod_value_clock);
    psynth->mod_value = F2S(0);
    for(uint8_t j=0;j<MAX_BREAKPOINT_SETS;j++) { psynth->last_scale[j] = 0; }
    psynth->last_two[0] = 0;
    psynth->last_two[1] = 0;
    for(int j = 0; j < 2 * FILT_NUM_DELAYS; ++j) psynth->filter_delay[j] = 0;
    psynth->last_filt_norm_bits = 0;
}

void reset_osc_by_pointer(struct synthinfo *psynth, struct mod_synthinfo *pmsynth) {
    // set synth state to defaults given a pointer
    // Current state
    reset_osc_params(psynth);
    reset_osc_state(psynth);
    reset_modosc(pmsynth);
}

void reset_osc(uint16_t i ) {
    // set all the synth state to defaults
    if (synth[i] == NULL) return;
    reset_osc_by_pointer(synth[i], msynth[i]);
    synth[i]->osc = i; // self-reference to make updating oscs easier
}

void amy_reset_oscs() {
    // Put the noise generator into a known state.
    srand48(517730);
    // We reset oscs by freeing them.
    // Include per-bus chorus oscs (osc=AMY_OSCS..AMY_OSCS+AMY_NUM_BUSES)
    for(uint16_t i=0;i<AMY_OSCS + AMY_NUM_BUSES;i++) free_osc(i);
    //for(uint16_t i=0;i<AMY_OSCS + AMY_NUM_BUSES;i++) reset_osc(i);
    // also reset filters and volume
    for (int bus = 0; bus < AMY_NUM_BUSES; ++bus)
        amy_global.volume[bus] = 1.0f;
    my_srand48(517730);
    buses_reset();
    // Reset patches
    patches_reset();
    // Reset instruments (synths)
    instruments_reset();
    // Reset memorypcm
    pcm_unload_all_presets();
    // Reset midi_mappings.
    midi_mappings_deinit();
    midi_mappings_init();
    cv_trigger_deinit();
    cv_trigger_init();
    cv_from_osc_deinit();
    cv_from_osc_init();
}


void amy_deltas_reset() {
    delta_release_list(amy_global.delta_queue);
    amy_global.delta_queue = NULL;
    amy_global.delta_qsize = 0;
}

void alloc_osc(int osc, uint8_t *max_num_breakpoints) {
    peek_stack("alloc_osc");
    uint8_t default_num_breakpoints[MAX_BREAKPOINT_SETS] = {DEFAULT_NUM_BREAKPOINTS, DEFAULT_NUM_BREAKPOINTS};
    if (max_num_breakpoints == NULL) {
        max_num_breakpoints = default_num_breakpoints;
    }
    int total_num_breakpoints = 0;
    for (int i=0; i < MAX_BREAKPOINT_SETS; ++i)  total_num_breakpoints += max_num_breakpoints[i];
    uint8_t *ptr = malloc_caps(sizeof(struct synthinfo) + sizeof(struct mod_synthinfo)
                               + total_num_breakpoints * (sizeof(float) + sizeof(uint32_t)),
                               amy_global.config.ram_caps_events);
    synth[osc] = (struct synthinfo *)ptr;
    msynth[osc] = (struct mod_synthinfo *)(ptr + sizeof(struct synthinfo));
    // Point to the breakpoint sets.
    uint8_t *breakpoint_area = ptr + sizeof(struct synthinfo) + sizeof(struct mod_synthinfo);
    for (int i=0; i < MAX_BREAKPOINT_SETS; ++i) {
        synth[osc]->max_num_breakpoints[i] = max_num_breakpoints[i];
        synth[osc]->breakpoint_times[i] = (uint32_t *)breakpoint_area;
        breakpoint_area +=  max_num_breakpoints[i] * sizeof(uint32_t);  // must be a multiple of 4 bytes
        synth[osc]->breakpoint_values[i] = (float *)breakpoint_area;
        breakpoint_area += sizeof(float) * max_num_breakpoints[i];
    }
    reset_osc(osc);
    //fprintf(stderr, "alloc_osc %d (0x%lx) num_breakpoints %d,%d\n", osc, (long)synth[osc], synth[osc]->max_num_breakpoints[0], synth[osc]->max_num_breakpoints[1]);
}

void free_osc(int osc) {
    if (synth[osc] != NULL) {
        //fprintf(stderr, "free_osc %d (0x%lx)\n", osc, (long)synth[osc]);
        free(synth[osc]);
    }
    synth[osc] = NULL;
    msynth[osc] = NULL;
}

void ensure_osc_allocd(int osc, uint8_t *max_num_breakpoints) {
    if (synth[osc] == NULL) alloc_osc(osc, max_num_breakpoints);
    else if (max_num_breakpoints) {
        bool realloc_needed = false;
        uint8_t new_max_num_breakpoints[MAX_BREAKPOINT_SETS];
        for (int i = 0; i < MAX_BREAKPOINT_SETS; ++i) {
            new_max_num_breakpoints[i] = DEFAULT_NUM_BREAKPOINTS;
            if (synth[osc]->max_num_breakpoints[i] < max_num_breakpoints[i]) {
                realloc_needed = true;
                // Increase num_breakpoints in blocks of DEFAULT_NUM_BREAKPOINTS.
                new_max_num_breakpoints[i] = MIN(MAX_BREAKPOINTS,
                                                 DEFAULT_NUM_BREAKPOINTS * ((max_num_breakpoints[i] + DEFAULT_NUM_BREAKPOINTS - 1) / DEFAULT_NUM_BREAKPOINTS));
            }
        }
        if (realloc_needed) {
            //fprintf(stderr, "realloc for osc %d (breakpoints %d, %d -> %d, %d (wave=%d)\n", osc, synth[osc]->max_num_breakpoints[0], synth[osc]->max_num_breakpoints[1], max_num_breakpoints[0], max_num_breakpoints[1], synth[osc]->wave);
            // Save the current values in the structure.
            struct synthinfo saved_values = *synth[osc];
            int32_t breakpoint_times[MAX_BREAKPOINT_SETS][MAX_BREAKPOINTS];
            float breakpoint_values[MAX_BREAKPOINT_SETS][MAX_BREAKPOINTS];
            int num_old_breakpoints[MAX_BREAKPOINT_SETS];
            for (int i = 0; i < MAX_BREAKPOINT_SETS; ++i) {
                num_old_breakpoints[i] = synth[osc]->max_num_breakpoints[i];
                for (int j = 0; j < num_old_breakpoints[i]; ++j) {
                    breakpoint_times[i][j] = synth[osc]->breakpoint_times[i][j];
                    breakpoint_values[i][j] = synth[osc]->breakpoint_values[i][j];
                }
            }
            // Reallocate the structure.
            free_osc(osc);
            alloc_osc(osc, new_max_num_breakpoints);
            // Save the pointers to the newly-alloc'd vectors.
            uint32_t *saved_breakpoint_times[MAX_BREAKPOINT_SETS];
            float *saved_breakpoint_values[MAX_BREAKPOINT_SETS];
            int saved_max_num_breakpoints[MAX_BREAKPOINT_SETS];
            for (int i = 0; i < MAX_BREAKPOINT_SETS; ++i) {
                saved_breakpoint_times[i] = synth[osc]->breakpoint_times[i];
                saved_breakpoint_values[i]= synth[osc]->breakpoint_values[i];
                saved_max_num_breakpoints[i] = synth[osc]->max_num_breakpoints[i];
            }
            // Copy all the values from the previous alloc.
            (*synth[osc]) = saved_values;
            // Restore the new breakpoint vectors.
            for (int i = 0; i < MAX_BREAKPOINT_SETS; ++i) {
                synth[osc]->breakpoint_times[i] = saved_breakpoint_times[i];
                synth[osc]->breakpoint_values[i] = saved_breakpoint_values[i];
                synth[osc]->max_num_breakpoints[i] = saved_max_num_breakpoints[i];
            }
            // And, to be conservative, the breakpoint values themselves.
            for (int i = 0; i < MAX_BREAKPOINT_SETS; ++i) {
                for (int j = 0; j < num_old_breakpoints[i]; ++j) {
                    synth[osc]->breakpoint_times[i][j] = breakpoint_times[i][j];
                    synth[osc]->breakpoint_values[i][j] = breakpoint_values[i][j];
                }
                // And clear the ones beyond
                for (int j = num_old_breakpoints[i]; j < synth[osc]->max_num_breakpoints[i]; ++j) {
                    AMY_UNSET(synth[osc]->breakpoint_times[i][j]);
                    AMY_UNSET(synth[osc]->breakpoint_values[i][j]);
                }
            }
        }
    }
}


// the synth object keeps held state, whereas deltas are only deltas/changes
int8_t oscs_init() {
    if(amy_global.config.ks_oscs>0)
        ks_init();
    algo_init();
    patches_init(amy_global.config.max_memory_patches);
    instruments_init(amy_global.config.max_synths);
    sequencer_init(amy_global.config.max_sequencer_tags);
    if(pcm_samples)  pcm_init();
    if(AMY_HAS_CUSTOM)  custom_init();
    // synth and msynth are now pointers to arrays of pointers to dynamically-allocated synth structures.
    synth = (struct synthinfo **) malloc_caps(sizeof(struct synthinfo *) * (AMY_OSCS + AMY_NUM_BUSES), amy_global.config.ram_caps_synth);
    bzero(synth, sizeof(struct synthinfo *) * (AMY_OSCS + AMY_NUM_BUSES));
    msynth = (struct mod_synthinfo **) malloc_caps(sizeof(struct mod_synthinfo *) * (AMY_OSCS + AMY_NUM_BUSES), amy_global.config.ram_caps_synth);
    output_block_0 = (output_sample_type *) malloc_caps(sizeof(output_sample_type) * AMY_BLOCK_SIZE * AMY_NCHANS, amy_global.config.ram_caps_block);
    output_block_1 = (output_sample_type *) malloc_caps(sizeof(output_sample_type) * AMY_BLOCK_SIZE * AMY_NCHANS, amy_global.config.ram_caps_block);
    output_block = output_block_0;
    amy_in_block = (output_sample_type*)malloc_caps(sizeof(output_sample_type)*AMY_BLOCK_SIZE*AMY_NCHANS, amy_global.config.ram_caps_block);
    amy_external_in_block = (output_sample_type*)malloc_caps(sizeof(output_sample_type)*AMY_BLOCK_SIZE*AMY_NCHANS, amy_global.config.ram_caps_block);
    // set all oscillators to their default values
    amy_reset_oscs();
    // reset the deltas queue
    deltas_pool_init();
    amy_deltas_reset();

    midi_mappings_init();
    cv_trigger_init();
    cv_from_osc_init();

    // clear out both as local mode won't use fbl[1] 
    for(uint16_t core=0;core<AMY_CORES;++core) {
        for (int bus = 0; bus < AMY_NUM_BUSES; ++bus) {
            per_osc_fb[core][bus] = (SAMPLE*)malloc_caps(sizeof(SAMPLE) * AMY_BLOCK_SIZE, amy_global.config.ram_caps_fbl);
            fbl[core][bus] = (SAMPLE*)malloc_caps(sizeof(SAMPLE) * AMY_BLOCK_SIZE * AMY_NCHANS, amy_global.config.ram_caps_fbl);
            bzero(fbl[core][bus], sizeof(SAMPLE) * AMY_BLOCK_SIZE * AMY_NCHANS);
        }
    }

    // Set the optional inputs to 0
    for(uint16_t i=0;i<AMY_BLOCK_SIZE*AMY_NCHANS;i++) {
        amy_in_block[i] = 0;
        amy_external_in_block[i] = 0;
    }

    amy_global.total_blocks = 0;
    amy_global.total_samples = 0;
    amy_global.time = 0;
    //printf("AMY online with %d oscillators, %d block size, %d cores, %d channels, %d pcm samples\n", 
    //    AMY_OSCS, AMY_BLOCK_SIZE, AMY_CORES, AMY_NCHANS, pcm_samples);
    return 0;
}

void fprint_combo_coefs(char *name, float *coefs) {
    fprintf(stderr, "  %s:", name);
    for (int i=0; i < NUM_COMBO_COEFS; ++i) {
        fprintf(stderr, " %.3f", coefs[i]);
    }
    fprintf(stderr, "\n");
}

void print_osc_debug(uint16_t i /* osc */, bool show_eg) {
    if (synth[i] == NULL)  {fprintf(stderr, "osc %" PRIu16 " not defined\n", i); return; }
    fprintf(stderr,"osc %" PRIu16 ": status %" PRIu8 " wave %" PRIu16 " mod_source %" PRIu16 " velocity %f logratio %f feedback %f filtype %" PRIu8 " resonance %f portamento_alpha %f step %f chained %" PRIu16 " algo %" PRIu8 " algo_source %" PRIu16 ",%" PRIu16 ",%" PRIu16 ",%" PRIu16 ",%" PRIu16 ",%" PRIu16 "  \n",
            i, synth[i]->status, synth[i]->wave, synth[i]->mod_source,
            synth[i]->velocity, synth[i]->logratio, synth[i]->feedback, synth[i]->filter_type, synth[i]->resonance, synth[i]->portamento_alpha, P2F(synth[i]->step), synth[i]->chained_osc,
            synth[i]->algorithm,
            synth[i]->algo_source[0], synth[i]->algo_source[1], synth[i]->algo_source[2], synth[i]->algo_source[3], synth[i]->algo_source[4], synth[i]->algo_source[5] );
    fprint_combo_coefs("amp_coefs", synth[i]->amp_coefs);
    fprint_combo_coefs("lfr_coefs", synth[i]->logfreq_coefs);
    fprint_combo_coefs("flf_coefs", synth[i]->filter_logfreq_coefs);
    fprint_combo_coefs("dut_coefs", synth[i]->duty_coefs);
    fprint_combo_coefs("pan_coefs", synth[i]->pan_coefs);
    if(show_eg) {
        for(uint8_t j=0;j<MAX_BREAKPOINT_SETS;j++) {
            fprintf(stderr,"  eg%" PRIu8 " (type %" PRIu8 "): ", j, synth[i]->eg_type[j]);
            for(uint8_t k=0;k<synth[i]->max_num_breakpoints[j];k++) {
                fprintf(stderr,"%" PRIi32 ": %f ", synth[i]->breakpoint_times[j][k], synth[i]->breakpoint_values[j][k]);
            }
            fprintf(stderr,"\n");
        }
        fprintf(stderr,"mod osc %" PRIu16 ": amp: %f, logfreq %f duty %f filter_logfreq %f resonance %f fb/bw %f pan %f \n", i, msynth[i]->amp, msynth[i]->logfreq, msynth[i]->duty, msynth[i]->filter_logfreq, msynth[i]->resonance, msynth[i]->feedback, msynth[i]->pan);
    }
}

// types: 0 - show profile if set
//        1 - show profile, queue
//        2 - show profile, queue, osc data
void show_debug(uint8_t type) {
    amy_global.debug_flag = type;
    amy_profiles_print();
    #ifdef ALLES
    esp_show_debug();
    #endif
    if(type>0) {
        struct delta * ptr = amy_global.delta_queue;
        uint16_t q = amy_global.delta_qsize;
        if(q > 25) q = 25;
        for(uint16_t i=0;i<q;i++) {
            fprintf(stderr,"%" PRIu16 " time %" PRIu32 " osc %" PRIu16 " param %d - %f %" PRIu32 "\n", i, ptr->time, ptr->osc, ptr->param, ptr->data.f, ptr->data.i);
            ptr = ptr->next;
        }
        fprintf(stderr, "deltas_queue len %" PRIi32 ", free len %" PRIi32 "\n", delta_list_len(amy_global.delta_queue), delta_num_free());
        sequencer_debug();
    }
    if(type>1) {
        // print out all the osc data
        //printf("global: filter %f resonance %f volume %f bend %f status %d\n", amy_global.filter_freq, amy_global.resonance, amy_global.volume, amy_global.pitch_bend, amy_global.status);
        fprintf(stderr,"global: volume %.3f %.3f %.3f %.3f bend %f bus 0 eq: %f %f %f \n", amy_global.volume[0], amy_global.volume[1], amy_global.volume[2], amy_global.volume[3], amy_global.pitch_bend, S2F(amy_global.bus[0]->eq.eq[0]), S2F(amy_global.bus[0]->eq.eq[1]), S2F(amy_global.bus[0]->eq.eq[2]));
        for(uint16_t i=0;i<10 /* AMY_OSCS */;i++) {
            print_osc_debug(i, (type > 3) /* show_eg */);
        }
        if (type > 4) {
            patches_debug();
        }
        if (type > 5) {
            midi_mapping_debug();
            cv_trigger_debug();
        }
        if (type > 6) {
            for (int synth = 0; synth < 32 /* MAX_INSTRUMENTS */; ++synth) {
                if (instrument_number_exists(synth, NULL)) {
                    fprintf(stderr, "synth %d:\n", synth);
                    void * state = NULL;
                    amy_event event = amy_default_event();
                    char s[MAX_MESSAGE_LEN];
                    bool include_fx = true;
                    do {
                        state = yield_synth_events(synth, &event, include_fx, state);
                        sprint_event(&event, s, MAX_MESSAGE_LEN, false);
                        fprintf(stderr, "%s\n", s);
                    } while(state != NULL);
                }
            }
        }
        fprintf(stderr, "\n");
    }
}

void oscs_deinit() {
    cv_from_osc_deinit();
    cv_trigger_deinit();
    midi_mappings_deinit();
    for (int bus = 0; bus < AMY_NUM_BUSES; ++bus) {
        dealloc_chorus_delay_lines(bus);
        dealloc_echo_delay_lines(bus);
        dealloc_reverb_delay_lines(bus);
    }
    for(int core = 0; core < AMY_CORES; ++core) {
        for (int bus = 0; bus < AMY_NUM_BUSES; ++bus) {
            free(fbl[core][bus]);
            free(per_osc_fb[core][bus]);
        }
    }
    deltas_pool_free();
    // Include chorus osc (osc=AMY_OSCS)
    for (int i = 0; i < AMY_OSCS + AMY_NUM_BUSES; ++i) free_osc(i);
    free(amy_external_in_block);
    free(amy_in_block);
    free(output_block_1);
    free(output_block_0);
    free(msynth);
    free(synth);
    if(AMY_HAS_CUSTOM)  custom_deinit();
    if(pcm_samples)  pcm_deinit();
    sequencer_deinit();
    instruments_deinit();
    patches_deinit();
    algo_deinit();
    if(amy_global.config.ks_oscs > 0)
        ks_deinit();
}



void osc_note_on(uint16_t osc, float initial_freq) {
    //fprintf(stderr,"Note on: osc %d wav %d note %.3f vel %.3f\n",
    //        osc, synth[osc]->wave,
    //        synth[osc]->midi_note, synth[osc]->velocity);
    // take care of fm & ks first -- no special treatment for bp/mod
    switch (synth[osc]->wave) {
    case KS: if(amy_global.config.ks_oscs) ks_note_on(osc); break;
    case SINE: sine_note_on(osc, initial_freq); break;
    case SAW_DOWN: saw_down_note_on(osc, initial_freq); break;
    case SAW_UP: saw_up_note_on(osc, initial_freq); break;
    case TRIANGLE: triangle_note_on(osc, initial_freq); break;
    case PULSE: pulse_note_on(osc, initial_freq); break;
    case PCM:
    case PCM_LEFT:
    case PCM_RIGHT:
        pcm_note_on(osc);
        break;
    case ALGO: algo_note_on(osc, initial_freq); break;
    case NOISE: noise_note_on(osc); break;
    case AUDIO_IN0: audio_in_note_on(osc, 0); break;
    case AUDIO_IN1: audio_in_note_on(osc, 1); break;
    case AUDIO_EXT0: external_audio_in_note_on(osc, 0); break;
    case AUDIO_EXT1: external_audio_in_note_on(osc, 1); break;
    case AMY_MIDI: amy_send_midi_note_on(osc); break;
    case BYO_PARTIALS: if(AMY_HAS_PARTIALS) partials_note_on(osc); break;
    case INTERP_PARTIALS: if(AMY_HAS_PARTIALS) interp_partials_note_on(osc); break;
    #ifdef AMY_WAVETABLE
    case WAVETABLE: wavetable_note_on(osc, initial_freq); break;
    #endif
    case SILENT: /* nothing */ break;
    case CUSTOM: if(AMY_HAS_CUSTOM) custom_note_on(osc, initial_freq); break;
    default: break;
    }
}

int chained_osc_would_cause_loop(uint16_t osc, uint16_t chained_osc) {
    // Check to see if chaining this osc would cause a loop.
    uint16_t next_osc = chained_osc;
    do {
        ensure_osc_allocd(chained_osc, NULL);
        if (next_osc == osc) {
            fprintf(stderr, "chaining osc %d to osc %d would cause loop.\n",
                    chained_osc, osc);
            return true;
        }
        next_osc = synth[next_osc]->chained_osc;
    } while(AMY_IS_SET(next_osc));
    return false;
}

float portamento_ms_to_alpha(uint16_t portamento_ms) {
    return 1.0f  - 1.0f / (1 + portamento_ms * AMY_SAMPLE_RATE / 1000 / AMY_BLOCK_SIZE);
}

uint16_t alpha_to_portamento_ms(float alpha) {
    return (int)roundf(1000.0f * AMY_BLOCK_SIZE / AMY_SAMPLE_RATE / (1.0f - alpha)) - 1;
}

#define DELTA_TO_SYNTH_I(FLAG, FIELD)  if (d->param == FLAG) synth[d->osc]->FIELD = d->data.i;
#define DELTA_TO_SYNTH_F(FLAG, FIELD)  if (d->param == FLAG) synth[d->osc]->FIELD = d->data.f;
#define DELTA_TO_COEFS(FLAG, FIELD) \
    if (PARAM_IS_COMBO_COEF(d->param, FLAG)) \
        synth[d->osc]->FIELD[d->param - FLAG] = d->data.f;

// play an delta, now -- tell the audio loop to start making noise
void play_delta(struct delta *d) {
    AMY_PROFILE_START(PLAY_DELTA)
    //fprintf(stderr,"play_delta: time %d osc %d param %d val 0x%x, qsize %d\n", amy_global.total_blocks, d->osc, d->param, d->data.i, amy_global.delta_qsize);
    //uint8_t trig=0;
    // todo: delta-only side effect, remove

    if (d->param != RESET_OSC)  ensure_osc_allocd(d->osc, NULL);

    if(d->param == MIDI_NOTE) {
        // Midi note and Velocity are propagated to chained_osc.
        uint16_t osc = d->osc;
        while(AMY_IS_SET(osc) &&
              !(synth[osc]->status == SYNTH_IS_MOD_SOURCE
                || synth[osc]->status == SYNTH_IS_ALGO_SOURCE
                || synth[osc]->wave == PARTIAL)) {
            // (We ignore note values directed at MOD_SOURCE to avoid default voice note-on messing up mod_osc.)
            synth[osc]->midi_note = d->data.f;
            osc = synth[osc]->chained_osc;
        }
    }
    if(d->param == WAVE) {
        synth[d->osc]->wave = d->data.i;
        // todo: delta-only side effect, remove
        // we do this because we need to set up LUTs for FM oscs. it's a TODO to make this cleaner
        if(synth[d->osc]->wave == SINE) {
            sine_note_on(d->osc, freq_of_logfreq(synth[d->osc]->logfreq_coefs[COEF_CONST]));
        }
    }
    DELTA_TO_SYNTH_I(BUS, bus)
    DELTA_TO_SYNTH_F(FEEDBACK, feedback)
    DELTA_TO_SYNTH_F(RATIO, logratio)
    DELTA_TO_SYNTH_F(RESONANCE, resonance)
    DELTA_TO_SYNTH_I(FILTER_TYPE, filter_type)
    DELTA_TO_SYNTH_I(NOTE_SOURCE, note_source)
    DELTA_TO_SYNTH_I(EG0_TYPE, eg_type[0])
    DELTA_TO_SYNTH_I(EG1_TYPE, eg_type[1])
    if (d->param == PRESET) {
        synth[d->osc]->preset = (uint16_t)d->data.i;
    }
    if (d->param == PORTAMENTO) synth[d->osc]->portamento_alpha = portamento_ms_to_alpha(d->data.i);
    if (d->param == PHASE) {
        // Phase sets the *initial* phase of the osc.
        synth[d->osc]->trigger_phase = d->data.f;
        // .. but also warps the current phase to that value.
        synth[d->osc]->phase = F2P(synth[d->osc]->trigger_phase);
    }
    DELTA_TO_COEFS(AMP, amp_coefs)
    DELTA_TO_COEFS(FREQ, logfreq_coefs)
    DELTA_TO_COEFS(FILTER_FREQ, filter_logfreq_coefs)
    DELTA_TO_COEFS(DUTY, duty_coefs)
    DELTA_TO_COEFS(PAN, pan_coefs)

    // todo, i really should clean this up
    if (PARAM_IS_BP_COEF(d->param)) {
        uint8_t pos = d->param - BP_START;
        uint8_t bp_set = 0;
        while(pos >= (MAX_BREAKPOINTS * 2)) { ++bp_set; pos -= (MAX_BREAKPOINTS * 2); }
        int bp_index = pos / 2;
        if (bp_index + 1 > synth[d->osc]->max_num_breakpoints[bp_set]) {
            uint8_t max_num_breakpoints[MAX_BREAKPOINT_SETS];
            for (int i = 0; i < MAX_BREAKPOINT_SETS; ++i)
                max_num_breakpoints[i] = synth[d->osc]->max_num_breakpoints[i];
            max_num_breakpoints[bp_set] = bp_index + 1;
            // realloc rounds up in blocks of DEFAULT_NUM_BREAKPOINTS (8).
            ensure_osc_allocd(d->osc, max_num_breakpoints);
        }
        if(pos % 2 == 0) {
            synth[d->osc]->breakpoint_times[bp_set][pos / 2] = d->data.i;
        } else {
            synth[d->osc]->breakpoint_values[bp_set][(pos-1) / 2] = d->data.f;
        }
    }

    if (PARAM_IS_COMBO_COEF(d->param, AMP) ||
        PARAM_IS_BP_COEF(d->param)) {
        // Changes to Amp/filter/EGs can potentially make a silence-suspended note come back.
        // Revive the note if it hasn't seen a note_off since the last note_on.
        if (synth[d->osc]->status == SYNTH_INAUDIBLE && AMY_IS_UNSET(synth[d->osc]->note_off_clock))
            synth[d->osc]->status = SYNTH_AUDIBLE;
        // In theory, changing the amp coefs away from their defaults could turn on an osc.  But we won't
        // But it won't get started without a note-on.  Unfortunately, we can't call hold_and_modify first.
        // (if the osc has a modulator, we call the modulator in hold_and_modify, but the LUT hasn't been
        // set up yet, so we get an access error.)
        //else if (synth[d->osc]->status == SYNTH_OFF) {
        //    hold_and_modify(d->osc);
        //    if (msynth[d->osc]->amp > 0) {
        //        osc_note_on(d->osc, freq_of_logfreq(msynth[d->osc]->logfreq));
        //    }
        //}
    }
    if(d->param == CHAINED_OSC) {
        int chained_osc = d->data.i;
        if (chained_osc >=0 && chained_osc < AMY_OSCS &&
            !chained_osc_would_cause_loop(d->osc, chained_osc))
            synth[d->osc]->chained_osc = chained_osc;
        else
            AMY_UNSET(synth[d->osc]->chained_osc);
    }
    if(d->param == RESET_OSC) { 
        // Remember that RESET_AMY, RESET_TIMEBASE and RESET_EVENTS happens immediately in the parse, so we don't deal with it here.
        if(d->data.i & RESET_ALL_OSCS) {
            amy_reset_oscs();
        }
        if(d->data.i & RESET_SEQUENCER) {
            sequencer_reset();
        }
        if(d->data.i & RESET_ALL_NOTES) {
            all_notes_off();
        }
        if(d->data.i & RESET_PATCH) {
            // If we got here, it's a full reset of patches.
            patches_reset();
        }
        if(d->data.i < (uint32_t)AMY_OSCS + AMY_NUM_BUSES) {
            reset_osc(d->data.i);
        }
    }
    if(d->param == MOD_SOURCE) {
        uint16_t mod_osc = d->data.i;
        synth[d->osc]->mod_source = mod_osc;
        // NOTE: These are delta-only side effects.  A purist would strive to remove them.
        // When an oscillator is named as a modulator, we change its state.
        ensure_osc_allocd(mod_osc, NULL);
        synth[mod_osc]->status = SYNTH_IS_MOD_SOURCE;
        // Remove default amplitude dependence on velocity when an oscillator is made a modulator.
        synth[mod_osc]->amp_coefs[COEF_VEL] = 0;
        // No longer record this osc in note_off state.
        AMY_UNSET(synth[mod_osc]->note_off_clock);
        // Start the mod osc.
        synth[mod_osc]->note_on_clock = amy_global.total_samples;  // Need a note_on_clock to have envelope work correctly.. not that we care about envelope
        osc_note_on(mod_osc, freq_of_logfreq(synth[mod_osc]->logfreq_coefs[COEF_CONST]));
    }
    if(d->param == ALGORITHM) {
        synth[d->osc]->algorithm = d->data.i;
        // This is a DX7-style control osc; ensure eg_types are set
        // but only when ALGO is specified, so user can override later if desired->
        synth[d->osc]->eg_type[0] = ENVELOPE_DX7;
        synth[d->osc]->eg_type[1] = ENVELOPE_TRUE_EXPONENTIAL;
    }
    if(d->param >= ALGO_SOURCE_START && d->param < ALGO_SOURCE_END) {
        uint16_t which_source = d->param - ALGO_SOURCE_START;
        synth[d->osc]->algo_source[which_source] = d->data.i;
        if(AMY_IS_SET(synth[d->osc]->algo_source[which_source])) {
            int osc = synth[d->osc]->algo_source[which_source];
            ensure_osc_allocd(osc, NULL);
            synth[osc]->status = SYNTH_IS_ALGO_SOURCE;
            // Configure the amp envelope appropriately, just once when named as an algo_source.
            synth[osc]->eg_type[0] = ENVELOPE_DX7;
        }
    }
    // for global changes, just make the change, no need to update the per-osc synth
    uint8_t bus = d->osc;  // We assume d.osc was hijacked in amy_event_to_deltas_queue
    if(d->param >= VOLUME_BASE && d->param < (VOLUME_BASE + AMY_NUM_BUSES)) amy_global.volume[d->param - VOLUME_BASE] = d->data.f;
    if(d->param == PITCH_BEND) amy_global.pitch_bend = d->data.f;
    if(d->param == LATENCY) amy_global.latency_ms = d->data.i;
    if(d->param == TEMPO) { amy_global.tempo = d->data.f; sequencer_recompute(); }
    if(d->param == EQ_L) amy_global.bus[bus]->eq.eq[0] = F2S(powf(10, d->data.f / 20.0));
    if(d->param == EQ_M) amy_global.bus[bus]->eq.eq[1] = F2S(powf(10, d->data.f / 20.0));
    if(d->param == EQ_H) amy_global.bus[bus]->eq.eq[2] = F2S(powf(10, d->data.f / 20.0));
    if(d->param == ECHO_LEVEL) config_echo(bus, d->data.f, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT);
    if(d->param == ECHO_DELAY_MS) config_echo(bus, AMY_UNSET_FLOAT, d->data.f, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT);
    if(d->param == ECHO_MAX_DELAY_MS) config_echo(bus, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, d->data.f, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT);
    if(d->param == ECHO_FEEDBACK) config_echo(bus, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, d->data.f, AMY_UNSET_FLOAT);
    if(d->param == ECHO_FILTER_COEF) config_echo(bus, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, d->data.f);
    if(d->param == CHORUS_LEVEL) config_chorus(bus, d->data.f, UINT16_MAX, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT);
    if(d->param == CHORUS_MAX_DELAY) config_chorus(bus, AMY_UNSET_FLOAT, AMY_IS_UNSET(d->data.f)?UINT16_MAX : (uint16_t)roundf(d->data.f), AMY_UNSET_FLOAT, AMY_UNSET_FLOAT);
    if(d->param == CHORUS_LFO_FREQ) config_chorus(bus, AMY_UNSET_FLOAT, UINT16_MAX, d->data.f, AMY_UNSET_FLOAT);
    if(d->param == CHORUS_DEPTH) config_chorus(bus, AMY_UNSET_FLOAT, UINT16_MAX, AMY_UNSET_FLOAT, d->data.f);
    if(d->param == REVERB_LEVEL) config_reverb(bus, d->data.f, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT);
    if(d->param == REVERB_LIVENESS) config_reverb(bus, AMY_UNSET_FLOAT, d->data.f, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT);
    if(d->param == REVERB_DAMPING) config_reverb(bus, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, d->data.f, AMY_UNSET_FLOAT);
    if(d->param == REVERB_XOVER_HZ) config_reverb(bus, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, AMY_UNSET_FLOAT, d->data.f);

    // triggers / envelopes
    // the only way a sound is made is if velocity (note on) is >0.
    // Ignore velocity deltas if we've already received one this frame.  This may be due to a loop in chained_oscs.
    if(d->param == VELOCITY) {
        //fprintf(stderr, "play_delta velocity %.1f osc %d note %.1f time %d\n", d->data.f, d->osc, synth[d->osc]->midi_note, d->time);
        if (d->data.f > 0) { // new note on (even if something is already playing on this osc)
            // Calculation common to all chained oscs.
            float note_logfreq = 0;
            if (AMY_IS_SET(synth[d->osc]->midi_note)) {
                // synth[osc]->logfreq_coefs[COEF_CONST] = 0;
                note_logfreq = logfreq_for_midi_note(synth[d->osc]->midi_note);
            }
            // Loop through chained oscs
            uint16_t osc = d->osc;
            //fprintf(stderr, "t %.3f: delta note_on: osc %d vel %.3f\n\r", amy_global.time, osc, d->data.f);
            while(AMY_IS_SET(osc)) {
                //fprintf(stderr, "osc: %d wave %d status %d\n\r", osc, synth[osc]->wave, synth[osc]->status);
                if (!(synth[osc]->status == SYNTH_IS_MOD_SOURCE
                      || synth[osc]->status == SYNTH_IS_ALGO_SOURCE
                      || synth[osc]->wave == PARTIAL)
                    && (AMY_IS_SET(synth[osc]->chained_osc)
                        || synth[osc]->amp_coefs[COEF_VEL] == 0
                        || synth[osc]->amp_coefs[COEF_VEL] > AMP_THRESH)) {
                    synth[osc]->velocity = d->data.f;  // was map_60dB_to_01f
                    //fprintf(stderr, "osc: %d status -> AUDIBLE\n\r", osc);
                    // Ignore velocity events for mod source or algo notes.
                    synth[osc]->status = SYNTH_AUDIBLE;
                    // ** no_amp_001
                    // an osc came in with a note on.
                    // start the bp clock
                    synth[osc]->note_on_clock = amy_global.total_samples; //esp_timer_get_time() / 1000;

                    // if there was a filter active for this voice, reset it
                    //if(synth[osc]->filter_type != FILTER_NONE) reset_filter(osc);
                    // We no longer reset the phase here; instead, we reset phase when an oscillator falls silent.
                    // But if a trigger_phase is set, use that.
                    if (AMY_IS_SET(synth[osc]->trigger_phase))
                        synth[osc]->phase = F2P(synth[osc]->trigger_phase);

                    // restart the waveforms
                    // Guess at the initial frequency depending only on const & note.  Envelopes not "developed" yet.
                    float initial_logfreq = synth[osc]->logfreq_coefs[COEF_CONST] + synth[osc]->logfreq_coefs[COEF_NOTE] * note_logfreq;
                    // If we're coming out of note-off, set the freq history for portamento.
                    //if (AMY_IS_SET(synth[osc]->note_off_clock))
                    //    msynth[osc]->last_logfreq = initial_logfreq;
                    // Now we've tested that, we can reset note-off clocks.
                    AMY_UNSET(synth[osc]->note_off_clock);  // Most recent note event is not note-off.

                    float initial_freq = freq_of_logfreq(initial_logfreq);
                    osc_note_on(osc, initial_freq);
                    // trigger the mod source, if we have one
                    uint16_t mod_osc = synth[osc]->mod_source;
                    if(AMY_IS_SET(mod_osc)) {
                        if (AMY_IS_SET(synth[mod_osc]->trigger_phase))
                            synth[mod_osc]->phase = F2P(synth[mod_osc]->trigger_phase);
                        synth[mod_osc]->note_on_clock = amy_global.total_samples;  // Need a note_on_clock to have envelope work correctly.
                        switch(synth[mod_osc]->wave) {
                        case SINE: sine_mod_trigger(mod_osc); break;
                        case SAW_DOWN: saw_up_mod_trigger(mod_osc); break;
                        case SAW_UP: saw_down_mod_trigger(mod_osc); break;
                        case TRIANGLE: triangle_mod_trigger(mod_osc); break;
                        case PULSE: pulse_mod_trigger(mod_osc); break;
                        case PCM:
                        case PCM_LEFT:
                        case PCM_RIGHT:
                            pcm_mod_trigger(mod_osc);
                            break;
                        case CUSTOM: custom_mod_trigger(mod_osc); break;
                        }
                    }
                }
                osc = synth[osc]->chained_osc;
            }
        } else if(synth[d->osc]->velocity > 0 && d->data.f == 0) { // new note off
            uint16_t osc = d->osc;
            while(AMY_IS_SET(osc)) {
                if (!(synth[osc]->status == SYNTH_IS_MOD_SOURCE
                      || synth[osc]->status == SYNTH_IS_ALGO_SOURCE
                      || synth[osc]->wave == PARTIAL)) {
                    //synth[osc]->velocity = 0;
                    switch(synth[osc]->wave) {
                    case KS: ks_note_off(osc); break;
                    case ALGO: algo_note_off(osc); break;
                    case PCM:
                    case PCM_LEFT:
                    case PCM_RIGHT:
                        pcm_note_off(osc);
                        break;
                    case AMY_MIDI: amy_send_midi_note_off(osc); break;
                    case CUSTOM: custom_note_off(osc); break;
                    case BYO_PARTIALS:
                    case INTERP_PARTIALS:
                        AMY_UNSET(synth[osc]->note_on_clock);
                        synth[osc]->note_off_clock = amy_global.total_samples;
                        if(synth[osc]->wave==INTERP_PARTIALS) interp_partials_note_off(osc);
                        else partials_note_off(osc);
                        break;
                    default:
                        // ** no_amp_001
                        // osc note off, start release
                        // For now, note_off_clock signals note off BUT ONLY IF IT'S NOT KS, ALGO, PARTIAL, PCM, or CUSTOM.
                        // I'm not crazy about this, but if we apply it in those cases, the default bp0 amp envelope immediately zeros-out
                        // those waves on note-off.
                        AMY_UNSET(synth[osc]->note_on_clock);
                        if (AMY_IS_UNSET(synth[osc]->note_off_clock)) {
                            // Only set the note_off_clock (start of release) if we don't already have one.
                            synth[osc]->note_off_clock = amy_global.total_samples;
                        }
                    }
                }
                osc = synth[osc]->chained_osc;
            }
        }
    }
    AMY_PROFILE_STOP(PLAY_DELTA)
}

float combine_controls(float *controls, float *coefs) {
    float result = 0;
    for (int i = 0; i < NUM_COMBO_COEFS; ++i)
        result += coefs[i] * controls[i];
    return result;
}

float amp_combine_controls(float *controls, float *coefs) {
    // Linear combination of amp coefs is then mapped so that 0 -> 0.001 and 1 -> 1 exponentially.
    float result = 0;
    for (int i = 0; i < NUM_COMBO_COEFS; ++i)  {
        float coef = coefs[i];
        float val = controls[i];
        if (i == COEF_CONST)  {val = coef; coef = 1.0f;}   // coef[CONST] is always 1.0f, so swap them.  We're going to map the val.
        if (i != COEF_MOD) {
            val = map_60dB_to_01f(MAX(0, val)) - 1.0;    // const, vel, eg0, eg1 get log-compressed.
            // make 0 mean "no amp" and 1 mean "regular (full) amp".
        }
        result += coef * val;
    }
    result = 3.0f * (result);
    //if (log_amp < -2.0f) {
    //    // Double the slope below 0.01.
    //    log_amp = -2.0f + 2.0f * (log_amp + 2.0f);
    //}
    result = powf(10.0f, result);
    if (result <= AMP_THRESH_PLUS)  result = 0; 
    return result;
}

// apply an mod & bp, if any, to the osc
#ifdef __EMSCRIPTEN__
#include "emscripten/webaudio.h"
#endif
void hold_and_modify(uint16_t osc) {
    // Calculate the dynamic parameters for this frame and store them in the msynth[osc] structure.
    AMY_PROFILE_START(HOLD_AND_MODIFY)
    float ctrl_inputs[NUM_COMBO_COEFS];
    ctrl_inputs[COEF_CONST] = 1.0f;
    ctrl_inputs[COEF_NOTE] = (AMY_IS_SET(synth[osc]->midi_note)) ? logfreq_for_midi_note(synth[osc]->midi_note) : 0;
    ctrl_inputs[COEF_VEL] = synth[osc]->velocity;
    ctrl_inputs[COEF_EG0] = S2F(compute_breakpoint_scale(osc, 0, 0));
    ctrl_inputs[COEF_EG1] = S2F(compute_breakpoint_scale(osc, 1, 0));
    ctrl_inputs[COEF_MOD] = S2F(compute_mod_scale(osc));
    ctrl_inputs[COEF_BEND] = amy_global.pitch_bend;
    ctrl_inputs[COEF_EXT0] = cv_inputs[0];
    ctrl_inputs[COEF_EXT1] = cv_inputs[1];

    msynth[osc]->last_pan = msynth[osc]->pan;

    // copy all the modifier variables
    float logfreq = combine_controls(ctrl_inputs, synth[osc]->logfreq_coefs);
    if (synth[osc]->portamento_alpha == 0) {
        msynth[osc]->logfreq = logfreq;
    } else {
        msynth[osc]->logfreq = logfreq + synth[osc]->portamento_alpha * (msynth[osc]->last_logfreq - logfreq);
    }
    msynth[osc]->last_logfreq = msynth[osc]->logfreq;
    float filter_logfreq = combine_controls(ctrl_inputs, synth[osc]->filter_logfreq_coefs);
    if (filter_logfreq < MIN_FILTER_LOGFREQ)  filter_logfreq = MIN_FILTER_LOGFREQ;
    if (AMY_IS_SET(msynth[osc]->last_filter_logfreq)) {
        #define MAX_DELTA_FILTER_LOGFREQ_DOWN 2.0
        float last_logfreq = msynth[osc]->last_filter_logfreq;
        if (filter_logfreq < last_logfreq - MAX_DELTA_FILTER_LOGFREQ_DOWN) {
            // Filter cutoff downward slew-rate limit.
            // See https://github.com/shorepine/amy/issues/126
            filter_logfreq = last_logfreq - MAX_DELTA_FILTER_LOGFREQ_DOWN;
        }
    }
    msynth[osc]->last_filter_logfreq = filter_logfreq;
    msynth[osc]->filter_logfreq = filter_logfreq;
    msynth[osc]->duty = combine_controls(ctrl_inputs, synth[osc]->duty_coefs);
    msynth[osc]->pan = combine_controls(ctrl_inputs, synth[osc]->pan_coefs);
    // amp is a special case - coeffs apply in log domain.
    float new_amp = amp_combine_controls(ctrl_inputs, synth[osc]->amp_coefs);
    // Also, we advance one frame by writing both last_amp and amp (=next amp)
    // *Except* for partials, where we allow one frame of ramp-on.
    if (synth[osc]->wave == PARTIAL) {
        msynth[osc]->last_amp = msynth[osc]->amp;
        msynth[osc]->amp = new_amp;
    } else {
        // Prevent hard-off on transition to release by updating last_amp only for nonzero new_last_amp.
        //if (new_amp > msynth[osc]->last_amp) {   // was > 0
        if (msynth[osc]->last_amp == 0) {
            msynth[osc]->last_amp = new_amp;
        }
        // Advance the envelopes to the beginning of the next frame.
        ctrl_inputs[COEF_EG0] = S2F(compute_breakpoint_scale(osc, 0, AMY_BLOCK_SIZE));
        ctrl_inputs[COEF_EG1] = S2F(compute_breakpoint_scale(osc, 1, AMY_BLOCK_SIZE));
        msynth[osc]->amp = amp_combine_controls(ctrl_inputs, synth[osc]->amp_coefs);
    }
    // synth[osc]->feedback is copied to msynth in pcm_note_on, then used to track note-off for looping PCM.
    // For PCM, don't re-copy it every loop, or we'd lose track of that flag.  (This means you can't change feedback mid-playback for PCM).
    // we also check for custom, for tulips' memorypcm 
    if (synth[osc]->wave != PCM && synth[osc]->wave != CUSTOM)  msynth[osc]->feedback = synth[osc]->feedback;
    msynth[osc]->resonance = synth[osc]->resonance;

    if (osc == 999) {
        fprintf(stderr, "h&m: time %f osc %d note %f vel %f eg0 %f eg1 %f ampc %.3f %.3f %.3f %.3f %.3f %.3f lfqc %.3f %.3f %.3f %.3f %.3f %.3f amp %f logfreq %f\n",
               amy_global.time, osc,
               ctrl_inputs[COEF_NOTE], ctrl_inputs[COEF_VEL], ctrl_inputs[COEF_EG0], ctrl_inputs[COEF_EG1],
               synth[osc]->amp_coefs[0], synth[osc]->amp_coefs[1], synth[osc]->amp_coefs[2], synth[osc]->amp_coefs[3], synth[osc]->amp_coefs[4], synth[osc]->amp_coefs[5],
               synth[osc]->logfreq_coefs[0], synth[osc]->logfreq_coefs[1], synth[osc]->logfreq_coefs[2], synth[osc]->logfreq_coefs[3], synth[osc]->logfreq_coefs[4], synth[osc]->logfreq_coefs[5],
               msynth[osc]->amp, msynth[osc]->logfreq);

    }
    AMY_PROFILE_STOP(HOLD_AND_MODIFY)

}

static inline float lgain_of_pan(float pan) {
    if(pan > 1.f)  pan = 1.f;
    if(pan < 0)  pan = 0;
    return dsps_sqrtf_f32_ansi(1.f - pan);
}

static inline float rgain_of_pan(float pan) {
    if(pan > 1.f)  pan = 1.f;
    if(pan < 0)  pan = 0;
    return dsps_sqrtf_f32_ansi(pan);
}


void mix_with_pan(SAMPLE *stereo_dest, SAMPLE *mono_src, float pan_start, float pan_end) {
    AMY_PROFILE_START(MIX_WITH_PAN)
    /* copy a block_size of mono samples into an interleaved stereo buffer, applying pan */
    if(AMY_NCHANS==1) {
        // actually dest is mono, pan is ignored.
        for(uint16_t i=0;i<AMY_BLOCK_SIZE;i++) { stereo_dest[i] += mono_src[i]; }
    } else { 
        // stereo
        SAMPLE gain_l = F2S(lgain_of_pan(pan_start));
        SAMPLE gain_r = F2S(rgain_of_pan(pan_start));
        SAMPLE d_gain_l = F2S((lgain_of_pan(pan_end) - lgain_of_pan(pan_start)) / AMY_BLOCK_SIZE);
        SAMPLE d_gain_r = F2S((rgain_of_pan(pan_end) - rgain_of_pan(pan_start)) / AMY_BLOCK_SIZE);
        for(uint16_t i=0;i<AMY_BLOCK_SIZE;i++) {
            stereo_dest[i] += MUL8_SS(gain_l, mono_src[i]);
            stereo_dest[AMY_BLOCK_SIZE + i] += MUL8_SS(gain_r, mono_src[i]);
            gain_l += d_gain_l;
            gain_r += d_gain_r;
        }
    }
    AMY_PROFILE_STOP(MIX_WITH_PAN)
}

// Test if the specified osc is in its release phase (i.e., note-off has been received).
#define OSC_IN_RELEASE(osc)  (AMY_IS_SET(synth[osc]->note_off_clock))

SAMPLE render_osc_wave(uint16_t osc, uint8_t core, SAMPLE* buf) {
    AMY_PROFILE_START(RENDER_OSC_WAVE)
    // Returns abs max of what it wrote.
        //if (osc < amy_global.config.max_oscs) // exclude chorus LFOs.
        //fprintf(stderr, "+render_osc_wave: t=%ld core=%d buf=0x%lx (%f, %f, %f, %f...) osc=%d osc_t=%ld\n\r",
        //    amy_global.total_samples, core, buf, S2F(buf[0]), S2F(buf[1]), S2F(buf[2]), S2F(buf[3]),
        //    osc, synth[osc]->render_clock);
    SAMPLE max_val = 0;
    // Only render if osc has not already been rendered this time step e.g. by chained_osc.
    if (synth[osc]->render_clock != amy_global.total_samples) {
        synth[osc]->render_clock = amy_global.total_samples;
        if (synth[osc]->amp_coefs[COEF_CONST] != 0) {
                    // fill buf with next block_size of samples for specified osc.
            hold_and_modify(osc); // apply bp / mod
            if(!(msynth[osc]->amp == 0 && msynth[osc]->last_amp == 0)) {
                if(synth[osc]->wave == NOISE) max_val = render_noise(buf, osc);
                if(synth[osc]->wave == SAW_DOWN) max_val = render_saw_down(buf, osc);
                if(synth[osc]->wave == SAW_UP) max_val = render_saw_up(buf, osc);
                if(synth[osc]->wave == PULSE) max_val = render_pulse(buf, osc);
                if(synth[osc]->wave == TRIANGLE) max_val = render_triangle(buf, osc);
                if(synth[osc]->wave == SINE) max_val = render_sine(buf, osc);
                #ifdef AMY_WAVETABLE
                if(synth[osc]->wave == WAVETABLE) max_val = render_wavetable(buf, osc);
                #endif
                if(synth[osc]->wave == AUDIO_IN0) max_val = render_audio_in(buf, osc, 0);
                if(synth[osc]->wave == AUDIO_IN1) max_val = render_audio_in(buf, osc, 1);
                if(synth[osc]->wave == AUDIO_EXT0) max_val = render_external_audio_in(buf, osc, 0);
                if(synth[osc]->wave == AUDIO_EXT1) max_val = render_external_audio_in(buf, osc, 1);
                if(synth[osc]->wave == AMY_MIDI) max_val = 1;
                if(synth[osc]->wave == KS) {
                    if(amy_global.config.ks_oscs) {
                        max_val = render_ks(buf, osc);
                    }
                }
                if(pcm_samples)
                    if (AMY_WAVE_IS_PCM(synth[osc]->wave)) max_val = render_pcm(buf, osc);
                if(synth[osc]->wave == ALGO) max_val = render_algo(buf, osc, core);
                if(AMY_HAS_PARTIALS) {
                    if(synth[osc]->wave == BYO_PARTIALS || synth[osc]->wave == INTERP_PARTIALS)
                        max_val = render_partials(buf, osc);
                }
            }
        }
        if(AMY_HAS_CUSTOM) {
            if(synth[osc]->wave == CUSTOM) max_val = render_custom(buf, osc);
        }
        if (synth[osc]->wave != SILENT) {
            // apply filter to osc if set
            if (synth[osc]->filter_type != FILTER_NONE) {
                max_val = filter_process(buf, osc, max_val);
                // Maybe clear filter state here if we've finshed this osc.
                if (synth[osc]->status != SYNTH_AUDIBLE) {
                    reset_filter(osc);  // (f)
                }
            }
        }
        if(AMY_IS_SET(synth[osc]->chained_osc)) {
            // Stack oscillators - render next osc into same buffer.
            uint16_t chained_osc = synth[osc]->chained_osc;
            if (synth[chained_osc] != NULL && synth[chained_osc]->status == SYNTH_AUDIBLE) {  // We have to recheck this since we're bypassing the skip in amy_render.
                SAMPLE new_max_val = render_osc_wave(chained_osc, core, buf);
                if (new_max_val > max_val)  max_val = new_max_val;
            }
        }
        // Unlike other oscs, SILENT osc is processed *after* collecting chained_oscs
        if (synth[osc]->wave == SILENT) {
            max_val = render_envelope(buf, osc);
            // apply filter to osc if set
            if (synth[osc]->filter_type != FILTER_NONE) {
                max_val = filter_process(buf, osc, max_val);
                // Maybe clear filter state here if we've finshed this osc.
                if (synth[osc]->status != SYNTH_AUDIBLE) {
                    reset_filter(osc);  // (f)
                }
            }
        }
        // note: Code transplanted here from hold_and_modify() to distinguish actual zero output
        // from zero-amplitude (but maybe inheriting values from chained_oscs).
        // Stop oscillators if amp is zero for several frames in a row.
        // Note: We can't wait for the note off because we need to turn off PARTIAL oscs when envelopes end, even if no note off.
        if (OSC_IN_RELEASE(osc) && max_val < F2S(AMP_THRESH) && synth[osc]->terminate_on_silence) {
            //printf("h&m: time %.3f osc %d OFF\n", amy_global.time, osc);
            // Oscillator has fallen silent, stop executing it.
            uint16_t osc_to_stop = osc;  // Type must match synthinfo.chained_osc
            while (AMY_IS_SET(osc_to_stop)) {
                synth[osc_to_stop]->status = SYNTH_INAUDIBLE;  // It *could* come back...
                // 2026-03-22: It's necessary to reset these two fields in msynth to get OwBass to restart without click...
                msynth[osc_to_stop]->filter_logfreq = 0;  // (a)
                msynth[osc_to_stop]->resonance = 0.7f;  // (b)
                //reset_filter(osc_to_stop);                // (c)
                //AMY_UNSET(msynth[osc_to_stop]->last_filter_logfreq);  // (d)
                // .. but force it to start at zero phase next time.
                synth[osc_to_stop]->phase = 0;
                osc_to_stop = synth[osc_to_stop]->chained_osc;
            }
        }
        //fprintf(stderr, "render_osc_wave: t=%.3f osc=%d max_val %.6f\n", amy_global.time, osc, S2F(max_val));
    }
    AMY_PROFILE_STOP(RENDER_OSC_WAVE)
    //fprintf(stderr, "-render_osc_wave: t=%ld core=%d buf=0x%lx (%f, %f, %f, %f...) osc=%d osc_t=%ld\n",
    //    amy_global.total_samples, core, buf, S2F(buf[0]), S2F(buf[1]), S2F(buf[2]), S2F(buf[3]),
    //    osc, synth[osc]->render_clock);
    return max_val;
}

void amy_render(uint16_t start, uint16_t end, uint8_t core) {
    AMY_PROFILE_START(AMY_RENDER)

    for(int bus = 0; bus <= amy_global.highest_bus; ++bus)
        bzero(fbl[core][bus], sizeof(SAMPLE) * AMY_BLOCK_SIZE * AMY_NCHANS); 
    SAMPLE max_max = 0;
    for(uint16_t osc=start; osc<end; osc++) {
        if(synth[osc] != NULL && synth[osc]->status == SYNTH_AUDIBLE) { // skip oscs that are silent or mod sources from playback
            uint8_t bus = synth[osc]->bus;
            bzero(per_osc_fb[core][bus], AMY_BLOCK_SIZE * sizeof(SAMPLE));
            SAMPLE max_val = render_osc_wave(osc, core, per_osc_fb[core][bus]);
            if (synth[osc]->status != SYNTH_AUDIBLE) {
                reset_modosc(msynth[osc]);  // (g)  This makes a difference, but not clicks
                reset_osc_state(synth[osc]);
            }
            uint8_t handled = 0;
            if(amy_global.config.amy_external_render_hook != NULL) {
                handled = amy_global.config.amy_external_render_hook(osc, per_osc_fb[core][bus], AMY_BLOCK_SIZE);
            } else {
                #ifdef __EMSCRIPTEN__
                // TODO -- pass the buffer to a JS shim using the new bytes support, we could use this to visualize CV output
                #endif
            }
            // only mix the audio in if the external hook did not handle it
            if(!handled) mix_with_pan(fbl[core][bus], per_osc_fb[core][bus], msynth[osc]->last_pan, msynth[osc]->pan);
            if (max_val > max_max) max_max = max_val;
        } // end if audible
    }
    core_max[core] = max_max;

    if(AMY_HAS_CHORUS && core == 0) {
        for(int bus = 0; bus <= amy_global.highest_bus; ++bus) {
            ensure_osc_allocd(CHORUS_MOD_SOURCE + bus, NULL);
            hold_and_modify(CHORUS_MOD_SOURCE + bus);
            if(amy_global.bus[bus]->chorus.level!=0)  {
                bzero(amy_global.bus[bus]->chorus.delay_mod, AMY_BLOCK_SIZE * sizeof(SAMPLE));
                render_osc_wave(CHORUS_MOD_SOURCE + bus, 0 /* core */, amy_global.bus[bus]->chorus.delay_mod);
            }
        }
    }

    if (amy_global.debug_flag) {
        amy_global.debug_flag = 0;  // Only do this once each time debug_flag is set.
        SAMPLE smax = scan_max(fbl[core][0 /* bus */], AMY_BLOCK_SIZE);
        fprintf(stderr, "time %" PRIu32 " core %d bus 0 max_max=%.3f post-eq max=%.3f\n", amy_global.total_samples, core, S2F(max_max), S2F(smax));
    }

    AMY_PROFILE_STOP(AMY_RENDER)

}


// this plays just the next delta if it's time.
void amy_execute_delta() {
    AMY_PROFILE_START(AMY_EXECUTE_DELTAS)
    // check to see which sounds to play
    uint32_t sysclock = amy_sysclock();
    amy_grab_lock();

    // find any deltas that need to be played from the (in-order) queue
    struct delta *d = amy_global.delta_queue;
    if(d && sysclock >= d->time) {
        play_delta(d);
        d = delta_release(d);
        amy_global.delta_qsize--;
    }
    amy_global.delta_queue = d;

    amy_release_lock();

    AMY_PROFILE_STOP(AMY_EXECUTE_DELTAS)

}

// this takes scheduled deltas and plays them at the right time
void amy_execute_deltas() {
    AMY_PROFILE_START(AMY_EXECUTE_DELTAS)
    // Make sure any CV-triggered events are added to delta queue
    update_external_cv_in();

    // check to see which sounds to play
    uint32_t sysclock = amy_sysclock();
    amy_grab_lock();

    // find any deltas that need to be played from the (in-order) queue
    struct delta *d = amy_global.delta_queue;
    while(d && sysclock >= d->time) {
        play_delta(d);
        d = delta_release(d);
        amy_global.delta_qsize--;
    }
    amy_global.delta_queue = d;

    amy_release_lock();

    AMY_PROFILE_STOP(AMY_EXECUTE_DELTAS)

}



// called by the audio render loop to alert JS (and then python) that a block has been rendered
void amy_block_processed(void) {

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if(typeof amy_block_processed_js_hook === 'function') {
            amy_block_processed_js_hook();
        }
    });
#else
    if(amy_global.config.amy_external_block_done_hook != NULL) {
        amy_global.config.amy_external_block_done_hook();
    }
#endif
}

void amy_process_event(amy_event *e) {
    peek_stack("process_event");
    if(AMY_IS_SET(e->sequence[SEQUENCE_TICK]) || AMY_IS_SET(e->sequence[SEQUENCE_PERIOD]) || AMY_IS_SET(e->sequence[SEQUENCE_TAG])) {
        uint8_t added = sequencer_add_event(e);
        (void)added; // we don't need to do anything with this info at this time
        e->status = EVENT_SEQUENCE;
    } else if (AMY_IS_SET(e->reset_osc) && (e->reset_osc & RESET_PATCH) && AMY_IS_SET(e->patch_number)) {
        // We're resetting just one patch, do it now.  But RESET_PATCH with no patch_number should propagate to deltas.
        patches_reset_patch(e->patch_number);
        AMY_UNSET(e->reset_osc);
    } else {
        // if time is set, play then
        // if time and latency is set, play in time + latency
        // if time is not set, play now
        // if time is not set + latency is set, play in latency
        uint32_t playback_time = amy_sysclock();
        if(AMY_IS_SET(e->time)) playback_time = e->time;
        playback_time += amy_global.latency_ms;
        e->time = playback_time;
        e->status = EVENT_SCHEDULED;
    }
}

// ==================== GrvEP bus-0 FX chain (activation-order dispatch) ====================
// The GrvEP-custom bus-0 effects below (FILT-non-ladder/LADDER, DRIVE, RESECHO, the
// REP/BITCRS wavefold, RINGMOD, COMPRESS) each get their own function here so they can
// be applied in ANY order — specifically the order the user actually activated them in
// (fxOrderList/fxOrderCount, tracked on the GrvEP/main.cpp side and mirrored here via a
// plain extern), not a fixed pipeline position. This is a deliberate replacement for an
// earlier one-off "run a second LPF specifically after RESECHO" patch, which worked for
// that one pairing but wasn't what was actually asked for: "je veux juste que les FX se
// placent les uns à la suite des autres selon l'ordre d'activation. Si j'active ResEcho
// puis LPF, j'aurai donc synth > ResEcho > LPF > sortie audio."
//
// AMY's OWN built-in per-bus effects (REVERB/CHORUS/FLANGER/DELAY/EQ/RES — the ones
// implemented via config_reverb/config_chorus/config_echo/config_eq) are NOT part of
// this reordering: they live inside AMY's own fixed per-bus loop (further below, after
// this dispatch), which processes EQ->chorus->echo->reverb in that fixed relative order
// regardless of when the user turned them on. Reordering those against each other or
// against this GrvEP chain would need restructuring AMY's own mixer, out of scope here.
// They always run AFTER this whole GrvEP chain, same position as before.
//
// FILT converting to a pure bus-0 effect (LPF/HPF/BPF via a new Chamberlin state-variable
// filter here, alongside the existing separate LADDER design) — instead of also keeping
// its old per-oscillator application — is what makes it a genuine, positionable chain
// participant at all: a per-oscillator filter is applied at the very source, upstream of
// every bus effect, so it could never be "placed after ResEcho" no matter what. This also
// means FILT now reaches literally everything mixed into bus 0 uniformly (drums, samples,
// patches, T303, etc.), same as LADDER already did, rather than only the specific
// oscillator ranges audioSetAllFiltersT() was wired to reach.
extern uint8_t fxOrderList[];
extern uint8_t fxOrderCount;

// REP/BITCRS wavefold — shared single gain (amy_wavefold_gain_bus0), see audioSetWavefold().
static void amy_apply_wavefold_bus0(SAMPLE* buf) {
    if (amy_wavefold_gain_bus0 <= 1.001f || buf == NULL) return;
    int n = AMY_BLOCK_SIZE * AMY_NCHANS;
    float peak = 0.0f;
    for (int i = 0; i < n; i++) { float a = fabsf(buf[i]); if (a > peak) peak = a; }
    if (peak <= 0.001f) return;
    float g = amy_wavefold_gain_bus0;
    float inv_peak = 1.0f / peak;
    for (int i = 0; i < n; i++) {
        float s = buf[i] * inv_peak * g;
        float phase = fmodf(s + 1.0f, 4.0f);
        if (phase < 0.0f) phase += 4.0f;
        buf[i] = ((phase < 2.0f) ? (phase - 1.0f) : (3.0f - phase)) * peak;
    }
}

// RINGMOD: multiplies the signal by a sine carrier. One carrier phase per stereo frame
// (not per channel) so the carrier stays phase-locked across L/R. fbl is PLANAR (each
// channel its own contiguous AMY_BLOCK_SIZE run — see the final mix's own indexing).
static void amy_apply_ringmod(SAMPLE* buf) {
    if (amy_ringmod_on <= 0.5f || amy_ringmod_mix <= 0.001f || buf == NULL) return;
    float phaseInc = amy_ringmod_freq * 2.0f * (float)M_PI / (float)AMY_SAMPLE_RATE;
    float mix = amy_ringmod_mix;
    for (int i = 0; i < AMY_BLOCK_SIZE; i++) {
        float carrier = sinf(amy_ringmod_phase);
        for (int ch = 0; ch < AMY_NCHANS; ch++) {
            int idx = i + ch * AMY_BLOCK_SIZE;
            float dry = buf[idx];
            buf[idx] = dry * (1.0f - mix) + (dry * carrier) * mix;
        }
        amy_ringmod_phase += phaseInc;
        if (amy_ringmod_phase > 2.0f * (float)M_PI) amy_ringmod_phase -= 2.0f * (float)M_PI;
    }
}

// FILT: LADDER (existing two-cascade feedforward-resonance design, unchanged/kept
// exactly as tuned) or LPF/HPF/BPF (new Chamberlin state-variable filter — the SAME
// stable topology already proven in RESECHO's own resonant filter, picking whichever
// of its three simultaneous taps (low/high/band) matches Typ).
static void amy_apply_filt(SAMPLE* buf) {
    if (buf == NULL) return;
    if (amy_ladder_on > 0.5f) {
        // g = 1 - exp(-2*pi*fc/fs): correct one-pole coefficient for cutoff fc.
        float g = 1.0f - expf(-2.0f * (float)M_PI * amy_ladder_cutoff / (float)AMY_SAMPLE_RATE);
        if (g < 0.001f) g = 0.001f;
        if (g > 0.999f) g = 0.999f;
        float nyquist  = AMY_SAMPLE_RATE * 0.5f;
        float headroom = (nyquist - amy_ladder_cutoff) / nyquist;
        if (headroom < 0.0f) headroom = 0.0f;
        if (headroom > 1.0f) headroom = 1.0f;
        float fc2 = amy_ladder_cutoff * (1.0f + 0.6f * headroom);
        float g2 = 1.0f - expf(-2.0f * (float)M_PI * fc2 / (float)AMY_SAMPLE_RATE);
        if (g2 < 0.001f) g2 = 0.001f;
        if (g2 > 0.999f) g2 = 0.999f;
        for (int ch = 0; ch < AMY_NCHANS; ch++) {
            float* s = amy_ladder_stage[ch];
            SAMPLE* cbuf = buf + ch * AMY_BLOCK_SIZE;
            for (int i = 0; i < AMY_BLOCK_SIZE; i++) {
                float rawin = S2F(cbuf[i]);
                s[0] += g * (rawin - s[0]);
                s[1] += g * (s[0] - s[1]);
                s[2] += g2 * (rawin - s[2]);
                s[3] += g2 * (s[2] - s[3]);
                float peak = (s[3] - s[1]) * amy_ladder_resonance;
                float outv = s[1] + peak;
                outv = tanhf(outv * 0.6f) * 1.6667f;
                cbuf[i] = F2S(outv);
            }
        }
    } else if (amy_filt_on > 0.5f) {
        // Zero-delay-feedback (TPT/Zavalishin) state-variable filter: unconditionally
        // stable for any cutoff/Q, unlike the Chamberlin form this replaced (only stable
        // for f < ~2-q — at high cutoff + high resonance it diverged to full-scale/NaN
        // and stayed stuck there even after the knobs came back to sane values).
        uint8_t type = (uint8_t)amy_filt_type;
        float fc = amy_filt_cutoff;
        if (fc < 20.0f) fc = 20.0f;
        if (fc > 0.45f * (float)AMY_SAMPLE_RATE) fc = 0.45f * (float)AMY_SAMPLE_RATE;
        float g = tanf((float)M_PI * fc / (float)AMY_SAMPLE_RATE);
        float k = 1.0f / (amy_filt_reso < 0.5f ? 0.5f : amy_filt_reso);
        if (k < 0.08f) k = 0.08f;
        float a1 = 1.0f / (1.0f + g * (g + k));
        float a2 = g * a1;
        float a3 = g * a2;
        for (int ch = 0; ch < AMY_NCHANS; ch++) {
            SAMPLE* cbuf = buf + ch * AMY_BLOCK_SIZE;
            float ic1 = amy_filt_svf_low[ch], ic2 = amy_filt_svf_band[ch];
            for (int i = 0; i < AMY_BLOCK_SIZE; i++) {
                float x = S2F(cbuf[i]);
                float v3 = x - ic2;
                float v1 = a1 * ic1 + a2 * v3;
                float v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                float y = (type == 0) ? v2 : (type == 1) ? (x - k * v1 - v2) : (k * v1);
                if (!(y > -64.0f && y < 64.0f)) { y = 0.0f; ic1 = 0.0f; ic2 = 0.0f; }  // NaN/inf guard
                cbuf[i] = F2S(y);
            }
            amy_filt_svf_low[ch] = ic1;   // (reused as TPT integrator state ic1eq)
            amy_filt_svf_band[ch] = ic2;  // (reused as TPT integrator state ic2eq)
        }
    }
}

static void amy_apply_drive(SAMPLE* buf) {
    if (amy_drive_on <= 0.5f || buf == NULL) return;
    uint8_t type = (uint8_t)amy_drive_type;
    float amt = amy_drive_amt;
    float pregain;
    switch (type) {
        case 0:  pregain = 1.0f + amt * 5.0f;  break;
        case 1:  pregain = 1.0f + amt * 11.0f; break;
        case 2:  pregain = 1.0f + amt * 25.0f; break;
        default: pregain = 1.0f + amt * 9.0f;  break;
    }
    float odNorm = 1.0f / tanhf(pregain);
    float toneCutoff = 18000.0f * powf(400.0f / 18000.0f, amy_drive_tone);
    float g = 1.0f - expf(-2.0f * (float)M_PI * toneCutoff / (float)AMY_SAMPLE_RATE);
    if (g < 0.001f) g = 0.001f;
    if (g > 0.999f) g = 0.999f;
    float level = amy_drive_level;
    int n = AMY_BLOCK_SIZE * AMY_NCHANS;
    for (int i = 0; i < n; i++) {
        int ch = i / AMY_BLOCK_SIZE;
        float s = S2F(buf[i]) * pregain;
        float y;
        if (type == 0) {
            y = tanhf(s) * odNorm;
        } else if (type == 1) {
            y = (s >= 0.0f) ? (1.0f - expf(-s)) : (-1.0f + expf(s));
        } else if (type == 2) {
            y = (s > 1.0f) ? 1.0f : ((s < -1.0f) ? -1.0f : s);
        } else {
            float phase = fmodf(s + 1.0f, 4.0f);
            if (phase < 0.0f) phase += 4.0f;
            y = (phase < 2.0f) ? (phase - 1.0f) : (3.0f - phase);
        }
        amy_drive_tone_lp[ch] += g * (y - amy_drive_tone_lp[ch]);
        buf[i] = F2S(amy_drive_tone_lp[ch] * level);
    }
}

// RESECHO: mono delay line with a resonant state-variable filter inside the feedback
// loop — see amy_resoecho_* declarations for why this is a separate engine from DELAY.
static void amy_apply_resoecho(SAMPLE* buf) {
    if (amy_resoecho_on <= 0.5f || amy_resoecho_buf == NULL || buf == NULL) return;
    uint32_t mask = amy_resoecho_buf_len - 1;
    uint32_t ds = amy_resoecho_delay_samples;
    float f = 2.0f * sinf((float)M_PI * amy_resoecho_centre / (float)AMY_SAMPLE_RATE);
    if (f > 1.9f) f = 1.9f;
    float q = 1.0f / amy_resoecho_reso;
    for (int i = 0; i < AMY_BLOCK_SIZE; i++) {
        float inSample = 0.5f * (S2F(buf[i]) + S2F(buf[AMY_BLOCK_SIZE + i]));
        uint32_t readIdx = (amy_resoecho_write - ds) & mask;
        float delayed = S2F(amy_resoecho_buf[readIdx]);
        float low  = amy_resoecho_svf_low + f * amy_resoecho_svf_band;
        float high = delayed - low - q * amy_resoecho_svf_band;
        float band = amy_resoecho_svf_band + f * high;
        amy_resoecho_svf_low  = low;
        amy_resoecho_svf_band = band;
        // Broadband feedback tap (was `band*q`, the SVF's bandpass output — resonance
        // concentrated at exactly `amy_resoecho_centre`, reported as sounding like it only
        // "rings" at one arbitrary pitch). `low` is a lowpass instead: everything below the
        // cutoff feeds back and can build up (comb-filter-style resonance across the WHOLE
        // passband, not a single peak), only progressively darkened above the cutoff. The
        // SVF's internal `q` still shapes `band`, which `low` integrates every sample, so
        // Wild/resonance still audibly changes how much this self-sustains/saturates — it's
        // just no longer the sole thing being fed back.
        // 0.4x: `low`'s natural passband gain (further reinforced by resonance, since it
        // integrates `band` every sample) runs noticeably hotter than the old `band*q`
        // tap's deliberately-unity-normalized peak — empirically compensated so the same
        // Lvl/FB/Wild dial positions land in roughly the same loudness ballpark as before.
        float filtered = low * 0.22f;
        float fbSample = inSample + filtered * amy_resoecho_feedback;
        fbSample = 2.0f * tanhf(fbSample * 0.5f);  // soft ceiling (~2.0), bounded even at unity loop gain
        if (!(fbSample > -8.0f && fbSample < 8.0f)) { fbSample = 0.0f; amy_resoecho_svf_low = 0.0f; amy_resoecho_svf_band = 0.0f; }
        amy_resoecho_buf[amy_resoecho_write] = F2S(fbSample);
        amy_resoecho_write = (amy_resoecho_write + 1) & mask;
        float wet = filtered * amy_resoecho_level;
        buf[i]                  = F2S(S2F(buf[i])                  + wet);
        buf[AMY_BLOCK_SIZE + i] = F2S(S2F(buf[AMY_BLOCK_SIZE + i]) + wet);
    }
}

// COMPRESSOR: feedforward peak-envelope-follower gain reduction. Linked stereo: one
// envelope/gain value per frame driven by whichever channel is louder, applied to both
// channels' (planar) copy of that same time index.
static void amy_apply_compressor(SAMPLE* buf) {
    if (amy_comp_on <= 0.5f || buf == NULL) return;
    float thresh = amy_comp_threshold;
    float ratio  = amy_comp_ratio;
    float attackCoef  = expf(-1.0f / (0.005f * (float)AMY_SAMPLE_RATE));
    float releaseCoef = expf(-1.0f / (0.080f * (float)AMY_SAMPLE_RATE));
    for (int i = 0; i < AMY_BLOCK_SIZE; i++) {
        float rect = 0.0f;
        for (int ch = 0; ch < AMY_NCHANS; ch++) {
            // S2F(): see COMPRESS's own history — comparing the raw SAMPLE magnitude
            // directly against amy_comp_threshold (normalized 0..1) was off by
            // 1<<S_FRAC_BITS on fixed-point builds, collapsing gr to near-zero always.
            float a = fabsf(S2F(buf[i + ch * AMY_BLOCK_SIZE]));
            if (a > rect) rect = a;
        }
        float coef = (rect > amy_comp_env) ? attackCoef : releaseCoef;
        amy_comp_env = rect + coef * (amy_comp_env - rect);
        float gr = 1.0f;
        if (amy_comp_env > thresh && thresh > 0.001f) {
            float over_db = 20.0f * log10f(amy_comp_env / thresh);
            float comp_db = over_db * (1.0f - 1.0f / ratio);
            gr = powf(10.0f, -comp_db / 20.0f);
        }
        for (int ch = 0; ch < AMY_NCHANS; ch++) buf[i + ch * AMY_BLOCK_SIZE] *= gr;
    }
}

// Applies every active GrvEP-custom bus-0 FX in fxOrderList order (activation order).
static void amy_apply_ordered_fx(SAMPLE* buf) {
    if (buf == NULL) return;
    for (uint8_t oi = 0; oi < fxOrderCount; oi++) {
        switch (fxOrderList[oi]) {
            case 0:  amy_apply_filt(buf);       break;  // FILT (LPF/HPF/BPF/LADDER)
            case 1:  amy_apply_drive(buf);      break;  // DRIVE
            case 7:  amy_apply_resoecho(buf);   break;  // RESECHO
            case 8:  amy_apply_wavefold_bus0(buf); break;  // REP
            case 9:  amy_apply_wavefold_bus0(buf); break;  // BITCRS (shares REP's gain)
            case 11: amy_apply_ringmod(buf);    break;  // RINGMOD
            case 12: amy_apply_compressor(buf); break;  // COMPRESS
            default: break;  // AMY-native FX (REVERB/CHORUS/FLANGER/DELAY/EQ/RES) — fixed position, see below
        }
    }
}

// ==================== FX Punch "looper" engine (phase 2) ====================
// LOOP 1/4-1/32, STUTTER, REVERSE, TAPESTOP and HALFSPD (the 8 Sloop-style punch effects
// that have no existing fxList[] FX to piggyback on, unlike the 8 "phase 1" ones in
// GrvEP's own main.cpp) all work the same way: continuously capture the final post-FX
// mix (exactly what's about to hit the DAC) into a ring buffer, then while a punch is
// held, overwrite output_block[] with a read from that history instead of the live
// render. The live render (oscillators, bus FX) is NEVER paused or bypassed — only what
// gets copied into output_block[] changes — so releasing a punch resumes live audio
// with zero glitch, and capture always has fresh material for the next punch.
//
// Two buffers: amy_punch_cap_buf is the continuously-written rolling history (used
// directly, with variable/negative rate, by REVERSE/HALFSPD/TAPESTOP); amy_punch_loop_buf
// is a separate frozen snapshot (used by LOOP/STUTTER) so a sustained loop isn't corrupted
// by the live capture ring wrapping around and overwriting the exact samples being
// repeated underneath it.
#define AMY_PUNCH_CAP_SECONDS 2
#define AMY_PUNCH_CAP_FRAMES (AMY_SAMPLE_RATE * AMY_PUNCH_CAP_SECONDS)
#ifdef ESP_PLATFORM
#define AMY_PUNCH_RAM_CAPS MALLOC_CAP_SPIRAM  // ~700KB total for both buffers — PSRAM only,
#else                                          // never internal DIRAM (see amy_global.config's
#define AMY_PUNCH_RAM_CAPS MALLOC_CAP_DEFAULT  // own ram_caps_delay, which GrvEP leaves at
#endif                                         // MALLOC_CAP_DEFAULT/internal RAM on this board)

static int16_t* amy_punch_cap_buf  = NULL;
static int16_t* amy_punch_loop_buf = NULL;
static uint32_t amy_punch_cap_write_frame = 0;
static uint32_t amy_punch_loop_len_frames = 0;

typedef enum { AMY_PUNCHLOOP_NONE = 0, AMY_PUNCHLOOP_FIXED, AMY_PUNCHLOOP_RATE } amy_punchloop_mode_t;
static amy_punchloop_mode_t amy_punchloop_mode = AMY_PUNCHLOOP_NONE;
static float    amy_punchloop_read_frame  = 0.0f;  // fractional read position (FIXED: within [0,len); RATE: absolute ring position)
static float    amy_punchloop_rate        = 1.0f;  // frames advanced per output frame; negative = reverse
static float    amy_punchloop_rate_decel  = 0.0f;  // >0: TAPESTOP's per-frame multiplicative rate decay
static uint32_t amy_punchloop_fixed_len   = 0;

static void amy_punch_cap_ensure_alloc() {
    if (amy_punch_cap_buf != NULL) return;
    amy_punch_cap_buf  = (int16_t*)malloc_caps(sizeof(int16_t) * AMY_PUNCH_CAP_FRAMES * AMY_NCHANS, AMY_PUNCH_RAM_CAPS);
    amy_punch_loop_buf = (int16_t*)malloc_caps(sizeof(int16_t) * AMY_PUNCH_CAP_FRAMES * AMY_NCHANS, AMY_PUNCH_RAM_CAPS);
    if (amy_punch_cap_buf)  memset(amy_punch_cap_buf,  0, sizeof(int16_t) * AMY_PUNCH_CAP_FRAMES * AMY_NCHANS);
    if (amy_punch_loop_buf) memset(amy_punch_loop_buf, 0, sizeof(int16_t) * AMY_PUNCH_CAP_FRAMES * AMY_NCHANS);
}

// Called every block, unconditionally (even while a punch is overriding the output) so
// history stays fresh and releasing a punch always resumes from live, current audio.
static void amy_punch_capture_block() {
    if (amy_punch_cap_buf == NULL) return;
    for (int32_t i = 0; i < AMY_BLOCK_SIZE; i++) {
        uint32_t wf = (amy_punch_cap_write_frame + (uint32_t)i) % AMY_PUNCH_CAP_FRAMES;
        for (int32_t c = 0; c < AMY_NCHANS; c++)
            amy_punch_cap_buf[wf * AMY_NCHANS + c] = output_block[i * AMY_NCHANS + c];
    }
    amy_punch_cap_write_frame = (amy_punch_cap_write_frame + AMY_BLOCK_SIZE) % AMY_PUNCH_CAP_FRAMES;
}

static inline int16_t amy_punch_read_interp(int16_t* buf, float pos, int32_t ch) {
    while (pos < 0) pos += AMY_PUNCH_CAP_FRAMES;
    uint32_t i0 = ((uint32_t)pos) % AMY_PUNCH_CAP_FRAMES;
    uint32_t i1 = (i0 + 1) % AMY_PUNCH_CAP_FRAMES;
    float frac = pos - floorf(pos);
    float s0 = buf[i0 * AMY_NCHANS + ch], s1 = buf[i1 * AMY_NCHANS + ch];
    return (int16_t)(s0 + (s1 - s0) * frac);
}

// Overwrites output_block[] with the active punch-loop's read-back, if any. No-op (leaves
// the live-rendered output_block untouched) when no loop/rate punch is active.
static void amy_punch_override_block() {
    if (amy_punchloop_mode == AMY_PUNCHLOOP_NONE) return;
    if (amy_punchloop_mode == AMY_PUNCHLOOP_FIXED) {
        uint32_t len = amy_punchloop_fixed_len;
        if (len < 1 || amy_punch_loop_buf == NULL) return;
        for (int32_t i = 0; i < AMY_BLOCK_SIZE; i++) {
            uint32_t rf = ((uint32_t)amy_punchloop_read_frame) % len;
            for (int32_t c = 0; c < AMY_NCHANS; c++)
                output_block[i * AMY_NCHANS + c] = amy_punch_loop_buf[rf * AMY_NCHANS + c];
            amy_punchloop_read_frame += 1.0f;
            if (amy_punchloop_read_frame >= (float)len) amy_punchloop_read_frame -= (float)len;
        }
        return;
    }
    // AMY_PUNCHLOOP_RATE: REVERSE / HALFSPD / TAPESTOP, reading from the live history ring.
    if (amy_punch_cap_buf == NULL) return;
    for (int32_t i = 0; i < AMY_BLOCK_SIZE; i++) {
        for (int32_t c = 0; c < AMY_NCHANS; c++)
            output_block[i * AMY_NCHANS + c] = amy_punch_read_interp(amy_punch_cap_buf, amy_punchloop_read_frame, c);
        amy_punchloop_read_frame += amy_punchloop_rate;
        if (amy_punchloop_rate_decel > 0.0f) amy_punchloop_rate *= amy_punchloop_rate_decel;  // TAPESTOP
        // Safety clamp: never let the read cursor fall more than (capacity - 1 block) behind
        // the live write cursor (in either direction) — a very long hold at a slow/negative
        // rate could otherwise eventually read samples the write cursor is about to recycle.
        float age = (float)amy_punch_cap_write_frame - amy_punchloop_read_frame;
        while (age < 0) age += AMY_PUNCH_CAP_FRAMES;
        float maxAge = (float)(AMY_PUNCH_CAP_FRAMES - AMY_BLOCK_SIZE);
        if (age > maxAge) {
            amy_punchloop_read_frame = (float)amy_punch_cap_write_frame - maxAge;
            while (amy_punchloop_read_frame < 0) amy_punchloop_read_frame += AMY_PUNCH_CAP_FRAMES;
        }
    }
}

// ---- Public API (amy.h) — called from GrvEP's fxPunchApply()/fxPunchRevert() ----
void amy_punch_loop_start(uint32_t len_frames) {
    amy_punch_cap_ensure_alloc();
    if (amy_punch_cap_buf == NULL || amy_punch_loop_buf == NULL) return;
    if (len_frames < 1) len_frames = 1;
    if (len_frames > AMY_PUNCH_CAP_FRAMES) len_frames = AMY_PUNCH_CAP_FRAMES;
    // Copy the most recent len_frames of history (ending at "now") into the dedicated,
    // never-overwritten-while-looping snapshot buffer.
    uint32_t start = (amy_punch_cap_write_frame + AMY_PUNCH_CAP_FRAMES - len_frames) % AMY_PUNCH_CAP_FRAMES;
    for (uint32_t i = 0; i < len_frames; i++) {
        uint32_t sf = (start + i) % AMY_PUNCH_CAP_FRAMES;
        for (int32_t c = 0; c < AMY_NCHANS; c++)
            amy_punch_loop_buf[i * AMY_NCHANS + c] = amy_punch_cap_buf[sf * AMY_NCHANS + c];
    }
    amy_punch_loop_len_frames = len_frames;
    amy_punchloop_fixed_len = len_frames;
    amy_punchloop_read_frame = 0.0f;
    amy_punchloop_rate = 1.0f;
    amy_punchloop_rate_decel = 0.0f;
    amy_punchloop_mode = AMY_PUNCHLOOP_FIXED;
}
void amy_punch_reverse_start() {
    amy_punch_cap_ensure_alloc();
    amy_punchloop_read_frame = (float)amy_punch_cap_write_frame;
    amy_punchloop_rate = -1.0f;
    amy_punchloop_rate_decel = 0.0f;
    amy_punchloop_mode = AMY_PUNCHLOOP_RATE;
}
void amy_punch_halfspeed_start() {
    amy_punch_cap_ensure_alloc();
    amy_punchloop_read_frame = (float)amy_punch_cap_write_frame;
    amy_punchloop_rate = 0.5f;
    amy_punchloop_rate_decel = 0.0f;
    amy_punchloop_mode = AMY_PUNCHLOOP_RATE;
}
void amy_punch_tapestop_start(float tapestop_seconds) {
    amy_punch_cap_ensure_alloc();
    amy_punchloop_read_frame = (float)amy_punch_cap_write_frame;
    amy_punchloop_rate = 1.0f;
    // Multiplicative per-frame decay so rate reaches ~1% of its starting value after
    // tapestop_seconds — an exponential "motor spinning down" curve, not linear.
    float frames = tapestop_seconds * (float)AMY_SAMPLE_RATE;
    amy_punchloop_rate_decel = (frames > 1.0f) ? powf(0.01f, 1.0f / frames) : 0.0f;
    amy_punchloop_mode = AMY_PUNCHLOOP_RATE;
}
void amy_punch_loop_stop() {
    amy_punchloop_mode = AMY_PUNCHLOOP_NONE;
}

int16_t * amy_fill_buffer() {
    AMY_PROFILE_START(AMY_FILL_BUFFER)
    #ifdef __EMSCRIPTEN__
    // post a message to the main thread of the audioworklet (amy main, in this case) that a block has been finished
    //emscripten_audio_worklet_post_function_v(0, amy_block_processed);
    #else
    amy_block_processed();
    #endif

    // Double-buffer the output block.
    if (output_block == output_block_0)  output_block = output_block_1;
    else output_block = output_block_0;

    // mix results from both cores.
    //SAMPLE max_val = core_max[0];
    #ifdef AMY_DUALCORE
    for (int bus = 0; bus <= amy_global.highest_bus; ++bus)
        for (int16_t i=0; i < AMY_BLOCK_SIZE * AMY_NCHANS; ++i)  fbl[0][bus][i] += fbl[1][bus][i];
    //    if (core_max[1] > max_val)  max_val = core_max[1];
    #endif
    // Apply global processing only if there is some signal.
    //if (max_val > 0) {      // NO - see #629
        // apply the eq filters if there is some signal and EQ is non-default.
    // Wavefolder on T303 bus (bus 1) BEFORE per-bus FX so reverb/chorus act on folded signal.
    // AMY bus buffers carry large-amplitude signals (±~10), so normalize to ±1 first,
    // apply the triangle fold, then scale back — avoiding near-silence from over-folding.
    if (amy_wavefold_gain > 1.001f && amy_global.highest_bus >= 1 && fbl[0][1] != NULL) {
        SAMPLE* buf = fbl[0][1];
        int n = AMY_BLOCK_SIZE * AMY_NCHANS;
        float peak = 0.0f;
        for (int i = 0; i < n; i++) { float a = fabsf(buf[i]); if (a > peak) peak = a; }
        if (peak > 0.001f) {
            float g = amy_wavefold_gain;
            float inv_peak = 1.0f / peak;
            if (amy_wavefold_pos_only > 0.5f) {
                // Asymmetric fold: positive peaks fold, negative half passes through (bass retention).
                for (int i = 0; i < n; i++) {
                    float s = buf[i] * inv_peak;
                    if (s > 0.0f) {
                        float sg = s * g;
                        float phase = fmodf(sg + 1.0f, 4.0f);
                        if (phase < 0.0f) phase += 4.0f;
                        buf[i] = ((phase < 2.0f) ? (phase - 1.0f) : (3.0f - phase)) * peak;
                    }
                    // negative half: buf[i] unchanged (already scaled by inv_peak... wait, no)
                }
            } else {
                // Symmetric fold (original behaviour).
                for (int i = 0; i < n; i++) {
                    float s = buf[i] * inv_peak * g;
                    float phase = fmodf(s + 1.0f, 4.0f);
                    if (phase < 0.0f) phase += 4.0f;
                    buf[i] = ((phase < 2.0f) ? (phase - 1.0f) : (3.0f - phase)) * peak;
                }
            }
        }
    }

    // Merge T303 bus (bus 1) into bus 0 BEFORE FX so reverb/chorus/echo/EQ on bus 0
    // process the combined signal. Bus 1 is zeroed to avoid double-counting in final mix.
    // No extra PSRAM allocation needed — only bus 0 FX buffers are used.
    if (amy_global.highest_bus >= 1 && fbl[0][0] != NULL && fbl[0][1] != NULL) {
        int n = AMY_BLOCK_SIZE * AMY_NCHANS;
        for (int i = 0; i < n; i++) fbl[0][0][i] += fbl[0][1][i];
        memset(fbl[0][1], 0, n * sizeof(SAMPLE));
    }

    // GrvEP's own bus-0 FX (FILT/DRIVE/RESECHO/REP/BITCRS/RINGMOD/COMPRESS), applied in
    // activation order — see amy_apply_ordered_fx() above.
    amy_apply_ordered_fx(fbl[0][0]);

    for (int bus=0; bus <= amy_global.highest_bus; ++bus) {
        // Per-bus EQ
        if (amy_global.bus[bus]->eq.eq[0] != F2S(1.0f) || amy_global.bus[bus]->eq.eq[1] != F2S(1.0f) || amy_global.bus[bus]->eq.eq[2] != F2S(1.0f)) {
            parametric_eq_process(bus, fbl[0][bus]);
        }
        if(AMY_HAS_CHORUS) {
            // apply per-bus chorus.
            if(amy_global.bus[bus]->chorus.level > 0 && amy_global.bus[bus]->chorus.chorus_delay_lines[0] != NULL) {
                // apply time-varying delays to both chans.
                // delay_mod_val, the modulated delay amount, is set up before calling render_*.
                SAMPLE scale = F2S(1.0f);
                for (int16_t c=0; c < AMY_NCHANS; ++c) {
                    apply_variable_delay(fbl[0][bus] + c * AMY_BLOCK_SIZE, amy_global.bus[bus]->chorus.chorus_delay_lines[c],
                                         amy_global.bus[bus]->chorus.delay_mod, scale, amy_global.bus[bus]->chorus.level, 0);
                    // flip delay direction for alternating channels.
                    scale = -scale;
                }
            }
        }
        //}
        if (AMY_HAS_ECHO) {
            // Apply per-bus echo.
            if (amy_global.bus[bus]->echo.level > 0 && amy_global.bus[bus]->echo.echo_delay_lines[0] != NULL ) {
                for (int16_t c=0; c < AMY_NCHANS; ++c) {
                    apply_fixed_delay(fbl[0][bus] + c * AMY_BLOCK_SIZE, amy_global.bus[bus]->echo.echo_delay_lines[c], amy_global.bus[bus]->echo.delay_samples, amy_global.bus[bus]->echo.level, amy_global.bus[bus]->echo.feedback, amy_global.bus[bus]->echo.filter_coef);
                }
            }
        }
        if(AMY_HAS_REVERB) {
            // apply per-bus reverb.
            if(amy_global.bus[bus]->reverb.level > 0 && amy_global.bus[bus]->reverb.rev != NULL && amy_global.bus[bus]->reverb.rev->delay_1 != NULL) {
                if(AMY_NCHANS == 1) {
                    stereo_reverb(amy_global.bus[bus]->reverb.rev, fbl[0][bus], NULL, fbl[0][bus], NULL, AMY_BLOCK_SIZE, amy_global.bus[bus]->reverb.level);
                } else {
                    stereo_reverb(amy_global.bus[bus]->reverb.rev, fbl[0][bus], fbl[0][bus] + AMY_BLOCK_SIZE, fbl[0][bus], fbl[0][bus] + AMY_BLOCK_SIZE, AMY_BLOCK_SIZE, amy_global.bus[bus]->reverb.level);
                }
            }
        }
    }  // end of per-bus FX

    // global volume is supposed to max out at 10, so scale by 0.1.
    SAMPLE volume_scale[AMY_NUM_BUSES];
    for (int bus = 0; bus <= amy_global.highest_bus; ++bus)
        volume_scale[bus] = MUL4_SS(F2S(0.1f), F2S(amy_global.volume[bus]));
    for(int16_t i=0; i < AMY_BLOCK_SIZE; ++i) {
        for (int16_t c=0; c < AMY_NCHANS; ++c) {

            SAMPLE fsample = 0;
            for (int bus = 0; bus <= amy_global.highest_bus; ++bus) {
                // Convert the mixed sample into the int16 range, applying overall gain.
                fsample += MUL8_SS(volume_scale[bus], fbl[0][bus][i + c * AMY_BLOCK_SIZE]);
            }

            // One-pole high-pass filter to remove large low-frequency excursions from
            // some FM patches. b = [1 -1]; a = [1 -0.995]
            //SAMPLE new_state = fsample + SMULR6(F2S(0.995f), amy_global.hpf_state);  // High-output-range, rounded MUL is critical here.
#ifdef AMY_HPF_OUTPUT
            SAMPLE new_state = fsample + amy_global.hpf_state
                - SHIFTR(amy_global.hpf_state + SHIFTR(F2S(1.0), 16), 8);  // i.e. 0.9961*hpf_state
            fsample = new_state - amy_global.hpf_state;
            amy_global.hpf_state = new_state;
#endif

            // soft clipping.
            int positive = 1;
            if (fsample < 0) {
                positive = 0;
                // avoid fabs()
                fsample = -fsample;
            }

            int32_t uintval = S2L(fsample);
            if (uintval >= FIRST_NONLIN) {
                if (uintval >= FIRST_HARDCLIP) {
                    uintval = SAMPLE_MAX;
                } else {
                    uintval = clipping_lookup_table[uintval - FIRST_NONLIN];
                }
            }
            int16_t sample;

            // TODO -- the esp stuff here could sit outside of AMY
            // For some reason, have to drop a bit to stop hard wrapping on esp?
#if defined(ESP_PLATFORM) || defined(__IMXRT1062__)
                uintval >>= 1;
            #endif
            if (positive) {
              sample = uintval;
            } else {
              sample = -uintval;
            }
            if(AMY_NCHANS == 1) {
                #ifdef ESP_PLATFORM
                    // esp32's i2s driver has this bug
                    output_block[i ^ 0x01] = sample;
                #else
                    output_block[i] = sample;
                #endif
            } else {
                output_block[(AMY_NCHANS * i) + c] = sample;
            }
        }
    }

    // Handle sampling after block is rendered
    if(amy_global.transfer_flag==AMY_TRANSFER_TYPE_SAMPLE) {
        uint32_t bytes_per_frame = AMY_NCHANS * sizeof(int16_t);
        uint32_t byte_offset = amy_global.transfer_stored_bytes;
        uint32_t bytes_to_copy = AMY_BLOCK_SIZE * bytes_per_frame;
        // Crop the bytes to copy if this block would exceed the requested length.
        if(byte_offset + bytes_to_copy >= amy_global.transfer_length_bytes) {
            bytes_to_copy = amy_global.transfer_length_bytes - byte_offset;
        }
        if(amy_global.transfer_file_handle==SAMPLE_FROM_OUTPUT) {
            // copy block[] to amy_global.transfer_storage
            memcpy(amy_global.transfer_storage + byte_offset, output_block, bytes_to_copy);
        } else if(amy_global.transfer_file_handle==SAMPLE_FROM_AUDIO_IN) {
            // copy audio input buffer to storage
            memcpy(amy_global.transfer_storage + byte_offset, amy_in_block, bytes_to_copy);
        }
        amy_global.transfer_stored_bytes += AMY_BLOCK_SIZE * AMY_NCHANS * sizeof(int16_t);
        if(amy_global.transfer_stored_bytes >= amy_global.transfer_length_bytes) {   
            amy_global.transfer_flag = AMY_TRANSFER_TYPE_NONE;
        }
    }
    amy_global.total_blocks++;
    amy_global.total_samples = amy_global.total_blocks * AMY_BLOCK_SIZE;
    amy_global.time = amy_global.total_samples / (float)AMY_SAMPLE_RATE;

    // FX Punch "looper" engine: capture this block's live final mix (for the NEXT punch,
    // or just to keep history fresh), then — only if a loop/reverse/tapestop/halfspeed
    // punch is currently held — overwrite output_block[] with its read-back. See the
    // engine's own comment above amy_fill_buffer() for why this never touches the live
    // render itself.
    amy_punch_capture_block();
    amy_punch_override_block();

    AMY_PROFILE_STOP(AMY_FILL_BUFFER)

    amy_out_block = output_block;
    return output_block;
}

void amy_reset_sysclock() {
    amy_global.total_blocks = 0;
    amy_global.total_samples = 0;
    amy_global.time = 0;
    amy_global.sequencer_tick_count = 0;
    sequencer_recompute();
}


/// Delta pool management

struct delta *free_deltas_pool = NULL;

#define MAX_DELTA_BLOCKS 16
struct delta *delta_blocks[MAX_DELTA_BLOCKS];
int next_delta_block = 0;

#define DELTA_BLOCK_SIZE 2048

struct delta *deltas_pool_alloc(int max_delta_pool_size, struct delta *tail) {
    struct delta *new_pool = (struct delta *)malloc_caps(max_delta_pool_size * sizeof(struct delta),
                                                         amy_global.config.ram_caps_synth);
    struct delta *d = new_pool;
    // Link all the deltas together
    for (int i = 1; i < max_delta_pool_size; ++i) {
        d->next = d + 1;
        d->osc = 0;
        d->data.i = 0;
        d->param = NO_PARAM;
        d->time = UINT32_MAX - 1;
        d = d->next;
    }
    d->next = tail;  // join up the end of the chain with passed-in chain (or NULL).
    return new_pool;
}

void deltas_add_pool_block(void) {
    //fprintf(stderr, "deltas_add_pool_block %d\n", next_delta_block);
    if (next_delta_block >= MAX_DELTA_BLOCKS) {
        fprintf(stderr, "**PANIC: Ran out of deltas (%d blocks of %d deltas)\n", MAX_DELTA_BLOCKS, DELTA_BLOCK_SIZE);
        abort();
    }
    free_deltas_pool = delta_blocks[next_delta_block++] = deltas_pool_alloc(DELTA_BLOCK_SIZE, free_deltas_pool);
}

void deltas_pool_init() {
    deltas_pool_free();
    deltas_add_pool_block();
}

void deltas_pool_free() {
    for (int i = 0; i < next_delta_block; ++i) {
        free(delta_blocks[i]);
    }
    next_delta_block = 0;
    free_deltas_pool = NULL;
}

struct delta *delta_get(struct delta *from) {
    // fetch the next free delta, copy in values if a reference is provided.
    struct delta *d = free_deltas_pool;
    if (d == NULL)  {
        deltas_add_pool_block();
        d = free_deltas_pool;
    }
    free_deltas_pool = d->next;
    if (from != NULL) {
        d->data.i = from->data.i;
        d->param = from->param;
        d->time = from->time;
        d->osc = from->osc;
        // don't copy from->next
    }
    d->next = NULL;
    return d;
}

struct delta *delta_release(struct delta *d) {
    // return a delta to the pool.  Returns the next value, for ease closing up
    struct delta *next_delta = d->next;
    d->next = free_deltas_pool;
    d->osc = 0;
    d->data.i = 0;
    d->param = NO_PARAM;
    d->time = UINT32_MAX - 1;

    free_deltas_pool = d;

    return next_delta;
}

void delta_release_list(struct delta *d) {
    // return a delta and all its sequestrae to the pool.
    while(d)  d = delta_release(d);
}

int32_t delta_list_len(struct delta *d) {
    int32_t len = 0;
    while(d) {
        ++len;
        d = d->next;
    }
    return len;
}

int32_t delta_num_free() {
    return delta_list_len(free_deltas_pool);
}
