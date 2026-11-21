#include "settings.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";
#define NS "rhythm"

void settings_load(settings_t *s)
{
    s->style = 0;
    s->brightness = 60;
    s->have_panel = false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "nothing saved yet");
        return;
    }
    nvs_get_u8(h, "style", &s->style);
    nvs_get_u8(h, "bright", &s->brightness);
    size_t len = sizeof s->panel;
    s->have_panel = nvs_get_blob(h, "panel", &s->panel, &len) == ESP_OK && len == sizeof s->panel;
    nvs_close(h);
    if (s->style > 5) s->style = 0;  // 5 = self-draw
    if (s->brightness > 100) s->brightness = 60;
    ESP_LOGI(TAG, "style %u, brightness %u, saved panel: %s", s->style, s->brightness,
             s->have_panel ? "yes" : "no");
}

static void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void settings_save_style(uint8_t style) { save_u8("style", style); }
void settings_save_brightness(uint8_t brightness) { save_u8("bright", brightness); }

void settings_save_panel(const ipixel_addr_t *addr)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "panel", addr, sizeof *addr);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "panel address saved");
    }
}
