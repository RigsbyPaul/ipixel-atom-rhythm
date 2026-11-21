# iPixel Rhythm — plan

Listen to the room with an **AtomS3 Lite + Atomic Echo Base**, turn the sound into
11 bar levels, and stream them over BLE to an **iPixel Color LED matrix**, which
draws them with its built-in "rhythm" (music bars) screen.

```
 mic (ES8311 on Echo Base) --I2S--> AtomS3 Lite (ESP32-S3)
     FFT -> 11 bands -> 0..15 levels --BLE write fa02--> iPixel panel (LED_BLE_...)
```

The protocol facts below come from `P:\dev_pr\ipixel-ble\custom_components\ipixel_ble\`
(`protocol.py`, `device.py`, `switch.py`), which were traced from the iPixel Color app
and work on a real 192x32 panel.

---

## 1. Hardware

| Part | Notes |
|---|---|
| AtomS3 Lite | ESP32-S3FN8, 8 MB flash, **no PSRAM**, native USB (USB-Serial/JTAG) on **COM14**, RGB LED on G35, button on G41 |
| Atomic Echo Base | ES8311 codec (I2C 0x18) + MEMS mic + speaker amp; PI4IOE5V6408 IO expander (I2C 0x43) gates the amp |
| iPixel panel | Advertises `LED_BLE_…`, service `fa01`, one BLE connection at a time |

AtomS3 Lite ↔ Echo Base pins (**check these against M5's Echo Base docs** before writing driver code):

| Signal | GPIO |
|---|---|
| I2C SDA / SCL | G38 / G39 |
| I2S BCLK | G8 |
| I2S WS (LRCK) | G6 |
| I2S DOUT (to codec, speaker) | G5 |
| I2S DIN (from codec, mic) | G7 |

No MCLK pin: the ES8311 must be clocked from BCLK (`use_mclk = false`).

**Close the iPixel Color phone app** (and disable the Home Assistant iPixel BLE
integration for this panel) while testing: the panel accepts only one connection.

---

## 2. Set up ESP-IDF on Windows

1. Download the **ESP-IDF Windows Installer (Online)** from
   https://dl.espressif.com/dl/esp-idf/ and install the latest stable **ESP-IDF v5.x**
   (v5.4 or newer). Keep the default tools path (e.g. `C:\Espressif`), select the
   ESP32-S3 target and install the USB drivers.
2. Open **"ESP-IDF 5.x PowerShell"** from the Start menu (it runs `export.ps1`)
   and check the install:
   ```powershell
   idf.py --version
   ```
3. Optional: install the **ESP-IDF VS Code extension** and point it at the same install.
4. Check the board: Device Manager → Ports → *USB JTAG/serial debug unit (COM14)*.
   If flashing fails, hold the button while plugging in to force download mode.
5. Smoke test:
   ```powershell
   cd P:\dev_pr\ipixel-rhythm
   idf.py create-project hello
   cd hello
   idf.py set-target esp32s3
   idf.py -p COM14 flash monitor
   ```
   Exit the monitor with `Ctrl+]`.

---

## 3. Project layout

```
ipixel-rhythm/
  plan.md
  CMakeLists.txt
  sdkconfig.defaults
  main/
    CMakeLists.txt
    idf_component.yml      # espressif/esp_codec_dev, espressif/esp-dsp, espressif/led_strip
    main.c                 # task wiring, button, status LED
    audio.c / audio.h      # ES8311 + I2S RX setup, sample reads
    bands.c / bands.h      # FFT -> 11 levels with AGC and smoothing
    ipixel.c / ipixel.h    # NimBLE central: scan, connect, write fa02
    ipixel_proto.h         # byte builders (rhythm, brightness, power)
```

`sdkconfig.defaults`:
```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=n
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_FREERTOS_HZ=1000
```

`main/idf_component.yml`:
```yaml
dependencies:
  espressif/esp_codec_dev: "^1.3"
  espressif/esp-dsp: "^1.5"
  espressif/led_strip: "^2.5"
```

---

## 4. iPixel BLE protocol (what we need)

**GATT**
- Service `0000fa01-0000-1000-8000-00805f9b34fb`
- Write: `0000fa02-0000-1000-8000-00805f9b34fb`. Use write-with-response if the
  characteristic has the `write` property, else write-without-response (ipixel-ble
  checks this at connect time).
- Notify: `0000fa03-0000-1000-8000-00805f9b34fb`. Acks are 5 bytes `05 .. .. .. status`
  (1 = next frame, 3 = complete, 0 = invalid command, 2 = no space).
- Write in chunks of `MTU - 3` bytes (min 20). Request MTU 512 after connecting.
  Rhythm commands are 16 bytes, so they always fit in one write.

All multi-byte values are little-endian; every command starts with its 2-byte total length.

**Rhythm bars: the core of this project** (`build_rhythm` in `protocol.py`):
```
10 00 01 02 <style 0..4> <L0> <L1> ... <L10>      (16 bytes)
```
- 11 bars, each level **0..15**; style 0–4 picks the panel's bar look and colours.
- The panel sends no reply.
- The panel shows a rhythm frame for only ~1–2 s, so **stream continuously**.
  ipixel-ble's Music mode sends one every 400 ms with random levels; for real music
  aim for **~50 ms (20 fps)** and lower it if the panel or link falls behind.

**Useful extras**

| Command | Bytes |
|---|---|
| Power on/off | `05 00 07 01 <1/0>` |
| Brightness 0..100 | `05 00 04 80 <level>` |
| Built-in music animation | `06 00 00 02 <speed 0..7> <style 0/1>` |
| Time sync (reply = device info; byte 4 = panel type, byte 10 = password flag, 0xFF = none) | `08 00 01 80 hh mm ss 00` |

On connect, ipixel-ble enables notifications and sends a time sync; the reply gives
the panel type (e.g. 143 = 192x32). We log it but don't need the size: the panel
draws the rhythm screen itself.

**Finding the panel:** scan for an advertised name starting with `LED_BLE_` (or the
`fa01` service). Save the address in NVS so later boots connect straight away.

---

## 5. Audio → 11 bars

1. **Capture**: I2S RX via `esp_codec_dev` with ES8311 as ADC, 16 kHz, 16-bit, mono.
   Mic gain ~30 dB to start. Read 512-sample blocks (32 ms).
2. **Window + FFT**: Hann window, 512-point real FFT with `esp-dsp`
   (`dsps_fft2r_fc32`), 31.25 Hz per bin.
3. **Bands**: 11 log-spaced bands from ~60 Hz to 8 kHz
   (edges ≈ 60, 95, 150, 240, 380, 600, 950, 1500, 2400, 3800, 6000, 8000 Hz).
   Band energy = sum of |X|² over its bins, in dB.
4. **AGC**: track a slowly decaying peak and a noise floor so quiet and loud rooms
   both fill the range; map floor..peak → 0..15.
5. **Smoothing**: instant attack, slow release (each bar falls at most 1–2 per frame),
   like a VU meter.
6. **Silence**: below the noise floor send zeros; after N seconds of silence, switch to
   the built-in animation or stop streaming so the panel returns to its program.

---

## 6. Firmware tasks

| Task | Core | Job |
|---|---|---|
| `audio_task` | 1 | read I2S, FFT, compute levels, publish the latest levels (mutex-protected) |
| NimBLE host | 0 | scan, connect, reconnect, discover fa02/fa03 |
| `send_task` | 0 | every 50 ms while connected, write the rhythm command |

- Button (G41): short press = next style (0–4); long press = cycle brightness.
- RGB LED (G35, WS2812 via `led_strip`): blue = scanning, green = connected, red = error.
- Reconnect: on disconnect wait 1 s and reconnect to the saved address; rescan after
  3 failures.

---

## 7. Milestones

**Status (2026-11-21):** all 6 done (polish: button style/brightness, status LED,
NVS-saved style/brightness/panel address, 10 s silence pause; NimBLE log level WARNING).
Added later: the panel's own rhythm screen draws a fixed layout (8 bars shown twice),
whatever the 11 levels say. Style 5 (purple LED) draws width/3 bars itself in DIY pixel
mode, sending only changed pixels; 16 bars at 20 fps works on the 48x24 panel. The panel
size is read at connect from the time-sync reply (this sets the panel clock to 00:00). ESP-IDF v6.0.1 (EIM install, `C:\esp\v6.0.1\esp-idf`;
activate with `. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1`). Panel
`LED_BLE_18329AD5`: fa02 is write-without-response, MTU 512, 20 fps with no failures.
Mic gain: PGA 30 dB + digital 24 dB. The FFT is hand-written (no esp-dsp), and the codec
is driven over raw I2C (no esp_codec_dev).

1. **ESP-IDF works**: hello world flashes on COM14 and the monitor shows output.
2. **BLE drive test**: NimBLE central finds `LED_BLE_…`, connects, sets brightness and
   streams random levels (a port of `next_music_levels`). Bars move on the panel.
   This proves the protocol end to end before any audio work.
3. **Mic test**: ES8311 + I2S RX; print RMS over serial while clapping.
4. **Bands**: FFT → 11 levels printed as ASCII bars in the monitor.
5. **Join**: stream real levels to the panel; tune gain, AGC, frame rate, smoothing.
6. **Polish**: button styles, status LED, NVS-saved address/style/brightness,
   silence fallback.

---

## 8. Risks and open questions

- **Frame rate**: unknown how fast the panel accepts rhythm writes. Start at 20 fps
  with write-without-response if offered; drop to 10 fps if it lags or disconnects.
- **Echo Base pins and IO expander**: confirm from M5's docs; the mic should work
  without enabling the speaker amp.
- **Panel password**: if one is set in the phone app the panel may ignore commands
  (ipixel-ble only reads the flag). Clear it in the app for now.
- **RAM**: no PSRAM, so keep FFT buffers static (~8 KB) and NimBLE at one connection.
- **BLE + audio on one chip**: fine on ESP32-S3; keep audio on core 1, no Wi-Fi.
