#include "sound.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "touch.h"

// ES3C28P audio: ES8311 on I2C0 (shared with touch) and I2S0, FM8002E amplifier enabled by GPIO1 (active low)
#define PIN_MCLK   4
#define PIN_BCLK   5
#define PIN_WS     7
#define PIN_DOUT   8
#define PIN_DIN    6
#define PIN_AMP_EN 1

#define SAMPLE_RATE    22050
#define CHUNK_FRAMES   256
#define ATTACK_MS      4     // TentacleOS: ENV_ATTACK_MS
#define RELEASE_MS     8     // TentacleOS: ENV_RELEASE_MS
#define MIN_GAP_MS     120   // TentacleOS: FB_MIN_GAP_MS
#define DEFAULT_VOLUME 70

static const char* TAG = "sound";

typedef struct {
    uint16_t freq_hz; // 0 = rest
    uint16_t dur_ms;
} Note;

typedef struct {
    const Note* notes;
    int count;
    float amp;
} SoundDef;

// Tones from TentacleOS (firmware_p4/.../feedback/ui_feedback.c and audio_i2s.c)
static const Note SND_NAV[] = {{2000, 32}};
static const Note SND_SELECT[] = {{1568, 40}};
static const Note SND_KEY[] = {{2000, 30}};                           // their audio_click()
static const Note SND_READ[] = {{1318, 60}, {1976, 95}};
static const Note SND_WRITE[] = {{1568, 45}, {2093, 80}};
static const Note SND_ERROR[] = {{392, 90}, {262, 140}};              // ours: falling, low
static const Note SND_BOOT[] = {{523, 120}, {659, 120}, {784, 175}};  // C5 E5 G5
static const Note SND_PLUG[] = {{1046, 45}, {1568, 55}, {2093, 80}};   // TentacleOS: SD card in
static const Note SND_UNPLUG[] = {{2093, 45}, {1568, 55}, {1046, 80}}; // TentacleOS: SD card out
static const Note SND_OFF[] = {{784, 120}, {659, 120}, {523, 175}};   // ours: the boot chime falling

static const SoundDef sounds[SOUND_COUNT] = {
    [SOUND_NAV] = {SND_NAV, 1, 0.30f},
    [SOUND_SELECT] = {SND_SELECT, 1, 0.32f},
    [SOUND_KEY] = {SND_KEY, 1, 0.18f},
    [SOUND_READ] = {SND_READ, 2, 0.40f},
    [SOUND_WRITE] = {SND_WRITE, 2, 0.40f},
    [SOUND_ERROR] = {SND_ERROR, 2, 0.40f},
    [SOUND_BOOT] = {SND_BOOT, 3, 0.35f},
    [SOUND_OFF] = {SND_OFF, 3, 0.35f},
    [SOUND_PLUG] = {SND_PLUG, 3, 0.40f},
    [SOUND_UNPLUG] = {SND_UNPLUG, 3, 0.35f},
};

static esp_codec_dev_handle_t codec;
static QueueHandle_t queue;
static volatile bool playing;
static int64_t last_accept_ms;
static portMUX_TYPE accept_lock = portMUX_INITIALIZER_UNLOCKED;
static int volume = DEFAULT_VOLUME;
static int16_t chunk[CHUNK_FRAMES * 2]; // stereo

static void render_note(const Note* note, float amp) {
    const int total = SAMPLE_RATE * note->dur_ms / 1000;
    const int attack = SAMPLE_RATE * ATTACK_MS / 1000;
    const int release = SAMPLE_RATE * RELEASE_MS / 1000;
    const float step = 2.0f * (float)M_PI * note->freq_hz / SAMPLE_RATE;
    float phase = 0;
    for (int done = 0; done < total;) {
        int n = total - done > CHUNK_FRAMES ? CHUNK_FRAMES : total - done;
        for (int i = 0; i < n; i++) {
            int16_t v = 0;
            if (note->freq_hz > 0) {
                int index = done + i;
                // Raised-cosine attack and release: no clicks at the edges
                float env = 1.0f;
                if (index < attack) env = 0.5f * (1.0f - cosf((float)M_PI * index / attack));
                else if (index >= total - release) env = 0.5f * (1.0f - cosf((float)M_PI * (total - index) / release));
                v = (int16_t)(sinf(phase) * amp * env * 32760.0f);
                phase += step;
                if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            }
            chunk[i * 2] = v;
            chunk[i * 2 + 1] = v;
        }
        esp_codec_dev_write(codec, chunk, n * 2 * sizeof(int16_t));
        done += n;
    }
}

static void write_silence(int ms) {
    memset(chunk, 0, sizeof(chunk));
    int frames = SAMPLE_RATE * ms / 1000;
    while (frames > 0) {
        int n = frames > CHUNK_FRAMES ? CHUNK_FRAMES : frames;
        esp_codec_dev_write(codec, chunk, n * 2 * sizeof(int16_t));
        frames -= n;
    }
}

static void sound_task(void* arg) {
    SoundKind kind;
    while (true) {
        xQueueReceive(queue, &kind, portMAX_DELAY);
        const SoundDef* def = &sounds[kind];
        for (int i = 0; i < def->count; i++) render_note(&def->notes[i], def->amp);
        write_silence(10); // flush the DMA so the next sound starts on silence
        playing = false;
    }
}

void sound_play(SoundKind kind) {
    if (codec == NULL || volume == 0 || kind >= SOUND_COUNT) return;
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool accepted = false;
    portENTER_CRITICAL(&accept_lock);
    if (!playing && now_ms - last_accept_ms >= MIN_GAP_MS) {
        playing = true;
        last_accept_ms = now_ms;
        accepted = true;
    }
    portEXIT_CRITICAL(&accept_lock);
    if (accepted && xQueueSend(queue, &kind, 0) != pdTRUE) playing = false;
}

void sound_set_volume(int percent) {
    volume = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    if (codec != NULL) esp_codec_dev_set_out_vol(codec, volume);
    nvs_handle_t nvs;
    if (nvs_open("deck", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "volume", volume);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

int sound_volume(void) {
    return volume;
}

void sound_init(void) {
    nvs_handle_t nvs;
    uint8_t saved;
    if (nvs_open("deck", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_u8(nvs, "volume", &saved) == ESP_OK) volume = saved;
        nvs_close(nvs);
    }

    // Amplifier on for good (like the vendor's device tree: a GPIO held low)
    gpio_hold_dis(PIN_AMP_EN); // held off while the deck was off
    gpio_config_t amp = {.pin_bit_mask = 1ULL << PIN_AMP_EN, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&amp);
    gpio_set_level(PIN_AMP_EN, 0);

    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_config.auto_clear = true;
    if (i2s_new_channel(&chan_config, &tx, &rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s channel failed");
        return;
    }
    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_MCLK,
            .bclk = PIN_BCLK,
            .ws = PIN_WS,
            .dout = PIN_DOUT,
            .din = PIN_DIN,
        },
    };
    i2s_channel_init_std_mode(tx, &std_config);
    i2s_channel_init_std_mode(rx, &std_config);
    i2s_channel_enable(tx);
    i2s_channel_enable(rx);

    audio_codec_i2c_cfg_t i2c_config = {.port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = board_i2c_bus()};
    const audio_codec_ctrl_if_t* ctrl = audio_codec_new_i2c_ctrl(&i2c_config);
    audio_codec_i2s_cfg_t i2s_config = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t* data = audio_codec_new_i2s_data(&i2s_config);
    const audio_codec_gpio_if_t* gpio = audio_codec_new_gpio();
    if (ctrl == NULL || data == NULL) {
        ESP_LOGE(TAG, "codec interfaces failed");
        return;
    }

    es8311_codec_cfg_t es8311 = {
        .ctrl_if = ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,          // the amplifier is driven above
        .use_mclk = true,
    };
    const audio_codec_if_t* codec_if = es8311_codec_new(&es8311);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "es8311 not found");
        return;
    }
    esp_codec_dev_cfg_t dev_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data,
    };
    codec = esp_codec_dev_new(&dev_config);
    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = SAMPLE_RATE,
    };
    if (codec == NULL || esp_codec_dev_open(codec, &format) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        codec = NULL;
        return;
    }
    esp_codec_dev_set_out_vol(codec, volume);

    queue = xQueueCreate(1, sizeof(SoundKind));
    // Above the carousel (main task) and the terminal renderer, so drawing never starves the sample feed
    xTaskCreate(sound_task, "sound", 3072, NULL, 6, NULL);
}

void sound_shutdown(void) {
    if (codec != NULL) esp_codec_dev_close(codec);
    gpio_set_level(PIN_AMP_EN, 1); // amplifier off
    gpio_hold_en(PIN_AMP_EN);
}
