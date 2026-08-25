#include "BluetoothA2DPSink.h"
#include "driver/i2s_std.h"
#include <math.h>
#include "freertos/ringbuf.h"
#include <Preferences.h>

#define SAMPLE_RATE     44100

// PCM5102A I2S pins (no internal DAC)
#define I2S_BCK_IO      (GPIO_NUM_26)  
#define I2S_WS_IO       (GPIO_NUM_25)  
#define I2S_DO_IO       (GPIO_NUM_22)  
//#define I2S_MCK_IO      (GPIO_NUM_33) (Not used - SCK tied to GND)

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

    uint8_t waveform_type = 0;       // 0=Sine, 1=Square, 2=Sawtooth
    bool tone_post_clipper = false;
    uint8_t hpf_freq = 50;           // 50-100 Hz ACMA HPF
    float lr_limit = 0.75f;          // CQUAM L-R limit (50-90%)
};

// --- Global System Instances ---
volatile ProcessorSettings settings;
Preferences prefs;
BluetoothA2DPSink a2dp_sink;
float tone_phase = 0.0f;

// Modern v3 framework handle for your I2S audio output
i2s_chan_handle_t tx_handle = NULL; 

// === PASTE THESE HERE IF THEY ARE MISSING ===
RingbufHandle_t audio_ring_buffer = NULL;
TaskHandle_t dsp_task_handle = NULL;

volatile uint32_t audio_packets_received = 0;
volatile uint32_t audio_packets_dropped = 0;


// --- Phase Rotator (All-Pass Filters at ~200Hz) ---
struct AllPassFilter {
    float a = -0.6f; 
    float x1 = 0.0f, y1 = 0.0f;
    inline float process(float in) {
        float out = a * in + x1 - a * y1;
        x1 = in; y1 = out;
        return out;
    }
};

struct PhaseRotator {
    AllPassFilter stage1, stage2, stage3, stage4; 
    inline float process(float in) {
        float out = stage1.process(in);
        out = stage2.process(out);
        out = stage3.process(out);
        out = stage4.process(out);
        return out;
    }
};

// Band Compressor
struct BandCompressor {
    float env = 0.0f;
    inline void process(float &signal, const volatile DynamicsSettings &cfg) {
        float abs_sig = fabs(signal);
        float current_attack_coef  = cfg.attack_coef;
        float current_release_coef = cfg.release_coef;
        float current_threshold    = cfg.threshold;
        float current_ratio        = cfg.ratio;
        float current_gate         = cfg.gate_threshold;
        
        if (abs_sig > env) env += current_attack_coef * (abs_sig - env);
        else               env += current_release_coef * (abs_sig - env);

        if (env < current_gate) return;

        if (env > current_threshold && env > 0.0001f) {
            float target_env = current_threshold + (env - current_threshold) / current_ratio;
            float gain = target_env / env;
            signal *= gain;
        }
    }
};

// === Look-ahead L-R Stereo Limiter (CQUAM) ===
#define LR_LOOKAHEAD_SAMPLES  441
float lr_delay_buffer[LR_LOOKAHEAD_SAMPLES][2];
int lr_write_index = 0;
float lr_env = 0.0f;

BandCompressor compL_low, compL_mid, compL_high;
BandCompressor compR_low, compR_mid, compR_high;
BandCompressor slowAGC_L, slowAGC_R;

PhaseRotator rotatorL, rotatorR;

// Biquad
struct Biquad {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
    void reset() { x1 = x2 = y1 = y2 = 0; }
    void setLPF(float frequency, float q) {
        float omega = 2.0f * M_PI * frequency / SAMPLE_RATE;
        float alpha = sin(omega) / (2.0f * q);
        float cosw = cos(omega);
        float b0_raw = (1.0f - cosw) / 2.0f; float a0 = 1.0f + alpha;
        b0 = b0_raw / a0; b1 = (1.0f - cosw) / a0; b2 = b0_raw / a0;
        a1 = (-2.0f * cosw) / a0; a2 = (1.0f - alpha) / a0;
    }
    void setHPF(float frequency, float q) {
        float omega = 2.0f * M_PI * frequency / SAMPLE_RATE;
        float alpha = sin(omega) / (2.0f * q);
        float cosw = cos(omega);
        float b0_raw = (1.0f + cosw) / 2.0f; float a0 = 1.0f + alpha;
        b0 = b0_raw / a0; b1 = -(1.0f + cosw) / a0; b2 = b0_raw / a0;
        a1 = (-2.0f * cosw) / a0; a2 = (1.0f - alpha) / a0;
    }
    inline float process(float in) {
        float out = b0*in + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2 = x1; x1 = in; y2 = y1; y1 = out;
        return out;
    }
};

struct LRCrossover3Band {
    Biquad lp1_L, lp2_L, hp1_L, hp2_L, lp3_L, lp4_L, hp3_L, hp4_L;
    Biquad lp1_R, lp2_R, hp1_R, hp2_R, lp3_R, lp4_R, hp3_R, hp4_R;
    void init(float low_mid_freq, float mid_high_freq) {
        float q = 0.707f;
        lp1_L.setLPF(low_mid_freq, q); lp2_L.setLPF(low_mid_freq, q); hp1_L.setHPF(low_mid_freq, q); hp2_L.setHPF(low_mid_freq, q);
        lp3_L.setLPF(mid_high_freq, q); lp4_L.setLPF(mid_high_freq, q); hp3_L.setHPF(mid_high_freq, q); hp4_L.setHPF(mid_high_freq, q);
        lp1_R.setLPF(low_mid_freq, q); lp2_R.setLPF(low_mid_freq, q); hp1_R.setHPF(low_mid_freq, q); hp2_R.setHPF(low_mid_freq, q);
        lp3_R.setLPF(mid_high_freq, q); lp4_R.setLPF(mid_high_freq, q); hp3_R.setHPF(mid_high_freq, q); hp4_R.setHPF(mid_high_freq, q);
    }
};

LRCrossover3Band crossover;
Biquad maskFilterL1, maskFilterL2, maskFilterR1, maskFilterR2;

// Test waveform generator (injected just before final clipper)
inline float generate_test_waveform(float phase, uint8_t type) {
    float t = phase / (2.0f * M_PI); // Normalized time: 0.0 to 1.0

    if (type == 0) { // Sine
        return sin(phase);
    } 
    else if (type == 1) { // Square Wave (Tilt Test)
        return (t < 0.50f) ? 1.0f : -1.0f; 
    } 
    else if (type == 2) { // Modified Saw (Falling) with 5% flat hold at +100%
        // Cycle breakdown: 
        // 0.00 -> 0.05 (5%): Flat hold at exactly +1.00 (100%)
        // 0.05 -> 0.10 (5%): Smooth rise from +1.00 up to +1.25 (Peak test)
        // 0.10 -> 1.00 (90%): Linear fall from +1.25 down to -1.00
        if (t < 0.05f) {
            return 1.0f; // 5% Flat plateau at 100% modulation
        } else if (t < 0.10f) {
            float ramp = (t - 0.05f) / 0.05f; 
            return 1.0f + (ramp * 0.25f); // Rises cleanly to 1.25
        } else {
            float ramp = (t - 0.10f) / 0.90f;
            return 1.25f - (ramp * 2.25f); // Falls linearly from +1.25 to -1.00
        }
    } 
    else { // Modified Saw (Rising) with 5% flat hold at +100%
        // Cycle breakdown:
        // 0.00 -> 0.90 (90%): Linear rise from -1.00 up to +1.00
        // 0.90 -> 0.95 (5%): Flat hold at exactly +1.00 (100%)
        // 0.95 -> 1.00 (5%): Final peak punch from +1.00 up to +1.25
        if (t < 0.90f) {
            float ramp = t / 0.90f;
            return -1.0f + (ramp * 2.0f); // Rises from -1.00 to +1.00
        } else if (t < 0.95f) {
            return 1.0f; // 5% Flat plateau at 100% modulation
        } else {
            float ramp = (t - 0.95f) / 0.05f;
            return 1.0f + (ramp * 0.25f); // Final 25% peak punch up to 1.25
        }
    }
}


void update_mask_filter(FilterSelection selection) {
    float freq = 10000.0f;
    if (selection == MASK_5KHZ) freq = 5000.0f;
    else if (selection == MASK_9KHZ) freq = 9000.0f;
    else if (selection == MASK_10KHZ) freq = 10000.0f;
    else if (selection == MASK_12KHZ) freq = 12000.0f;
    else if (selection == MASK_15KHZ) freq = 15000.0f;

    maskFilterL1.setLPF(freq, 0.707f); maskFilterL2.setLPF(freq, 0.707f);
    maskFilterR1.setLPF(freq, 0.707f); maskFilterR2.setLPF(freq, 0.707f);
}

void audio_data_callback(const uint8_t *data, uint32_t length) {
    if (audio_ring_buffer != NULL) {
        BaseType_t status = xRingbufferSend(audio_ring_buffer, (void*)data, length, 0);
        if (status == pdTRUE) {
            // Safe replacement for audio_packets_received++;
            __atomic_fetch_add(&audio_packets_received, 1, __ATOMIC_SEQ_CST);
        } else {
            // Safe replacement for audio_packets_dropped++;
            __atomic_fetch_add(&audio_packets_dropped, 1, __ATOMIC_SEQ_CST);
        }
    }
}



void dsp_processing_task(void *pvParameters) {
    size_t item_size;
    
    // Move filter configurations completely outside the active while(1) loop 
    // to save massive stack space!
    Biquad hpfL, hpfR;
    uint8_t last_hpf = 0;
    hpfL.setHPF(settings.hpf_freq, 0.707f);
    hpfR.setHPF(settings.hpf_freq, 0.707f);

    while (1) {
        uint8_t *buffer = (uint8_t *)xRingbufferReceive(audio_ring_buffer, &item_size, portMAX_DELAY);
        if (buffer != NULL) {
            int16_t *samples = (int16_t*) buffer;
            uint32_t sample_count = item_size / 4; 

            for (uint32_t i = 0; i < sample_count; i++) {
                float left = 0.0f;
                float right = 0.0f;

                if (!settings.generator_on) {
                    left = samples[i * 2] / 32768.0f; 
                    right = samples[i * 2 + 1] / 32768.0f;
                }

                left *= settings.master_gain; 
                right *= settings.master_gain;

                if (settings.phase_rotator_on && !settings.generator_on) {
                    left = rotatorL.process(left); 
                    right = rotatorR.process(right);
                }

                // Cleaned HPF check
                if (settings.hpf_freq != last_hpf) {
                    float f = (settings.hpf_freq < 50) ? 50 : ((settings.hpf_freq > 100) ? 100 : settings.hpf_freq);
                    hpfL.setHPF(f, 0.707f);
                    hpfR.setHPF(f, 0.707f);
                    last_hpf = settings.hpf_freq;
                }
                left  = hpfL.process(left);
                right = hpfR.process(right);

                // Slow AGC
                slowAGC_L.process(left, settings.slow_comp);
                slowAGC_R.process(right, settings.slow_comp);

                // 3-band crossover + compression
                float low_L = crossover.lp2_L.process(crossover.lp1_L.process(left));
                float high_L = crossover.hp2_L.process(crossover.hp1_L.process(left));
                float mid_L = crossover.lp4_L.process(crossover.lp3_L.process(high_L));
                high_L = crossover.hp4_L.process(crossover.hp3_L.process(high_L));

                float low_R = crossover.lp2_R.process(crossover.lp1_R.process(right));
                float high_R = crossover.hp2_R.process(crossover.hp1_R.process(right));
                float mid_R = crossover.lp4_R.process(crossover.lp3_R.process(high_R));
                high_R = crossover.hp4_R.process(crossover.hp3_R.process(high_R));

                compL_low.process(low_L, settings.low_comp);
                compL_mid.process(mid_L, settings.mid_comp);
                compL_high.process(high_L, settings.high_comp);
                compR_low.process(low_R, settings.low_comp);
                compR_mid.process(mid_R, settings.mid_comp);
                compR_high.process(high_R, settings.high_comp);

                // 1. Combine multiband compressor outputs
                left = low_L + mid_L + high_L;
                right = low_R + mid_R + high_R;

                // 2. Pre-mask Soft Clipper (keeps things inside spectral bounds)
                left = (left > settings.pos_clip_limit) ? settings.pos_clip_limit : ((left < settings.neg_clip_limit) ? settings.neg_clip_limit : left);
                right = (right > settings.pos_clip_limit) ? settings.pos_clip_limit : ((right < settings.neg_clip_limit) ? settings.neg_clip_limit : right);

                // 3. Sharp NRSC Spectral Mask Filter
                left = maskFilterL2.process(maskFilterL1.process(left));
                right = maskFilterR2.process(maskFilterR1.process(right));

                // ========================================================
                // 4. Look-ahead L-R Stereo Limiter & Phase Protection (FIXED)
                // ========================================================
                
                // 1. Read the delayed samples that have been waiting in the look-ahead window
                float delayed_L = lr_delay_buffer[lr_write_index][0];
                float delayed_R = lr_delay_buffer[lr_write_index][1];
              
                // 2. Put the CURRENT raw incoming samples into the delay line
                lr_delay_buffer[lr_write_index][0] = left;
                lr_delay_buffer[lr_write_index][1] = right;
                
                // 3. Move index safely
                lr_write_index++;
                if (lr_write_index >= LR_LOOKAHEAD_SAMPLES) {
                   lr_write_index = 0;
                }
              
                // 4. Look-ahead Peak Envelope detection using CURRENT samples
                float current_L_plus_R = (left + right) * 0.5f;
                float current_L_minus_R = (left - right) * 0.5f;
                float abs_lr_diff = fabs(current_L_minus_R);
              
                // Instant peak capture for look-ahead action
                if (abs_lr_diff > lr_env) {
                    lr_env = abs_lr_diff; 
                } else {
                    // Fast decay tailored for high-speed transients
                    lr_env += 0.002f * (abs_lr_diff - lr_env); 
                }
                 
                // 5. Calculate gain reduction based on user limits
                float limit = settings.lr_limit;
                float knee_start = limit * 0.80f; // Slightly wider knee for smoother tracking
                float gain_reduction = 1.0f;
              
                if (lr_env > knee_start) {
                    if (lr_env > limit) {
                        gain_reduction = limit / lr_env;
                    } else {
                        float over = (lr_env - knee_start) / (limit - knee_start);
                        gain_reduction = 1.0f - (over * over * 0.20f);
                    }
                }
              
                // 6. Apply the tracked gain reduction to the DELAYED samples
                left = delayed_L * gain_reduction;
                right = delayed_R * gain_reduction;
              
                // 🎯 7. SAFETY NET: Instantaneous Hard Phase-Angle Clipper
                // This prevents carrier cancellation even if a transient breaks past the envelope follower.
                // 🎯 FINAL TWEAK: Cross-Matrix Carrier Protection for Mono DSP Radios
                float post_L_plus_R = (left + right) * 0.5f;
                float post_L_minus_R = (left - right) * 0.5f;
                
                // Calculate the absolute minimum carrier threshold allowed.
                // If |L-R| approaches or exceeds |L+R|, the carrier dips to zero, causing the tinny sound.
                // We ensure L-R never exceeds 85% of the current mono carrier envelope.
                float safe_lr_ceiling = fabs(post_L_plus_R) * 0.85f;
                
                // Also respect your user-defined hard limit from the UI settings
                if (safe_lr_ceiling > settings.lr_limit) {
                    safe_lr_ceiling = settings.lr_limit;
                }
                
                // Absolute lower bound fallback to keep things stable during silence or low passages
                if (safe_lr_ceiling < 0.10f) {
                    safe_lr_ceiling = 0.10f; 
                }
                
                // If the difference signal is too hot for the current carrier level, scale it down smoothly
                if (fabs(post_L_minus_R) > safe_lr_ceiling) {
                    float cross_matrix_reduction = safe_lr_ceiling / fabs(post_L_minus_R);
                    post_L_minus_R *= cross_matrix_reduction;
                    
                    // Reconstruct the safe Left and Right channels
                    left = post_L_plus_R + post_L_minus_R;
                    right = post_L_plus_R - post_L_minus_R;
                }




                // ========================================================
                // 5. 🎯 TARGET INJECTION POINT: Calibration Oscillator
                // ========================================================
                // Injected AFTER L-R limiting but BEFORE the final Asymmetric Clipper
                if (settings.generator_on || settings.tilt_test_on) {
                    float freq = settings.generator_on ? settings.gen_freq : settings.tilt_freq;
                    uint8_t wtype = settings.generator_on ? settings.waveform_type : 1; // 1 = Square for tilt test
                    
                    float val = generate_test_waveform(tone_phase, wtype);
                    left = val; 
                    right = val; // True mono injection to test carrier balancing
                    
                    tone_phase += (2.0f * M_PI * freq) / SAMPLE_RATE;
                    if (tone_phase >= 2.0f * M_PI) tone_phase -= 2.0f * M_PI;
                }

                // Apply final output makeup gain
                left *= settings.output_gain;
                right *= settings.output_gain;


                // ========================================================
                // 6. 🔒 BRICK-WALL ASYMMETRIC OUTPUT CLIPPER
                // ========================================================
                // This protects your AM transmitter from overmodulating positive peaks (+125%) 
                // and clipping negative carrier peaks completely (-95%).
                if (left > settings.pos_clip_limit)       left = settings.pos_clip_limit;
                else if (left < settings.neg_clip_limit)  left = settings.neg_clip_limit;

                if (right > settings.pos_clip_limit)      right = settings.pos_clip_limit;
                else if (right < settings.neg_clip_limit) right = settings.neg_clip_limit;


                // 7. Safe conversion back to 16-bit PCM Space for DAC output
                // (Removed the secondary trailing tone generator that overrode everything!)
                samples[i * 2]     = (int16_t)(left * 24000.0f); 
                samples[i * 2 + 1] = (int16_t)(right * 24000.0f);
            }

            size_t bytes_written;
            // Using modern explicit casting for ring buffer blocks
            i2s_channel_write(tx_handle, (const void *)buffer, item_size, &bytes_written, pdMS_TO_TICKS(20));
            
            // Release memory back to the ring buffer immediately so A2DP can write more data!
            vRingbufferReturnItem(audio_ring_buffer, (void *)buffer);
        }
        
        // Essential yield step to allow Bluetooth background threads to catch up
        vTaskDelay(1); 
    }
}


void init_modern_i2s() {
    // 1. Configure the modern I2S Controller slot
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Prevents noise burst artifacts
    
    // Allocate the channel
    i2s_new_channel(&chan_cfg, &tx_handle, NULL);

    // 2. Configure the clock and sample widths
        i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,      // <-- FIXED: Must be .bclk, not .bck
            .ws = I2S_WS_IO,         // <-- Keep this as .ws
            .dout = I2S_DO_IO,       // <-- Keep this as .dout
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };

    
    // Initialize and enable the physical peripheral
    i2s_channel_init_std_mode(tx_handle, &std_cfg);
    i2s_channel_enable(tx_handle);
}

void load_settings() {
    prefs.begin("am_proc", false);
    settings.master_gain       = prefs.getFloat("master_gain", 1.0f);
    settings.pos_clip_limit    = prefs.getFloat("pos_clip", 1.25f);
    settings.neg_clip_limit    = prefs.getFloat("neg_clip", -0.95f);
    settings.generator_on      = prefs.getBool("gen_on", false);
    settings.gen_freq          = prefs.getFloat("gen_freq", 400.0f);
    settings.phase_rotator_on  = prefs.getBool("rot_on", true);
    settings.tilt_test_on      = prefs.getBool("tilt_on", false);
    settings.tilt_freq         = prefs.getFloat("tilt_freq", 75.0f);
    settings.output_gain       = prefs.getFloat("out_gain", 1.0f);
    settings.mask_selection    = (FilterSelection)prefs.getUChar("mask", MASK_10KHZ);
    settings.hpf_freq          = prefs.getUChar("hpf", 50);
    settings.lr_limit          = prefs.getFloat("lr_limit", 0.75f);

    settings.low_comp.threshold  = prefs.getFloat("low_th", 0.3f);
    settings.low_comp.ratio      = prefs.getFloat("low_rt", 4.0f);
    settings.low_comp.attack_coef  = 1.0f - exp(-1.0f / (SAMPLE_RATE * (prefs.getFloat("low_at", 10.0f) / 1000.0f)));
    settings.low_comp.release_coef = 1.0f - exp(-1.0f / (SAMPLE_RATE * (prefs.getFloat("low_re", 100.0f) / 1000.0f)));
    settings.low_comp.gate_threshold = prefs.getFloat("low_gate", 0.01f);

    settings.mid_comp.threshold  = prefs.getFloat("mid_th", 0.3f);
    settings.mid_comp.ratio      = prefs.getFloat("mid_rt", 4.0f);
    settings.mid_comp.attack_coef  = 1.0f - exp(-1.0f / (SAMPLE_RATE * (prefs.getFloat("mid_at", 10.0f) / 1000.0f)));
    settings.mid_comp.release_coef = 1.0f - exp(-1.0f / (SAMPLE_RATE * (prefs.getFloat("mid_re", 100.0f) / 1000.0f)));
    settings.mid_comp.gate_threshold = prefs.getFloat("mid_gate", 0.01f);

    settings.high_comp.threshold  = prefs.getFloat("high_th", 0.3f);
    settings.high_comp.ratio      = prefs.getFloat("high_rt", 4.0f);
    settings.high_comp.attack_coef  = 1.0f - exp(-1.0f / (SAMPLE_RATE * (prefs.getFloat("high_at", 10.0f) / 1000.0f)));
    settings.high_comp.release_coef = 1.0f - exp(-1.0f / (SAMPLE_RATE * (prefs.getFloat("high_re", 100.0f) / 1000.0f)));
    settings.high_comp.gate_threshold = prefs.getFloat("high_gate", 0.01f);

    settings.slow_comp.threshold  = prefs.getFloat("slow_th", 0.25f);
    settings.slow_comp.ratio      = prefs.getFloat("slow_rt", 4.0f);
    settings.slow_comp.attack_coef  = 1.0f - exp(-1.0f / (SAMPLE_RATE * (prefs.getFloat("slow_at", 400.0f) / 1000.0f)));
    settings.slow_comp.release_coef = 1.0f - exp(-1.0f / (SAMPLE_RATE * (prefs.getFloat("slow_re", 2000.0f) / 1000.0f)));
    settings.slow_comp.gate_threshold = prefs.getFloat("slow_gate", 0.005f);
}

void save_setting(const char* key, float val) { prefs.putFloat(key, val); }
void save_setting(const char* key, bool val)   { prefs.putBool(key, val); }
void save_setting(const char* key, uint8_t val){ prefs.putUChar(key, val); }

void setup() {
    Serial.begin(115200);
    delay(500); 
    
    load_settings();
    crossover.init(180.0f, 3200.0f);
    update_mask_filter(settings.mask_selection);
    
    // Initialize the new, warning-free modern I2S hardware channel
    init_modern_i2s();
    
    // Allow-split buffer allocation handles incoming Bluetooth chunks flawlessly
    audio_ring_buffer = xRingbufferCreate(16384, RINGBUF_TYPE_BYTEBUF);
    if (audio_ring_buffer == NULL) {
        Serial.println("CRITICAL: Failed to create ring buffer!");
    }
    
    // Spin up your background processing pipeline thread on Core 1
    xTaskCreatePinnedToCore(dsp_processing_task, "DSP_Task", 16384, NULL, 3, &dsp_task_handle, 1);
    
    // Tell A2DP to capture audio but keep its hands completely off the I2S hardware (false)
    a2dp_sink.set_stream_reader(audio_data_callback, false); 
    a2dp_sink.start("ESP32_AM_Asym_Proc");
    
    Serial.println("System modernised and ready.");
}


void loop() {
    if (Serial.available() > 0) {
        String cmd = Serial.readStringUntil('\n'); cmd.trim();
        
        if (cmd.startsWith("GAIN=")) { settings.master_gain = cmd.substring(5).toFloat(); save_setting("master_gain", settings.master_gain); }
        else if (cmd.startsWith("PCLIP=")) { settings.pos_clip_limit = cmd.substring(6).toFloat(); save_setting("pos_clip", settings.pos_clip_limit); }
        else if (cmd.startsWith("NCLIP=")) { settings.neg_clip_limit = -fabs(cmd.substring(6).toFloat()); save_setting("neg_clip", settings.neg_clip_limit); }
        else if (cmd.startsWith("TONE_EN=")) { settings.generator_on = (cmd.substring(8).toInt() == 1); save_setting("gen_on", settings.generator_on); }
        else if (cmd.startsWith("TONE_FREQ=")) { settings.gen_freq = cmd.substring(10).toFloat(); save_setting("gen_freq", settings.gen_freq); }
        else if (cmd.startsWith("ROT_EN=")) { settings.phase_rotator_on = (cmd.substring(7).toInt() == 1); save_setting("rot_on", settings.phase_rotator_on); }
        else if (cmd.startsWith("MASK=")) {
            settings.mask_selection = (FilterSelection)cmd.substring(5).toInt();
            update_mask_filter(settings.mask_selection);
            save_setting("mask", (uint8_t)settings.mask_selection);
        }
        else if (cmd.startsWith("TILT_EN=")) settings.tilt_test_on = (cmd.substring(8).toInt() == 1);
        else if (cmd.startsWith("TILT_FREQ=")) settings.tilt_freq = cmd.substring(10).toFloat();
        else if (cmd.startsWith("WAVE=")) settings.waveform_type = cmd.substring(5).toInt();
        else if (cmd.startsWith("TONE_POST=")) settings.tone_post_clipper = (cmd.substring(10).toInt() == 1);
        else if (cmd.startsWith("OUTGAIN=")) { settings.output_gain = cmd.substring(8).toFloat(); save_setting("out_gain", settings.output_gain); }
        else if (cmd.startsWith("HPF=")) { settings.hpf_freq = (uint8_t)cmd.substring(4).toInt(); }
        else if (cmd.startsWith("LR_LIMIT=")) { settings.lr_limit = cmd.substring(9).toFloat() / 100.0f; }
        else if (cmd.startsWith("SAVE")) {
            save_setting("master_gain", settings.master_gain);
            save_setting("pos_clip", settings.pos_clip_limit);
            save_setting("neg_clip", settings.neg_clip_limit);
            save_setting("gen_on", settings.generator_on);
            save_setting("gen_freq", settings.gen_freq);
            save_setting("rot_on", settings.phase_rotator_on);
            save_setting("mask", (uint8_t)settings.mask_selection);
            save_setting("tilt_on", settings.tilt_test_on);
            save_setting("tilt_freq", settings.tilt_freq);
            save_setting("out_gain", settings.output_gain);
            save_setting("hpf", settings.hpf_freq);
            save_setting("lr_limit", settings.lr_limit);
            save_setting("low_th", settings.low_comp.threshold); save_setting("low_rt", settings.low_comp.ratio);
            save_setting("mid_th", settings.mid_comp.threshold); save_setting("mid_rt", settings.mid_comp.ratio);
            save_setting("high_th", settings.high_comp.threshold); save_setting("high_rt", settings.high_comp.ratio);
        }
        else if (cmd.startsWith("COMP=")) {
            String data = cmd.substring(5);
            int c[5];
            c[0] = data.indexOf(',');
            for (int i=1; i<5; i++) c[i] = data.indexOf(',', c[i-1]+1);
            
            String band = data.substring(0, c[0]);
            float th   = data.substring(c[0]+1, c[1]).toFloat();
            float rt   = data.substring(c[1]+1, c[2]).toFloat();
            float at   = data.substring(c[2]+1, c[3]).toFloat();
            float rel  = data.substring(c[3]+1, c[4]).toFloat();
            float gate = data.substring(c[4]+1).toFloat();
            
            float calculated_attack_coef  = 1.0f - exp(-1.0f / (SAMPLE_RATE * (at / 1000.0f)));
            float calculated_release_coef = 1.0f - exp(-1.0f / (SAMPLE_RATE * (rel / 1000.0f)));
            
                        volatile DynamicsSettings* targetBand = NULL;
            if (band == "LOW")       targetBand = &settings.low_comp;
            else if (band == "MID")  targetBand = &settings.mid_comp;
            else if (band == "HIGH") targetBand = &settings.high_comp;
            else if (band == "SLOW") targetBand = &settings.slow_comp;
            
            // FIX: Write directly to volatile fields safely without dangerous pointer casting!
            if (targetBand != NULL) {
                targetBand->threshold      = th;
                targetBand->ratio          = rt;
                targetBand->attack_coef    = calculated_attack_coef;
                targetBand->release_coef   = calculated_release_coef;
                targetBand->gate_threshold = gate;
            }

            const char* prefix = (band == "LOW") ? "low" : (band == "MID") ? "mid" : (band == "HIGH") ? "high" : "slow";
            if (band == "LOW" || band == "MID" || band == "HIGH" || band == "SLOW") {
                save_setting((String(prefix)+"_th").c_str(), th);
                save_setting((String(prefix)+"_rt").c_str(), rt);
                save_setting((String(prefix)+"_at").c_str(), at);
                save_setting((String(prefix)+"_re").c_str(), rel);
                save_setting((String(prefix)+"_gate").c_str(), gate);
            }
        }
    }
    delay(10);
}
