#include "BluetoothA2DPSink.h"
#include "driver/i2s_std.h"
#include <math.h>
#include <string.h>
#include "freertos/ringbuf.h"
#include <Preferences.h>

#define SAMPLE_RATE 44100

#define I2S_BCK_IO (GPIO_NUM_26)
#define I2S_WS_IO  (GPIO_NUM_25)
#define I2S_DO_IO  (GPIO_NUM_22)

// --- Jitter-critical constants ---
static const size_t kRingBytes     = 32 * 1024;           // ~186 ms at 44.1k stereo s16
static const size_t kChunkBytes    = 512 * 4;             // 512 frames = ~11.6 ms
static const size_t kPrefetchBytes = (kRingBytes * 3) / 5; // ~60% before I2S starts
static const int    kDspPriority   = configMAX_PRIORITIES - 3;
static const float  kPcmScale      = 1.0f / 32768.0f;
static const float  kDacScale      = 24000.0f;

enum FilterSelection { MASK_5KHZ = 0, MASK_9KHZ, MASK_10KHZ, MASK_12KHZ, MASK_15KHZ };

struct DynamicsSettings {
    float threshold = 0.3f;
    float ratio = 4.0f;
    float attack_coef = 0.0022f;
    float release_coef = 0.00022f;
    float gate_threshold = 0.01f;
};

struct ProcessorSettings {
    float master_gain = 1.0f;
    DynamicsSettings low_comp;
    DynamicsSettings mid_comp;
    DynamicsSettings high_comp;
    DynamicsSettings slow_comp;

    float pos_clip_limit = 1.25f;
    float neg_clip_limit = -0.95f;

    FilterSelection mask_selection = MASK_10KHZ;
    bool generator_on = false;
    float gen_freq = 400.0f;
    bool phase_rotator_on = true;
    bool tilt_test_on = false;
    float tilt_freq = 75.0f;
    float output_gain = 1.0f;

    uint8_t waveform_type = 0;     // 0=Sine, 1=Square, 2=Sawtooth
    bool tone_post_clipper = false;
    uint8_t hpf_freq = 50;
    float lr_limit = 0.75f;
};

// Not volatile: we snapshot once per block under a spinlock.
ProcessorSettings settings;
static portMUX_TYPE g_settings_mux = portMUX_INITIALIZER_UNLOCKED;

Preferences prefs;
BluetoothA2DPSink a2dp_sink;
float tone_phase = 0.0f;

i2s_chan_handle_t tx_handle = NULL;
RingbufHandle_t audio_ring_buffer = NULL;
TaskHandle_t dsp_task_handle = NULL;

volatile uint32_t audio_packets_received = 0;
volatile uint32_t audio_packets_dropped = 0;
volatile bool g_stream_reset = true;
volatile bool g_mask_dirty = true;

// --- Phase rotator: 4× first-order allpass, 90° at ~200 Hz ---
// H(z) = (a + z^{-1}) / (1 + a z^{-1})
// a = (tan(π fb/fs) - 1) / (tan(π fb/fs) + 1)  ≈ -0.972 at 200 Hz / 44.1 kHz
static const float kRotatorHz = 200.0f;

struct AllPassFilter {
    float a = 0.0f;
    float x1 = 0.0f, y1 = 0.0f;

    void set_coeff(float coeff) { a = coeff; }

    void reset() { x1 = y1 = 0.0f; }

    inline float process(float in) {
        const float out = a * in + x1 - a * y1;
        x1 = in;
        y1 = out;
        return out;
    }
};

struct PhaseRotator {
    AllPassFilter stage1, stage2, stage3, stage4;

    static float coeff_for(float fc) {
        const float t = tanf((float)M_PI * fc / (float)SAMPLE_RATE);
        return (t - 1.0f) / (t + 1.0f);
    }

    void init(float fc = kRotatorHz) {
        const float a = coeff_for(fc);
        stage1.set_coeff(a);
        stage2.set_coeff(a);
        stage3.set_coeff(a);
        stage4.set_coeff(a);
        reset();
    }

    void reset() {
        stage1.reset();
        stage2.reset();
        stage3.reset();
        stage4.reset();
    }

    inline float process(float in) {
        float out = stage1.process(in);
        out = stage2.process(out);
        out = stage3.process(out);
        out = stage4.process(out);
        return out;
    }
};


struct BandCompressor {
    float env = 0.0f;
    inline void process(float &signal, const DynamicsSettings &cfg) {
        float abs_sig = fabsf(signal);

        if (abs_sig > env) env += cfg.attack_coef * (abs_sig - env);
        else               env += cfg.release_coef * (abs_sig - env);

        if (env < cfg.gate_threshold) return;

        if (env > cfg.threshold && env > 0.0001f) {
            float target_env = cfg.threshold + (env - cfg.threshold) / cfg.ratio;
            signal *= target_env / env;
        }
    }
};

static const int   kLrLookahead    = 220;      // ~5 ms @ 44.1 kHz
static const float kLrReleaseCoef  = 0.0005f;  // ~45 ms
static const float kLrRelToMid     = 0.85f;    // |S| <= 0.85 |M|
static const float kLrAbsFloor     = 0.10f;

float lr_delay[kLrLookahead][2];  // [0]=M, [1]=S
int   lr_write_index = 0;
float lr_env = 0.0f;
int   lr_hold = 0;

static void reset_lr_limiter() {
    memset(lr_delay, 0, sizeof(lr_delay));
    lr_write_index = 0;
    lr_env = 0.0f;
    lr_hold = 0;
}

static inline void process_lr_limiter(float &left, float &right, float limit) {
    if (limit < 0.05f) limit = 0.05f;
    else if (limit > 1.0f) limit = 1.0f;

    const float mid  = 0.5f * (left + right);
    const float side = 0.5f * (left - right);

    const int i = lr_write_index;
    const float delayed_m = lr_delay[i][0];
    const float delayed_s = lr_delay[i][1];
    lr_delay[i][0] = mid;
    lr_delay[i][1] = side;
    if (++lr_write_index >= kLrLookahead) lr_write_index = 0;

    const float abs_s = fabsf(side);
    if (abs_s >= lr_env) {
        lr_env  = abs_s;
        lr_hold = kLrLookahead;
    } else if (lr_hold > 0) {
        --lr_hold;
    } else {
        lr_env += kLrReleaseCoef * (abs_s - lr_env);
    }

    float gr = 1.0f;
    const float knee = limit * 0.75f;
    if (lr_env > knee && lr_env > 1e-6f) {
        if (lr_env >= limit) {
            gr = limit / lr_env;
        } else {
            const float t = (lr_env - knee) / (limit - knee);
            const float desired = knee + t * t * (limit - knee);
            gr = desired / lr_env;
        }
    }

    float m = delayed_m;
    float s = delayed_s * gr;

    float ceiling = fabsf(m) * kLrRelToMid;
    const float floor = fminf(kLrAbsFloor, limit);
    if (ceiling > limit) ceiling = limit;
    if (ceiling < floor)  ceiling = floor;

    const float as = fabsf(s);
    if (as > ceiling) s *= ceiling / as;

    left  = m + s;
    right = m - s;
}


BandCompressor compL_low, compL_mid, compL_high;
BandCompressor compR_low, compR_mid, compR_high;
BandCompressor slowAGC_L, slowAGC_R;
PhaseRotator rotatorL, rotatorR;

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void reset() { x1 = x2 = y1 = y2 = 0; }

    void setLPF(float frequency, float q) {
        float omega = 2.0f * M_PI * frequency / SAMPLE_RATE;
        float alpha = sinf(omega) / (2.0f * q);
        float cosw = cosf(omega);
        float b0_raw = (1.0f - cosw) / 2.0f;
        float a0 = 1.0f + alpha;
        b0 = b0_raw / a0;
        b1 = (1.0f - cosw) / a0;
        b2 = b0_raw / a0;
        a1 = (-2.0f * cosw) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void setHPF(float frequency, float q) {
        float omega = 2.0f * M_PI * frequency / SAMPLE_RATE;
        float alpha = sinf(omega) / (2.0f * q);
        float cosw = cosf(omega);
        float b0_raw = (1.0f + cosw) / 2.0f;
        float a0 = 1.0f + alpha;
        b0 = b0_raw / a0;
        b1 = -(1.0f + cosw) / a0;
        b2 = b0_raw / a0;
        a1 = (-2.0f * cosw) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    inline float process(float in) {
        float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in; y2 = y1; y1 = out;
        return out;
    }
};

struct LRCrossover3Band {
    Biquad lp1_L, lp2_L, hp1_L, hp2_L, lp3_L, lp4_L, hp3_L, hp4_L;
    Biquad lp1_R, lp2_R, hp1_R, hp2_R, lp3_R, lp4_R, hp3_R, hp4_R;

    void init(float low_mid_freq, float mid_high_freq) {
        const float q = 0.707f;
        lp1_L.setLPF(low_mid_freq, q); lp2_L.setLPF(low_mid_freq, q);
        hp1_L.setHPF(low_mid_freq, q); hp2_L.setHPF(low_mid_freq, q);
        lp3_L.setLPF(mid_high_freq, q); lp4_L.setLPF(mid_high_freq, q);
        hp3_L.setHPF(mid_high_freq, q); hp4_L.setHPF(mid_high_freq, q);

        lp1_R.setLPF(low_mid_freq, q); lp2_R.setLPF(low_mid_freq, q);
        hp1_R.setHPF(low_mid_freq, q); hp2_R.setHPF(low_mid_freq, q);
        lp3_R.setLPF(mid_high_freq, q); lp4_R.setLPF(mid_high_freq, q);
        hp3_R.setHPF(mid_high_freq, q); hp4_R.setHPF(mid_high_freq, q);
    }
};

LRCrossover3Band crossover;
Biquad maskFilterL1, maskFilterL2, maskFilterR1, maskFilterR2;

static inline float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static inline float clip_asym(float x, float pos, float neg) {
    if (x > pos) return pos;
    if (x < neg) return neg;
    return x;
}

static inline int16_t float_to_pcm(float x) {
    float y = x * kDacScale;
    if (y >  32767.0f) y =  32767.0f;
    if (y < -32768.0f) y = -32768.0f;
    return (int16_t)lrintf(y);
}

static ProcessorSettings snapshot_settings() {
    ProcessorSettings copy;
    portENTER_CRITICAL(&g_settings_mux);
    memcpy(&copy, &settings, sizeof(copy));
    portEXIT_CRITICAL(&g_settings_mux);
    return copy;
}

static UBaseType_t ring_bytes_waiting() {
    UBaseType_t waiting = 0;
    if (audio_ring_buffer) {
        vRingbufferGetInfo(audio_ring_buffer, NULL, NULL, NULL, NULL, &waiting);
    }
    return waiting;
}

static void drain_ring_buffer() {
    if (!audio_ring_buffer) return;
    size_t sz = 0;
    void *item;
    while ((item = xRingbufferReceiveUpTo(audio_ring_buffer, &sz, 0, kChunkBytes)) != NULL) {
        vRingbufferReturnItem(audio_ring_buffer, item);
    }
}

inline float generate_test_waveform(float phase, uint8_t type) {
    float t = phase / (2.0f * M_PI);
    if (type == 0) {
        return sinf(phase);
    }
    if (type == 1) {
        return (t < 0.50f) ? 1.0f : -1.0f;
    }
    if (type == 2) {
        if (t < 0.05f) {
            return 1.0f;
        }
        if (t < 0.10f) {
            float ramp = (t - 0.05f) / 0.05f;
            return 1.0f + (ramp * 0.25f);
        }
        float ramp = (t - 0.10f) / 0.90f;
        return 1.25f - (ramp * 2.25f);
    }
    if (t < 0.90f) {
        float ramp = t / 0.90f;
        return -1.0f + (ramp * 2.0f);
    }
    if (t < 0.95f) {
        return 1.0f;
    }
    float ramp = (t - 0.95f) / 0.05f;
    return 1.0f + (ramp * 0.25f);
}

void update_mask_filter(FilterSelection selection) {
    float freq = 10000.0f;
    if (selection == MASK_5KHZ)       freq = 5000.0f;
    else if (selection == MASK_9KHZ)  freq = 9000.0f;
    else if (selection == MASK_10KHZ) freq = 10000.0f;
    else if (selection == MASK_12KHZ) freq = 12000.0f;
    else if (selection == MASK_15KHZ) freq = 15000.0f;

    maskFilterL1.setLPF(freq, 0.707f);
    maskFilterL2.setLPF(freq, 0.707f);
    maskFilterR1.setLPF(freq, 0.707f);
    maskFilterR2.setLPF(freq, 0.707f);
}

static void inject_tone(float &left, float &right, const ProcessorSettings &cfg) {
    float freq = cfg.generator_on ? cfg.gen_freq : cfg.tilt_freq;
    uint8_t wtype = cfg.generator_on ? cfg.waveform_type : 1;
    float val = generate_test_waveform(tone_phase, wtype);
    left = val;
    right = val;
    tone_phase += (2.0f * M_PI * freq) / SAMPLE_RATE;
    if (tone_phase >= 2.0f * M_PI) tone_phase -= 2.0f * M_PI;
}

static void process_program_path(float &left, float &right, const ProcessorSettings &cfg) {
    left  *= cfg.master_gain;
    right *= cfg.master_gain;

    if (cfg.phase_rotator_on) {
        left  = rotatorL.process(left);
        right = rotatorR.process(right);
    }

    // HPF is applied by the caller (coefficients updated between blocks)

    slowAGC_L.process(left, cfg.slow_comp);
    slowAGC_R.process(right, cfg.slow_comp);

    float low_L  = crossover.lp2_L.process(crossover.lp1_L.process(left));
    float high_L = crossover.hp2_L.process(crossover.hp1_L.process(left));
    float mid_L  = crossover.lp4_L.process(crossover.lp3_L.process(high_L));
    high_L       = crossover.hp4_L.process(crossover.hp3_L.process(high_L));

    float low_R  = crossover.lp2_R.process(crossover.lp1_R.process(right));
    float high_R = crossover.hp2_R.process(crossover.hp1_R.process(right));
    float mid_R  = crossover.lp4_R.process(crossover.lp3_R.process(high_R));
    high_R       = crossover.hp4_R.process(crossover.hp3_R.process(high_R));

    compL_low.process(low_L, cfg.low_comp);
    compL_mid.process(mid_L, cfg.mid_comp);
    compL_high.process(high_L, cfg.high_comp);
    compR_low.process(low_R, cfg.low_comp);
    compR_mid.process(mid_R, cfg.mid_comp);
    compR_high.process(high_R, cfg.high_comp);

    left  = low_L + mid_L + high_L;
    right = low_R + mid_R + high_R;

    process_lr_limiter(left, right, cfg.lr_limit);
}


// Producer: BT thread. Never receive. Never block.
void audio_data_callback(const uint8_t *data, uint32_t length) {
    if (!audio_ring_buffer || !data || length == 0) return;

    if (xRingbufferSend(audio_ring_buffer, (void *)data, length, 0) == pdTRUE) {
        __atomic_fetch_add(&audio_packets_received, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&audio_packets_dropped, 1, __ATOMIC_RELAXED);
    }
}

void on_connection_state(esp_a2d_connection_state_t state, void *) {
    if (state != ESP_A2D_CONNECTION_STATE_CONNECTED) {
        g_stream_reset = true;
    }
}

void on_audio_state(esp_a2d_audio_state_t state, void *) {
    if (state == ESP_A2D_AUDIO_STATE_STARTED) {
        g_stream_reset = true;
    } else {
        g_stream_reset = true;
    }
}

// Pull up to max_bytes, stitching a ring wrap and any leftover odd bytes
// so DSP never sees a torn stereo frame.
static size_t assemble_chunk(uint8_t *dst, size_t max_bytes, TickType_t first_wait) {
    static uint8_t tail[4];
    static size_t tail_n = 0;

    size_t filled = 0;
    if (tail_n) {
        memcpy(dst, tail, tail_n);
        filled = tail_n;
        tail_n = 0;
    }

    TickType_t wait = first_wait;
    while (filled < max_bytes) {
        size_t got = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(
            audio_ring_buffer, &got, wait, max_bytes - filled);
        wait = 0;  // only block on the first pull
        if (!item) break;
        memcpy(dst + filled, item, got);
        filled += got;
        vRingbufferReturnItem(audio_ring_buffer, item);
    }

    size_t aligned = filled & ~size_t{3};
    tail_n = filled - aligned;
    if (tail_n) memcpy(tail, dst + aligned, tail_n);
    return aligned;
}

void dsp_processing_task(void *pvParameters) {
    Biquad hpfL, hpfR;
    uint8_t last_hpf = 0xFF;
    bool primed = false;

    // +4 bytes of headroom so a 1-frame clock slip can duplicate a sample
    static uint8_t chunk_bytes[kChunkBytes + 4];
    int16_t *chunk = (int16_t *)chunk_bytes;

    while (true) {
            if (g_stream_reset) {
                drain_ring_buffer();
                reset_lr_limiter();
                rotatorL.reset();
                rotatorR.reset();
                primed = false;
                g_stream_reset = false;
            }


        ProcessorSettings cfg = snapshot_settings();
        const bool tone_active = cfg.generator_on || cfg.tilt_test_on;

        if (g_mask_dirty) {
            update_mask_filter(cfg.mask_selection);
            g_mask_dirty = false;
        }

        if (cfg.hpf_freq != last_hpf) {
            float f = clampf((float)cfg.hpf_freq, 50.0f, 100.0f);
            hpfL.setHPF(f, 0.707f);
            hpfR.setHPF(f, 0.707f);
            last_hpf = cfg.hpf_freq;
        }

        if (!primed && !tone_active) {
            if (ring_bytes_waiting() < kPrefetchBytes) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            primed = true;
        }

        size_t nbytes = assemble_chunk(chunk_bytes, kChunkBytes, pdMS_TO_TICKS(20));

        if (nbytes < 4) {
            if (!tone_active) {
                primed = false;  // underrun: re-prefetch, don't spin
                continue;
            }
            nbytes = kChunkBytes;
            memset(chunk_bytes, 0, nbytes);
        }

        // Mild clock-domain slip: drop/dup 1 frame when the jitter buffer
        // leaves the deadband. ~0.2% at 512-frame chunks, only when needed.
        UBaseType_t waiting = ring_bytes_waiting();
        int slip = 0;
        if (!tone_active) {
            if (waiting > (UBaseType_t)(kRingBytes * 4 / 5))      slip = -1;
            else if (primed && waiting < (UBaseType_t)(kRingBytes / 4)) slip = +1;
        }

        uint32_t frames = nbytes / 4;
        if (slip < 0 && frames > 1) frames -= 1;

        for (uint32_t i = 0; i < frames; ++i) {
            float left  = chunk[i * 2]     * kPcmScale;
            float right = chunk[i * 2 + 1] * kPcmScale;

            const bool pre_tone  = tone_active && !cfg.tone_post_clipper;
            const bool post_tone = tone_active &&  cfg.tone_post_clipper;

            if (!pre_tone && !cfg.generator_on) {
                left  = hpfL.process(left);
                right = hpfR.process(right);
                process_program_path(left, right, cfg);
            } else if (!pre_tone) {
                left = 0.0f;
                right = 0.0f;
            }

            if (pre_tone) inject_tone(left, right, cfg);

            left  = clip_asym(left,  cfg.pos_clip_limit, cfg.neg_clip_limit);
            right = clip_asym(right, cfg.pos_clip_limit, cfg.neg_clip_limit);

            left  = maskFilterL2.process(maskFilterL1.process(left));
            right = maskFilterR2.process(maskFilterR1.process(right));

            left  *= cfg.output_gain;
            right *= cfg.output_gain;

            if (post_tone) inject_tone(left, right, cfg);

            left  = clip_asym(left,  cfg.pos_clip_limit, cfg.neg_clip_limit);
            right = clip_asym(right, cfg.pos_clip_limit, cfg.neg_clip_limit);

            chunk[i * 2]     = float_to_pcm(left);
            chunk[i * 2 + 1] = float_to_pcm(right);
        }

        if (slip > 0 && frames > 0) {
            chunk[frames * 2]     = chunk[(frames - 1) * 2];
            chunk[frames * 2 + 1] = chunk[(frames - 1) * 2 + 1];
            frames += 1;
        }

        size_t written = 0;
        const size_t out_bytes = frames * 4;
        i2s_channel_write(tx_handle, chunk, out_bytes, &written, portMAX_DELAY);
        if (written != out_bytes) {
            primed = false;
        }
    }
}

void init_modern_i2s() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 256;   // 256 * 2ch * 2B = 1024 B/desc, 8 KB DMA
    chan_cfg.auto_clear    = false; // don't inject silence on a short stall

    i2s_new_channel(&chan_cfg, &tx_handle, NULL);


    // swap uncomments on the I2S_STD_("X")_DEFAULT_CONFIG to change sound module format
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        //.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = true }
        }
    };
    // If you later hear slow wow vs the phone, try:
    // std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;

    i2s_channel_init_std_mode(tx_handle, &std_cfg);
    i2s_channel_enable(tx_handle);
}

static float ms_to_coef(float ms) {
    float seconds = ms / 1000.0f;
    if (seconds < 0.0001f) seconds = 0.0001f;
    return 1.0f - expf(-1.0f / (SAMPLE_RATE * seconds));
}

void load_settings() {
    prefs.begin("am_proc", false);
    settings.master_gain      = prefs.getFloat("master_gain", 1.0f);
    settings.pos_clip_limit   = prefs.getFloat("pos_clip", 1.25f);
    settings.neg_clip_limit   = prefs.getFloat("neg_clip", -0.95f);
    settings.generator_on     = prefs.getBool("gen_on", false);
    settings.gen_freq         = prefs.getFloat("gen_freq", 400.0f);
    settings.phase_rotator_on = prefs.getBool("rot_on", true);
    settings.tilt_test_on     = prefs.getBool("tilt_on", false);
    settings.tilt_freq        = prefs.getFloat("tilt_freq", 75.0f);
    settings.output_gain      = prefs.getFloat("out_gain", 1.0f);
    settings.mask_selection   = (FilterSelection)prefs.getUChar("mask", MASK_10KHZ);
    settings.hpf_freq         = prefs.getUChar("hpf", 50);
    settings.lr_limit         = prefs.getFloat("lr_limit", 0.75f);
    settings.waveform_type    = prefs.getUChar("wave", 0);
    settings.tone_post_clipper = prefs.getBool("tone_post", false);

    settings.low_comp.threshold      = prefs.getFloat("low_th", 0.3f);
    settings.low_comp.ratio          = prefs.getFloat("low_rt", 4.0f);
    settings.low_comp.attack_coef    = ms_to_coef(prefs.getFloat("low_at", 10.0f));
    settings.low_comp.release_coef   = ms_to_coef(prefs.getFloat("low_re", 100.0f));
    settings.low_comp.gate_threshold = prefs.getFloat("low_gate", 0.01f);

    settings.mid_comp.threshold      = prefs.getFloat("mid_th", 0.3f);
    settings.mid_comp.ratio          = prefs.getFloat("mid_rt", 4.0f);
    settings.mid_comp.attack_coef    = ms_to_coef(prefs.getFloat("mid_at", 10.0f));
    settings.mid_comp.release_coef   = ms_to_coef(prefs.getFloat("mid_re", 100.0f));
    settings.mid_comp.gate_threshold = prefs.getFloat("mid_gate", 0.01f);

    settings.high_comp.threshold      = prefs.getFloat("high_th", 0.3f);
    settings.high_comp.ratio          = prefs.getFloat("high_rt", 4.0f);
    settings.high_comp.attack_coef    = ms_to_coef(prefs.getFloat("high_at", 10.0f));
    settings.high_comp.release_coef   = ms_to_coef(prefs.getFloat("high_re", 100.0f));
    settings.high_comp.gate_threshold = prefs.getFloat("high_gate", 0.01f);

    settings.slow_comp.threshold      = prefs.getFloat("slow_th", 0.25f);
    settings.slow_comp.ratio          = prefs.getFloat("slow_rt", 4.0f);
    settings.slow_comp.attack_coef    = ms_to_coef(prefs.getFloat("slow_at", 400.0f));
    settings.slow_comp.release_coef   = ms_to_coef(prefs.getFloat("slow_re", 2000.0f));
    settings.slow_comp.gate_threshold = prefs.getFloat("slow_gate", 0.005f);

    g_mask_dirty = true;
}

void save_setting(const char *key, float val)   { prefs.putFloat(key, val); }
void save_setting(const char *key, bool val)    { prefs.putBool(key, val); }
void save_setting(const char *key, uint8_t val) { prefs.putUChar(key, val); }

void setup() {
    Serial.begin(115200);
    delay(500);

    reset_lr_limiter();
    rotatorL.init();
    rotatorR.init();


    load_settings();
    crossover.init(360.0f, 3600.0f);
    update_mask_filter(settings.mask_selection);

    init_modern_i2s();

    audio_ring_buffer = xRingbufferCreate(kRingBytes, RINGBUF_TYPE_BYTEBUF);
    if (audio_ring_buffer == NULL) {
        Serial.println("CRITICAL: Failed to create ring buffer!");
        return;
    }

    xTaskCreatePinnedToCore(
        dsp_processing_task, "DSP_Task", 16384, NULL,
        kDspPriority, &dsp_task_handle, 1);

    a2dp_sink.set_stream_reader(audio_data_callback, false);
    a2dp_sink.set_on_connection_state_changed(on_connection_state);
    a2dp_sink.set_on_audio_state_changed(on_audio_state);
    a2dp_sink.start("ESP32_AM_Asym_Proc");

    Serial.println("System modernised and ready.");
}

void loop() {
    if (Serial.available() > 0) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        portENTER_CRITICAL(&g_settings_mux);

        if (cmd.startsWith("GAIN=")) {
            settings.master_gain = cmd.substring(5).toFloat();
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("master_gain", settings.master_gain);
        } else if (cmd.startsWith("PCLIP=")) {
            settings.pos_clip_limit = cmd.substring(6).toFloat();
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("pos_clip", settings.pos_clip_limit);
        } else if (cmd.startsWith("NCLIP=")) {
            settings.neg_clip_limit = -fabs(cmd.substring(6).toFloat());
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("neg_clip", settings.neg_clip_limit);
        } else if (cmd.startsWith("TONE_EN=")) {
            settings.generator_on = (cmd.substring(8).toInt() == 1);
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("gen_on", settings.generator_on);
        } else if (cmd.startsWith("TONE_FREQ=")) {
            settings.gen_freq = cmd.substring(10).toFloat();
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("gen_freq", settings.gen_freq);
        } else if (cmd.startsWith("ROT_EN=")) {
            settings.phase_rotator_on = (cmd.substring(7).toInt() == 1);
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("rot_on", settings.phase_rotator_on);
        } else if (cmd.startsWith("MASK=")) {
            settings.mask_selection = (FilterSelection)cmd.substring(5).toInt();
            g_mask_dirty = true;
            FilterSelection sel = settings.mask_selection;
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("mask", (uint8_t)sel);
        } else if (cmd.startsWith("TILT_EN=")) {
            settings.tilt_test_on = (cmd.substring(8).toInt() == 1);
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("tilt_on", settings.tilt_test_on);
        } else if (cmd.startsWith("TILT_FREQ=")) {
            settings.tilt_freq = cmd.substring(10).toFloat();
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("tilt_freq", settings.tilt_freq);
        } else if (cmd.startsWith("WAVE=")) {
            settings.waveform_type = (uint8_t)cmd.substring(5).toInt();
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("wave", settings.waveform_type);
        } else if (cmd.startsWith("TONE_POST=")) {
            settings.tone_post_clipper = (cmd.substring(10).toInt() == 1);
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("tone_post", settings.tone_post_clipper);
        } else if (cmd.startsWith("OUTGAIN=")) {
            settings.output_gain = cmd.substring(8).toFloat();
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("out_gain", settings.output_gain);
        } else if (cmd.startsWith("HPF=")) {
            settings.hpf_freq = (uint8_t)cmd.substring(4).toInt();
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("hpf", settings.hpf_freq);
        } else if (cmd.startsWith("LR_LIMIT=")) {
            settings.lr_limit = cmd.substring(9).toFloat() / 100.0f;
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("lr_limit", settings.lr_limit);
        } else if (cmd == "STAT") {
            portEXIT_CRITICAL(&g_settings_mux);
            Serial.printf("rx=%u drop=%u waiting=%u reset=%d\n",
                          (unsigned)audio_packets_received,
                          (unsigned)audio_packets_dropped,
                          (unsigned)ring_bytes_waiting(),
                          (int)g_stream_reset);
        } else if (cmd.startsWith("SAVE")) {
            ProcessorSettings s = settings;
            portEXIT_CRITICAL(&g_settings_mux);
            save_setting("master_gain", s.master_gain);
            save_setting("pos_clip", s.pos_clip_limit);
            save_setting("neg_clip", s.neg_clip_limit);
            save_setting("gen_on", s.generator_on);
            save_setting("gen_freq", s.gen_freq);
            save_setting("rot_on", s.phase_rotator_on);
            save_setting("mask", (uint8_t)s.mask_selection);
            save_setting("tilt_on", s.tilt_test_on);
            save_setting("tilt_freq", s.tilt_freq);
            save_setting("out_gain", s.output_gain);
            save_setting("hpf", s.hpf_freq);
            save_setting("lr_limit", s.lr_limit);
            save_setting("wave", s.waveform_type);
            save_setting("tone_post", s.tone_post_clipper);
            save_setting("low_th", s.low_comp.threshold);
            save_setting("low_rt", s.low_comp.ratio);
            save_setting("mid_th", s.mid_comp.threshold);
            save_setting("mid_rt", s.mid_comp.ratio);
            save_setting("high_th", s.high_comp.threshold);
            save_setting("high_rt", s.high_comp.ratio);
        } else if (cmd.startsWith("COMP=")) {
            portEXIT_CRITICAL(&g_settings_mux);
            String data = cmd.substring(5);
            int c[5];
            c[0] = data.indexOf(',');
            bool ok = (c[0] > 0);
            for (int i = 1; i < 5 && ok; i++) {
                c[i] = data.indexOf(',', c[i - 1] + 1);
                if (c[i] < 0) ok = false;
            }
            if (!ok) {
                Serial.println("COMP= needs BAND,th,rt,at,rel,gate");
            } else {
                String band = data.substring(0, c[0]);
                float th   = data.substring(c[0] + 1, c[1]).toFloat();
                float rt   = data.substring(c[1] + 1, c[2]).toFloat();
                float at   = data.substring(c[2] + 1, c[3]).toFloat();
                float rel  = data.substring(c[3] + 1, c[4]).toFloat();
                float gate = data.substring(c[4] + 1).toFloat();
                float ac   = ms_to_coef(at);
                float rc   = ms_to_coef(rel);

                portENTER_CRITICAL(&g_settings_mux);
                DynamicsSettings *target = NULL;
                if (band == "LOW")       target = &settings.low_comp;
                else if (band == "MID")  target = &settings.mid_comp;
                else if (band == "HIGH") target = &settings.high_comp;
                else if (band == "SLOW") target = &settings.slow_comp;
                if (target) {
                    target->threshold = th;
                    target->ratio = rt;
                    target->attack_coef = ac;
                    target->release_coef = rc;
                    target->gate_threshold = gate;
                }
                portEXIT_CRITICAL(&g_settings_mux);

                const char *prefix = (band == "LOW") ? "low" :
                                     (band == "MID") ? "mid" :
                                     (band == "HIGH") ? "high" :
                                     (band == "SLOW") ? "slow" : NULL;
                if (prefix) {
                    save_setting((String(prefix) + "_th").c_str(), th);
                    save_setting((String(prefix) + "_rt").c_str(), rt);
                    save_setting((String(prefix) + "_at").c_str(), at);
                    save_setting((String(prefix) + "_re").c_str(), rel);
                    save_setting((String(prefix) + "_gate").c_str(), gate);
                }
            }
        } else {
            portEXIT_CRITICAL(&g_settings_mux);
        }
    }
    delay(10);
}
