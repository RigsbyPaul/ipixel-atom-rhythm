// AtomS3 Lite button (G41, active low) and WS2812 status LED (G35).
// LED: blue blinking = scanning, yellow = connecting, steady = connected in the
// current style's colour (STYLE_RGB). Disco styles 6..11 show their colour for
// 1 s after a press and while the panel is dark, and otherwise follow the
// panel's average colour. White flash = button press acknowledged.
#include "ui.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "ipixel.h"

#define PIN_BUTTON 41
#define PIN_LED 35
#define LONG_PRESS_MS 700
#define TICK_MS 20

static const char *TAG = "ui";
static led_strip_handle_t s_led;
static ui_press_cb s_on_press;
static volatile int s_flash_ticks;
static volatile uint8_t s_style;

// Connected colour for each style (kept clear of scanning blue and connecting
// yellow). Disco styles 6..11 show theirs for a second after a press, and
// whenever the panel is dark; otherwise the LED follows the panel.
#define STYLES 12
static const uint8_t STYLE_RGB[STYLES][3] = {
    {0, 14, 0},    // 0 green
    {0, 12, 12},   // 1 cyan
    {16, 0, 14},   // 2 magenta
    {24, 6, 0},    // 3 orange
    {22, 0, 2},    // 4 red
    {10, 0, 25},   // 5 purple (self-draw)
    {12, 25, 0},   // 6 lime (colour wash)
    {25, 2, 10},   // 7 pink (beat flash)
    {4, 12, 25},   // 8 sky blue (bass pulse)
    {14, 8, 25},   // 9 lavender (spectrum glow)
    {25, 18, 10},  // 10 warm white (sparkle)
    {6, 25, 14},   // 11 mint (rainbow chase)
};
#define HOLD_TICKS (1000 / TICK_MS)  // show the fixed colour for 1 s after a press

void ui_flash(void) { s_flash_ticks = 5; }
static volatile uint8_t s_live[3];
static volatile uint32_t s_live_gen;
static volatile int s_hold_ticks;

void ui_set_style(uint8_t style)
{
    s_style = style < STYLES ? style : 0;
    s_hold_ticks = HOLD_TICKS;
}

void ui_set_live_colour(uint8_t r, uint8_t g, uint8_t b)
{
    // The LED is far brighter than the panel's glow: scale it down.
    s_live[0] = r / 8;
    s_live[1] = g / 8;
    s_live[2] = b / 8;
    s_live_gen++;
}

static void set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) return;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

static void ui_task(void *arg)
{
    int held = 0;  // ticks the button has been down
    bool long_sent = false;
    uint32_t tick = 0;
    int last_colour = -1;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        tick++;

        // Button: long press fires while held; short press on release.
        if (gpio_get_level(PIN_BUTTON) == 0) {
            held++;
            if (!long_sent && held * TICK_MS >= LONG_PRESS_MS) {
                long_sent = true;
                s_on_press(UI_LONG_PRESS);
            }
        } else {
            if (held * TICK_MS >= 40 && !long_sent) {
                s_on_press(UI_SHORT_PRESS);
            }
            held = 0;
            long_sent = false;
        }

        // LED.
        if (s_hold_ticks > 0) s_hold_ticks--;
        int colour;
        if (s_flash_ticks > 0) {
            s_flash_ticks--;
            colour = 3;
        } else {
            switch (ipixel_state()) {
            case IPIXEL_READY: {
                // Disco styles follow the panel's colour, except for 1 s after a
                // press and while the panel is dark (quiet, faded, or no size yet).
                bool dark = s_live[0] + s_live[1] + s_live[2] == 0;
                if (s_style < 6 || s_hold_ticks > 0 || dark) {
                    colour = 10 + s_style;
                } else {
                    colour = 100 + (int)(s_live_gen & 0x7FFF);
                }
                break;
            }
            case IPIXEL_CONNECTING: colour = 1; break;
            default: colour = (tick / 25) % 2 ? 0 : 4; break;  // blink blue, 1 Hz
            }
        }
        if (colour != last_colour) {
            last_colour = colour;
            switch (colour) {
            case 0: set(0, 0, 0); break;
            case 1: set(20, 14, 0); break;
            case 3: set(30, 30, 30); break;
            case 4: set(0, 0, 25); break;
            default:
                if (colour >= 100) {
                    set(s_live[0], s_live[1], s_live[2]);
                } else {
                    const uint8_t *c = STYLE_RGB[colour - 10];
                    set(c[0], c[1], c[2]);
                }
                break;
            }
        }
    }
}

void ui_start(ui_press_cb on_press)
{
    s_on_press = on_press;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    led_strip_config_t strip = {
        .strip_gpio_num = PIN_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&strip, &rmt, &s_led) != ESP_OK) {
        ESP_LOGW(TAG, "status LED not available");
        s_led = NULL;
    }
    set(0, 0, 0);

    xTaskCreatePinnedToCore(ui_task, "ui", 3072, NULL, 4, NULL, 0);
}
