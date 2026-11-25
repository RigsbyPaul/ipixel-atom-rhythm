// iPixel Rhythm: Echo Base mic -> frequency bands -> bars on the iPixel panel over BLE.
// Styles 0..4 use the panel's own rhythm screen (11 levels, panel decides the layout).
// Style 5 draws width/3 bars itself in the panel's live drawing mode.
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio.h"
#include "bands.h"
#include "draw.h"
#include "fx.h"
#include "live.h"
#include "ipixel.h"
#include "ipixel_proto.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "main";

#define FRAME_MS 50         // 20 fps to the panel; ipixel-ble uses 400 ms
#define SILENCE_S 10        // seconds of empty bars before streaming stops
#define FADE_STEPS 8        // disco frames to fade to black when it goes quiet
#define LOG_EVERY 8         // blocks (~0.25 s) between level prints
#define STATUS_MS 5000      // status line interval
#define STYLE_SELF_DRAW IPIXEL_RHYTHM_STYLES  // 5
#define STYLE_DISCO (STYLE_SELF_DRAW + 1)        // 6..11: whole-panel disco effects
#define STYLE_COUNT (STYLE_DISCO + FX_COUNT)

static const uint8_t BRIGHTNESS_STEPS[] = {20, 40, 60, 80, 100};
static volatile uint8_t s_style;
static volatile uint8_t s_brightness;
static volatile bool s_brightness_dirty;

// Latest levels from the audio task: native (11 x 0..15) and self-draw (n x 0..h).
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_native[IPIXEL_RHYTHM_BARS];
static uint8_t s_drawn[BANDS_MAX];
static int s_drawn_count, s_drawn_h;

static void audio_task(void *arg)
{
    static int32_t buf[BANDS_FFT_SIZE];  // 32 ms at 16 kHz
    static bands_t native, drawn;
    uint8_t lv[BANDS_MAX];
    int drawn_w = 0, drawn_h = 0;
    int blocks = 0;

    bands_init(&native, IPIXEL_RHYTHM_BARS, IPIXEL_RHYTHM_LEVEL_MAX);
    for (;;) {
        size_t got = 0;
        while (got < BANDS_FFT_SIZE) {
            got += audio_read(buf + got, BANDS_FFT_SIZE - got);
        }
        bool silent = bands_fft(buf);

        bands_levels(&native, silent, lv);
        taskENTER_CRITICAL(&s_lock);
        memcpy(s_native, lv, IPIXEL_RHYTHM_BARS);
        taskEXIT_CRITICAL(&s_lock);

        // Self-draw bands follow the panel size it reported at connect.
        int w, h;
        if (ipixel_panel_size(&w, &h)) {
            if (w != drawn_w || h != drawn_h) {
                drawn_w = w;
                drawn_h = h;
                bands_init(&drawn, draw_bar_count(w), h);
                ESP_LOGI(TAG, "self-draw: %d bars x %d px", drawn.count, h);
            }
            bands_levels(&drawn, silent, lv);
            taskENTER_CRITICAL(&s_lock);
            memcpy(s_drawn, lv, drawn.count);
            s_drawn_count = drawn.count;
            s_drawn_h = h;
            taskEXIT_CRITICAL(&s_lock);
        }

        if (++blocks % LOG_EVERY == 0) {
            static const char hex[] = "0123456789ABCDEF";
            char line[IPIXEL_RHYTHM_BARS * 2 + 1];
            for (int b = 0; b < IPIXEL_RHYTHM_BARS; b++) {
                line[b * 2] = hex[s_native[b] & 0x0F];
                line[b * 2 + 1] = ' ';
            }
            line[IPIXEL_RHYTHM_BARS * 2 - 1] = 0;
            ESP_LOGD(TAG, "bars [%s]", line);
        }
    }
}

static void on_press(ui_press_t press)
{
    ui_flash();
    if (press == UI_SHORT_PRESS) {
        s_style = (s_style + 1) % STYLE_COUNT;
        ui_set_style(s_style);
        settings_save_style(s_style);
        static const char *const fx_names[FX_COUNT] = {"colour wash", "beat flash", "bass pulse",
                                                       "spectrum glow", "sparkle", "rainbow chase"};
        ESP_LOGI(TAG, "style %u%s%s", s_style, s_style == STYLE_SELF_DRAW ? " (self-draw)" : "",
                 s_style >= STYLE_DISCO ? fx_names[s_style - STYLE_DISCO] : "");
    } else {
        int next = 0;
        for (int i = 0; i < (int)sizeof BRIGHTNESS_STEPS; i++) {
            if (BRIGHTNESS_STEPS[i] > s_brightness) { next = i; break; }
        }
        s_brightness = BRIGHTNESS_STEPS[next];
        s_brightness_dirty = true;
        settings_save_brightness(s_brightness);
        ESP_LOGI(TAG, "brightness %u", s_brightness);
    }
}

static bool any_above(const uint8_t *levels, int n, int v)
{
    for (int b = 0; b < n; b++) {
        if (levels[b] > v) return true;
    }
    return false;
}

static void send_task(void *arg)
{
    uint8_t native[IPIXEL_RHYTHM_BARS];
    uint8_t drawn[BANDS_MAX];
    uint8_t cmd[IPIXEL_RHYTHM_LEN];
    bool was_ready = false;
    bool drawing = false;  // panel is in live drawing mode
    bool idle = false;
    float quiet_s = 0;
    int fade_left = 0;  // disco frames left in the fade to black
    uint32_t sent = 0, failed = 0;
    int last_err = 0;
    TickType_t last = xTaskGetTickCount();
    TickType_t last_status = last;
    TickType_t last_size_ask = last;
    int size_tries = 0;
    bool started = false;  // size settled and power/brightness sent for this connection
    int64_t last_us = esp_timer_get_time();
    uint32_t live_frames = 0;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(FRAME_MS));
        if (!ipixel_ready()) {
            was_ready = false;
            drawing = false;
            continue;
        }
        if (!was_ready) {
            was_ready = true;
            started = false;
            sent = failed = 0;
            last_size_ask = xTaskGetTickCount();  // the connect already asked once
            size_tries = 0;
        }

        // Panel size: some panels (a 192 x 32 one) don't always answer the request
        // sent at connect, and seem to miss it while other commands arrive. So until
        // it answers, send nothing else for up to 6 s, asking again every 2 s.
        // If it never answers, use the size it reported on an earlier connect.
        if (!started) {
            int w, ph;
            bool known = ipixel_panel_size(&w, &ph);
            if (!known && size_tries < 3) {
                if (xTaskGetTickCount() - last_size_ask >= pdMS_TO_TICKS(2000)) {
                    last_size_ask = xTaskGetTickCount();
                    size_tries++;
                    ESP_LOGW(TAG, "no panel size yet, asking again (%d)", size_tries);
                    ipixel_request_panel_size();
                }
                continue;
            }
            ipixel_addr_t addr;
            if (!known && ipixel_connected_addr(&addr) && settings_load_panel_size(&addr, &w, &ph)) {
                ESP_LOGW(TAG, "panel didn't report its size; using saved %d x %d", w, ph);
                ipixel_set_panel_size(w, ph);
            } else if (!known) {
                ESP_LOGW(TAG, "panel size unknown: style 5 shows the music screen instead");
            }
            uint8_t c[5];
            ipixel_build_power(c, 1);
            ipixel_write(c, sizeof c);
            s_brightness_dirty = true;
            started = true;
        }
        if (s_brightness_dirty) {
            uint8_t c[5];
            ipixel_build_brightness(c, s_brightness);
            if (ipixel_write(c, sizeof c) == 0) s_brightness_dirty = false;
        }

        int count, h;
        taskENTER_CRITICAL(&s_lock);
        memcpy(native, s_native, sizeof native);
        count = s_drawn_count;
        h = s_drawn_h;
        memcpy(drawn, s_drawn, count);
        taskEXIT_CRITICAL(&s_lock);

        int pw = 0, ph = 0;
        bool sized = ipixel_panel_size(&pw, &ph);
        bool self_draw = s_style == STYLE_SELF_DRAW && count > 0;
        bool disco = s_style >= STYLE_DISCO && sized;
        int64_t now_us = esp_timer_get_time();
        float dt = (now_us - last_us) / 1e6f;
        last_us = now_us;
        fx_listen(native, dt);
        const uint8_t *lv = self_draw ? drawn : native;
        int n = self_draw ? count : IPIXEL_RHYTHM_BARS;

        // Silence: after SILENCE_S of empty bars stop sending; the panel goes
        // back to its saved program (disco styles fade to black first). Wake as
        // soon as a bar shows real sound. Timed in seconds, since disco frames
        // come slower than FRAME_MS.
        if (any_above(lv, n, idle ? 1 : 0)) {
            quiet_s = 0;
            if (idle) {
                idle = false;
                fade_left = 0;
                ESP_LOGI(TAG, "sound again, streaming");
            }
        } else if (!idle && (quiet_s += dt) >= SILENCE_S) {
            idle = true;
            fade_left = disco ? FADE_STEPS : 0;
            ESP_LOGI(TAG, "quiet for %d s, %s", SILENCE_S, disco ? "fading to black" : "pausing");
        }

        // Fade the last disco picture to black over FADE_STEPS frames.
        if (idle && fade_left > 0) {
            uint8_t *rgb = live_begin(pw, ph);
            if (rgb) {
                float k = (float)(fade_left - 1) / fade_left;  // last step is all black
                for (int i = 0; i < pw * ph * 3; i++) rgb[i] = (uint8_t)(rgb[i] * k);
                live_send();
            }
            fade_left--;
            ui_set_live_colour(0, 0, 0);
            continue;
        }

        // Switch the panel in or out of drawing mode as needed.
        bool want_drawing = self_draw && !idle;
        if (drawing && !want_drawing) {
            draw_leave();
            drawing = false;
        } else if (!drawing && want_drawing) {
            drawing = draw_enter() == 0;
        }
        if (idle) {
            continue;
        }

        int rc;
        if (drawing) {
            // On failure draw.c marks the affected bars for a full redraw; no need
            // to clear the screen (a full redraw at once is what floods the link).
            rc = draw_frame(drawn, count, pw, h);
        } else if (disco) {
            // Whole-panel picture as a live frame. Confirmed writes make this
            // block until the panel has it, which sets the frame rate.
            uint8_t *rgb = live_begin(pw, ph);
            uint8_t avg[3] = {0};
            if (rgb) {
                fx_render((fx_t)(s_style - STYLE_DISCO), rgb, pw, ph, dt, avg);
                rc = live_send();
                if (rc == 0) live_frames++;
            } else {
                rc = -1;
            }
            ui_set_live_colour(avg[0], avg[1], avg[2]);
        } else {
            ipixel_build_rhythm(cmd, s_style, native);
            rc = ipixel_write(cmd, sizeof cmd);
        }
        if (rc == 0) {
            sent++;
        } else {
            failed++;
            last_err = rc;
        }
        if (xTaskGetTickCount() - last_status >= pdMS_TO_TICKS(STATUS_MS)) {
            last_status = xTaskGetTickCount();
            ESP_LOGI(TAG, "status: style %u, %s, frames sent %lu, failed %lu (last error %d), "
                     "live %.1f fps, last jpeg %d B, free heap %lu",
                     s_style, drawing ? "self-draw" : disco ? "disco" : "music screen", (unsigned long)sent,
                     (unsigned long)failed, last_err, live_frames * 1000.0f / STATUS_MS, live_last_size(),
                     (unsigned long)esp_get_free_heap_size());
            live_frames = 0;
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (audio_init() == ESP_OK) {
        xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 6, NULL, 1);
    } else {
        ESP_LOGE(TAG, "mic not started");
    }
    settings_t cfg;
    settings_load(&cfg);
    s_style = cfg.style;
    s_brightness = cfg.brightness;
    ui_set_style(s_style);
    ui_start(on_press);
    ipixel_on_size(settings_save_panel_size);
    ipixel_start(cfg.have_panel ? &cfg.panel : NULL, settings_save_panel);
    xTaskCreatePinnedToCore(send_task, "send", 8192, NULL, 5, NULL, 0);
}
