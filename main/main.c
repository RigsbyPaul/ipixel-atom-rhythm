// iPixel Rhythm: Echo Base mic -> frequency bands -> bars on the iPixel panel over BLE.
// Styles 0..4 use the panel's own rhythm screen (11 levels, panel decides the layout).
// Style 5 draws width/3 bars itself in the panel's live drawing mode.
// Style 6 is live disco: whole-panel pictures of the music (disco.c) sent as
// raw RGB live frames in the panel's DIY mode (live.c).
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
#include "disco.h"
#include "live.h"
#include "ipixel.h"
#include "ipixel_proto.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "main";

#define FRAME_MS 50         // 20 fps to the panel; ipixel-ble uses 400 ms
#define SILENCE_S 10        // seconds of empty bars before streaming stops
#define LOG_EVERY 8         // blocks (~0.25 s) between level prints
#define DRAWN_TILT_DB 10.0f  // style 5 treble lift across all bars
#define STATUS_MS 5000      // status line interval
#define STYLE_SELF_DRAW IPIXEL_RHYTHM_STYLES  // 5
#define STYLE_DISCO (STYLE_SELF_DRAW + 1)        // 6: saved animation pulsed by the music
#define STYLE_COUNT (STYLE_DISCO + 1)

enum { DRAW_NONE, DRAW_BARS, DRAW_DISCO };

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
        fx_listen(lv, (float)BANDS_FFT_SIZE / AUDIO_SAMPLE_RATE);  // every block, so no beat is missed

        // Self-draw bands follow the panel size it reported at connect.
        int w, h;
        if (ipixel_panel_size(&w, &h)) {
            if (w != drawn_w || h != drawn_h) {
                drawn_w = w;
                drawn_h = h;
                bands_init(&drawn, draw_bar_count(w), h);
                // Less treble lift than the music screen (16.5 dB): the right-hand
                // bars were too lively.
                drawn.tilt = drawn.count > 1 ? DRAWN_TILT_DB / (drawn.count - 1) : 0;
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
        ESP_LOGI(TAG, "style %u%s", s_style,
                 s_style == STYLE_SELF_DRAW ? " (self-draw)" : s_style == STYLE_DISCO ? " (disco)" : "");
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
    int drawing = DRAW_NONE;  // what the panel is in live drawing mode for
    bool idle = false;
    float quiet_s = 0;
    uint32_t sent = 0, failed = 0;
    int last_err = 0;
    TickType_t last = xTaskGetTickCount();
    TickType_t last_status = last;
    TickType_t last_size_ask = last;
    int size_tries = 0;
    bool started = false;  // size settled and power/brightness sent for this connection
    int64_t last_us = esp_timer_get_time();
    uint32_t disco_frames = 0;
    int64_t disco_last_us = 0;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(FRAME_MS));
        if (!ipixel_ready()) {
            was_ready = false;
            drawing = DRAW_NONE;
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
        const uint8_t *lv = self_draw ? drawn : native;
        int n = self_draw ? count : IPIXEL_RHYTHM_BARS;

        // Style 6: live disco. The panel stays in DIY mode and gets a new
        // whole-panel picture as fast as it takes them (~2.4 a second).
        if (disco) {
            if (drawing != DRAW_DISCO) {
                // Get the panel off its saved program first: entering DIY mode
                // straight from it (e.g. starting up on style 6) left the saved
                // program showing. One music-screen frame, then leave and enter
                // DIY mode, as switching here from styles 0..5 does.
                ipixel_build_rhythm(cmd, 0, native);
                ipixel_write(cmd, sizeof cmd);
                vTaskDelay(pdMS_TO_TICKS(100));
                draw_leave();
                vTaskDelay(pdMS_TO_TICKS(100));
                drawing = draw_enter() == 0 ? DRAW_DISCO : DRAW_NONE;
                if (drawing == DRAW_NONE) {
                    failed++;
                    continue;
                }
                disco_last_us = now_us;
            }
            uint8_t *rgb = live_begin(pw, ph);
            uint8_t avg[3] = {0};
            int rc = -1;
            if (rgb) {
                disco_render(rgb, pw, ph, (now_us - disco_last_us) / 1e6f, avg);
                disco_last_us = now_us;
                rc = live_send();
            }
            if (rc == 0) {
                sent++;
                disco_frames++;
            } else {
                failed++;
                last_err = rc;
            }
            ui_set_live_colour(avg[0], avg[1], avg[2]);
            goto status;
        }
        if (drawing == DRAW_DISCO) {
            draw_leave();
            drawing = DRAW_NONE;
        }

        // Silence: after SILENCE_S of empty bars stop sending; the panel goes
        // back to its saved program. Wake as soon as a bar shows real sound.
        if (any_above(lv, n, idle ? 1 : 0)) {
            quiet_s = 0;
            if (idle) {
                idle = false;
                ESP_LOGI(TAG, "sound again, streaming");
            }
        } else if (!idle && !self_draw && (quiet_s += dt) >= SILENCE_S) {
            // (Style 5 never pauses: silence is its 1-pixel line, and unchanged
            // pixels cost nothing to keep.)
            idle = true;
            ESP_LOGI(TAG, "quiet for %d s, pausing", SILENCE_S);
        }

        if (self_draw) idle = false;  // e.g. switched to style 5 while quiet

        // Switch the panel in or out of drawing mode as needed.
        bool want_drawing = self_draw && !idle;
        if (drawing == DRAW_BARS && !want_drawing) {
            draw_leave();
            drawing = DRAW_NONE;
        } else if (!drawing && want_drawing && draw_enter() == 0) {
            drawing = DRAW_BARS;
        }
        if (idle) {
            continue;
        }

        int rc;
        if (drawing) {
            // On failure draw.c marks the affected bars for a full redraw; no need
            // to clear the screen (a full redraw at once is what floods the link).
            rc = draw_frame(drawn, count, pw, h);
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
    status:
        if (xTaskGetTickCount() - last_status >= pdMS_TO_TICKS(STATUS_MS)) {
            last_status = xTaskGetTickCount();
            ESP_LOGI(TAG, "status: style %u, %s, frames sent %lu, failed %lu (last error %d), "
                     "disco %.1f fps, free heap %lu",
                     s_style, drawing == DRAW_BARS ? "self-draw" : drawing == DRAW_DISCO ? "disco" : "music screen",
                     (unsigned long)sent, (unsigned long)failed, last_err, disco_frames * 1000.0f / STATUS_MS,
                     (unsigned long)esp_get_free_heap_size());
            if (drawing == DRAW_DISCO) {
                uint32_t a[5];
                live_acks(a);
                ESP_LOGI(TAG, "disco panel answers: next %lu, end of frame %lu, invalid %lu, timeouts %lu",
                         (unsigned long)a[1], (unsigned long)a[2], (unsigned long)a[0], (unsigned long)a[4]);
            }
            disco_frames = 0;
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
