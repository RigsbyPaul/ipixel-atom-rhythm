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

// NVS key "sz" + 12 hex digits of the address (14 chars, under the 15-char limit).
static void size_key(const ipixel_addr_t *a, char key[16])
{
    static const char hex[] = "0123456789abcdef";
    key[0] = 's';
    key[1] = 'z';
    for (int i = 0; i < 6; i++) {
        key[2 + i * 2] = hex[a->val[5 - i] >> 4];
        key[3 + i * 2] = hex[a->val[5 - i] & 0x0F];
    }
    key[14] = 0;
}

void settings_save_panel_size(const ipixel_addr_t *addr, int width, int height)
{
    int ow, oh;
    if (settings_load_panel_size(addr, &ow, &oh) && ow == width && oh == height) return;
    char key[16];
    size_key(addr, key);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, key, (uint16_t)((width << 8) | height));
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "panel size %d x %d saved", width, height);
    }
}

bool settings_load_panel_size(const ipixel_addr_t *addr, int *width, int *height)
{
    char key[16];
    size_key(addr, key);
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_u16(h, key, &v) == ESP_OK;
    nvs_close(h);
    if (!ok || (v >> 8) == 0 || (v & 0xFF) == 0) return false;
    *width = v >> 8;
    *height = v & 0xFF;
    return true;
}
