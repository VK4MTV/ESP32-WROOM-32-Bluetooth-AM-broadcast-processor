#include "BluetoothA2DPSink.h"
#include "driver/i2s.h" 
#include <math.h>
#include "freertos/ringbuf.h"

#define SAMPLE_RATE     44100

#define I2S_BCK_IO      (GPIO_NUM_26)  
#define I2S_WS_IO       (GPIO_NUM_25)  
#define I2S_DO_IO       (GPIO_NUM_22)  

enum FilterSelection { MASK_5KHZ = 0, MASK_9KHZ, MASK_10KHZ, MASK_12KHZ, MASK_15KHZ };

struct DynamicsSettings {
    float threshold = 0.3f; 
    float ratio = 4.0f;     
    float attack_coef = 0.0022f;  // Pre-calculated for 10ms default
    float release_coef = 0.00022f; // Pre-calculated for 100ms default
};

struct ProcessorSettings {
    float master_gain = 1.0f;
    DynamicsSettings low_comp;
    DynamicsSettings mid_comp;
    DynamicsSettings high_comp;
    
    float pos_clip_limit = 1.25f;  
    float neg_clip_limit = -0.95f; 
    
    FilterSelection mask_selection = MASK_10KHZ;
    bool generator_on = false;
    float gen_freq = 400.0f;
    bool phase_rotator_on = true; 
};

volatile ProcessorSettings settings;
BluetoothA2DPSink a2dp_sink;
float tone_phase = 0.0f;
RingbufHandle_t audio_ring_buffer = NULL;
TaskHandle_t dsp_task_handle = NULL;

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
PhaseRotator rotatorL, rotatorR;

// --- Optimized Multi-band Compressor (Zero Log/Pow Math) ---
// --- Fixed Multi-band Compressor Envelope Tracker Class ---
struct BandCompressor {
    float env = 0.0f;

    inline void process(float &signal, const volatile DynamicsSettings &cfg) {
        float abs_sig = fabs(signal);
        
        // Force the hardware to read the live updated memory addresses explicitly
        float current_attack_coef  = *(float*)&(cfg.attack_coef);
        float current_release_coef = *(float*)&(cfg.release_coef);
        float current_threshold    = *(float*)&(cfg.threshold);
        float current_ratio        = *(float*)&(cfg.ratio);
        
        if (abs_sig > env) {
            env += current_attack_coef * (abs_sig - env);
        } else {
            env += current_release_coef * (abs_sig - env);
        }

        if (env > current_threshold && env > 0.0001f) {
            float target_env = current_threshold + (env - current_threshold) / current_ratio;
            float gain = target_env / env;
            signal *= gain;
        }
    }
};


BandCompressor compL_low, compL_mid, compL_high;
BandCompressor compR_low, compR_mid, compR_high;

// --- Biquad & Crossover Infrastructure ---
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
        xRingbufferSend(audio_ring_buffer, data, length, 0);
    }
}

void dsp_processing_task(void *pvParameters) {
    size_t item_size;
    while (1) {
        uint8_t *buffer = (uint8_t *)xRingbufferReceive(audio_ring_buffer, &item_size, portMAX_DELAY);
        if (buffer != NULL) {
            int16_t *samples = (int16_t*) buffer;
            uint32_t sample_count = item_size / 4; 

            for (uint32_t i = 0; i < sample_count; i++) {
                float left, right;
                if (settings.generator_on) {
                    float val = sin(tone_phase); left = val; right = val;
                    tone_phase += (2.0f * M_PI * settings.gen_freq) / SAMPLE_RATE;
                    if (tone_phase >= 2.0f * M_PI) tone_phase -= 2.0f * M_PI;
                } else {
                    left = samples[i * 2] / 32768.0f; right = samples[i * 2 + 1] / 32768.0f;
                }

                left *= settings.master_gain; right *= settings.master_gain;

                if (settings.phase_rotator_on && !settings.generator_on) {
                    left = rotatorL.process(left); right = rotatorR.process(right);
                }

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

                left = low_L + mid_L + high_L; right = low_R + mid_R + high_R;

                if (left > settings.pos_clip_limit) left = settings.pos_clip_limit;
                if (left < settings.neg_clip_limit) left = settings.neg_clip_limit;
                if (right > settings.pos_clip_limit) right = settings.pos_clip_limit;
                if (right < settings.neg_clip_limit) right = settings.neg_clip_limit;

                left = maskFilterL2.process(maskFilterL1.process(left));
                right = maskFilterR2.process(maskFilterR1.process(right));

                samples[i * 2] = (int16_t)(left * 24000.0f); 
                samples[i * 2 + 1] = (int16_t)(right * 24000.0f);
            }

            size_t bytes_written;
            i2s_write(I2S_NUM_0, buffer, item_size, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(audio_ring_buffer, (void *)buffer);
        }
        vTaskDelay(pdMS_TO_TICKS(1)); 
    }
}

void init_i2s() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,  
        .use_apll = false,
        .tx_desc_auto_clear = true
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_IO, .ws_io_num = I2S_WS_IO, .data_out_num = I2S_DO_IO, .data_in_num = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
}

void setup() {
    Serial.begin(115200);
    crossover.init(180.0f, 3200.0f); // Balanced broadcast crossover frequencies
    update_mask_filter(settings.mask_selection);
    init_i2s();
    
    audio_ring_buffer = xRingbufferCreate(16384, RINGBUF_TYPE_NOSPLIT);
    xTaskCreatePinnedToCore(dsp_processing_task, "DSP_Task", 8192, NULL, 5, &dsp_task_handle, 1);
    
    a2dp_sink.set_stream_reader(audio_data_callback, false); 
    a2dp_sink.start("ESP32_AM_Asym_Proc");
}

void loop() {
    if (Serial.available() > 0) {
        String cmd = Serial.readStringUntil('\n'); cmd.trim();
        if (cmd.startsWith("GAIN=")) settings.master_gain = cmd.substring(5).toFloat();
        else if (cmd.startsWith("PCLIP=")) settings.pos_clip_limit = cmd.substring(6).toFloat();
        else if (cmd.startsWith("NCLIP=")) settings.neg_clip_limit = -fabs(cmd.substring(6).toFloat());
        else if (cmd.startsWith("TONE_EN=")) settings.generator_on = (cmd.substring(8).toInt() == 1);
        else if (cmd.startsWith("TONE_FREQ=")) settings.gen_freq = cmd.substring(10).toFloat();
        else if (cmd.startsWith("ROT_EN=")) settings.phase_rotator_on = (cmd.substring(7).toInt() == 1);
        else if (cmd.startsWith("MASK=")) {
            settings.mask_selection = (FilterSelection)cmd.substring(5).toInt();
            update_mask_filter(settings.mask_selection);
        }
        else if (cmd.startsWith("COMP=")) {
            String data = cmd.substring(5);
            int idx1 = data.indexOf(','); int idx2 = data.indexOf(',', idx1+1);
            int idx3 = data.indexOf(',', idx2+1); int idx4 = data.indexOf(',', idx3+1);
            
            String band = data.substring(0, idx1);
            float th = data.substring(idx1+1, idx2).toFloat();
            float rt = data.substring(idx2+1, idx3).toFloat();
            float at = data.substring(idx3+1, idx4).toFloat();
            float rel = data.substring(idx4+1).toFloat();

            // Core 0 calculates transcendental time coefficients natively during slider updates
            float a_coef = 1.0f - exp(-1.0f / (SAMPLE_RATE * (at / 1000.0f)));
            float r_coef = 1.0f - exp(-1.0f / (SAMPLE_RATE * (rel / 1000.0f)));
            
            volatile DynamicsSettings* targetBand = NULL;
            if (band == "LOW")       targetBand = &settings.low_comp;
            else if (band == "MID")  targetBand = &settings.mid_comp;
            else if (band == "HIGH") targetBand = &settings.high_comp;
            
            if (targetBand != NULL) {
                *(float*)&(targetBand->threshold) = th;
                *(float*)&(targetBand->ratio) = rt;
                *(float*)&(targetBand->attack_coef) = a_coef;
                *(float*)&(targetBand->release_coef) = r_coef;
            }
        }
    }
    delay(10);
}
