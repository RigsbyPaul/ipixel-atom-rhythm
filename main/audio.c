// Atomic Echo Base microphone on AtomS3 Lite.
// Pins, addresses and the ES8311 setup follow m5stack/M5Atomic-EchoBase:
// I2C SDA 38 / SCL 39, I2S BCK 8 / WS 6 / DIN 7 / DOUT 5, no MCLK
// (the ES8311 derives its clock from BCK), ES8311 at 0x18, PI4IOE5V6408 at 0x43.
#include "audio.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define PIN_SDA 38
#define PIN_SCL 39
#define PIN_BCK 8
#define PIN_WS 6
#define PIN_DIN 7

#define ES8311_ADDR 0x18
#define PI4IOE_ADDR 0x43

// Mic gain: REG14 low nibble = analog PGA (0..10 -> 0..30 dB),
// REG16 = digital gain (0..7 -> 0..42 dB, 6 dB steps).
#define MIC_PGA 0x0A
#define MIC_DIGITAL_GAIN 0x04  // +24 dB (was +12: quiet room sat near -68 dBFS)

static i2c_master_dev_handle_t s_codec, s_ioexp;
static i2s_chan_handle_t s_rx;

static esp_err_t wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(dev, b, 2, 100);
}

static esp_err_t codec_init(void)
{
    // Reset, then slave mode with everything powered.
    ESP_RETURN_ON_ERROR(wr(s_codec, 0x00, 0x1F), TAG, "ES8311 not answering at 0x18");
    vTaskDelay(pdMS_TO_TICKS(20));
    wr(s_codec, 0x00, 0x00);
    wr(s_codec, 0x00, 0x80);

    // Clocks: MCLK from BCK (bit 7), all clocks on. BCK = 16 kHz * 64 = 1.024 MHz,
    // x8 multiplier -> 8.192 MHz internal (Espressif coeff table: 1024000 / 16000).
    wr(s_codec, 0x01, 0xBF);
    wr(s_codec, 0x02, 0x18);  // pre_div 1, pre_multi x8
    wr(s_codec, 0x03, 0x10);  // single speed, ADC OSR 16
    wr(s_codec, 0x04, 0x10);  // DAC OSR
    wr(s_codec, 0x05, 0x00);  // ADC/DAC dividers 1
    wr(s_codec, 0x06, 0x03);  // BCLK divider 4
    wr(s_codec, 0x07, 0x00);  // LRCK divider 256 (0x00ff + 1)
    wr(s_codec, 0x08, 0xFF);

    // Serial ports: I2S format, 32-bit words.
    wr(s_codec, 0x09, 0x10);  // DAC in
    wr(s_codec, 0x0A, 0x10);  // ADC out

    // Power up analog, PGA and ADC modulator.
    wr(s_codec, 0x0D, 0x01);
    wr(s_codec, 0x0E, 0x02);
    wr(s_codec, 0x12, 0x00);
    wr(s_codec, 0x13, 0x10);
    wr(s_codec, 0x1C, 0x6A);  // ADC equaliser bypass, DC offset cancel
    wr(s_codec, 0x37, 0x08);

    // Mic: analog input with PGA, digital gain, ADC volume 0 dB.
    wr(s_codec, 0x14, 0x10 | MIC_PGA);
    wr(s_codec, 0x16, MIC_DIGITAL_GAIN);
    wr(s_codec, 0x17, 0xBF);

    // We never send audio out: mute the DAC so the speaker stays quiet.
    wr(s_codec, 0x31, 0x60);
    wr(s_codec, 0x32, 0x00);
    return ESP_OK;
}

static esp_err_t ioexp_init(void)
{
    // Same as the M5 library: push-pull off, pull-ups on, P0..P3,P5,P6 outputs, all high.
    ESP_RETURN_ON_ERROR(wr(s_ioexp, 0x07, 0x00), TAG, "PI4IOE5V6408 not answering at 0x43");
    wr(s_ioexp, 0x0D, 0xFF);
    wr(s_ioexp, 0x03, 0x6F);
    wr(s_ioexp, 0x05, 0xFF);
    return ESP_OK;
}

esp_err_t audio_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "I2C bus");
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .scl_speed_hz = 100000};
    dev.device_address = ES8311_ADDR;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_codec), TAG, "codec dev");
    dev.device_address = PI4IOE_ADDR;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_ioexp), TAG, "ioexp dev");

    // Start I2S first: the ES8311 needs BCK running to clock its registers' logic.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "I2S channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_BCK,
            .ws = PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_DIN,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "I2S std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "I2S enable");

    ESP_RETURN_ON_ERROR(ioexp_init(), TAG, "IO expander");
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "codec");
    ESP_LOGI(TAG, "mic ready, %d Hz", AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

size_t audio_read(int32_t *samples, size_t n)
{
    size_t got = 0;
    if (i2s_channel_read(s_rx, samples, n * sizeof(int32_t), &got, portMAX_DELAY) != ESP_OK) {
        return 0;
    }
    return got / sizeof(int32_t);
}
