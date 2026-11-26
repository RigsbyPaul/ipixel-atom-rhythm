# iPixel Atom Rhythm

Music-reactive bars on an **iPixel Color Bluetooth LED matrix**, driven by an
**M5Stack AtomS3 Lite** with the **Atomic Echo Base** microphone.

The Atom listens to the room, splits the sound into frequency bands and streams
bar heights to the panel over Bluetooth LE 20 times a second, or draws a
whole-panel disco picture from the music. No phone, hub or Wi-Fi is needed.

```
Echo Base mic --I2S--> AtomS3 Lite (ESP32-S3) --BLE--> iPixel LED panel
                       FFT -> bands -> bar heights
```

The panel protocol comes from [ipixel-ble](https://github.com/RigsbyPaul/ipixel-ble),
a Home Assistant integration for the same panels.

## Contents

* [Hardware](#hardware)
* [Set up ESP-IDF](#set-up-esp-idf)
* [Build and flash](#build-and-flash)
* [Using it](#using-it)
* [How it works](#how-it-works)
* [Tuning](#tuning)
* [Limitations](#limitations)
* [Project layout](#project-layout)

## Hardware

| Part | Notes |
|---|---|
| [AtomS3 Lite](https://docs.m5stack.com/en/core/AtomS3%20Lite) | ESP32-S3, USB-C, button on G41, RGB LED on G35 |
| [Atomic Echo Base](https://docs.m5stack.com/en/atom/Atomic%20Echo%20Base) | ES8311 codec + MEMS mic; the Atom plugs on top |
| iPixel Color LED panel | Advertises as `LED_BLE_…`; tested on a 48 x 24 panel (type 137) and a 192 x 32 panel (type 143) |

Pins used (from M5's Echo Base library): I2C SDA G38 / SCL G39, I2S BCK G8,
WS G6, mic data in G7. The ES8311 is at I2C `0x18` and the PI4IOE5V6408 IO
expander at `0x43`.

**The panel accepts one Bluetooth connection at a time.** Close the iPixel Color
phone app, and turn off any Home Assistant integration for the panel, before
using the Atom.

## Set up ESP-IDF

This project uses **ESP-IDF v6.0** (v6.0.1 tested).

1. Download the ESP-IDF installer (EIM) from
   <https://dl.espressif.com/dl/esp-idf/> and install ESP-IDF v6.0.x with the
   ESP32-S3 target.
2. Open a PowerShell window and activate ESP-IDF. With the default EIM install:
   ```powershell
   . C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1
   ```
3. Check it works:
   ```powershell
   idf.py --version
   ```

## Build and flash

Plug the Atom in over USB-C and find its COM port in Device Manager (it shows
as *USB JTAG/serial debug unit*). Then:

```powershell
cd ipixel-atom-rhythm
idf.py set-target esp32s3
idf.py -p COM14 flash monitor
```

Replace `COM14` with your port. The first build downloads the `led_strip`
component. Leave the monitor with `Ctrl+]`.

If flashing fails, hold the Atom's button while plugging it in to force
download mode.

## Using it

Power the Atom. It finds the panel, connects, and starts drawing bars as soon
as there is sound.

**Button**

| Press | Action |
|---|---|
| Short | Next style (0 to 6, then back to 0) |
| Long (0.7 s) | Next brightness: 20, 40, 60, 80, 100 % |

Style and brightness are saved and come back after a power cycle.

**Styles**

| Style | LED colour | What the panel shows |
|---|---|---|
| 0 | green | The panel's own music screen, style 0 |
| 1 | cyan | The panel's own music screen, style 1 |
| 2 | magenta | The panel's own music screen, style 2 |
| 3 | orange | The panel's own music screen, style 3 |
| 4 | red | The panel's own music screen, style 4 |
| 5 | purple | **Self-drawn bars**: width / 3 bars (16 on a 48-wide panel), full panel height, green / yellow / red; silence is a 1-pixel green line along the bottom |
| 6 | lime, then the panel's colour | **Live disco**: whole-panel pictures of the music, about 2.4 a second (see below) |

The panel's own music screen always uses its built-in layout. On a 48 x 24
panel that is 8 bars shown twice, however many levels are sent. Style 5 draws
every pixel itself, so the number of bars follows the panel's width.

**Live disco (style 6)** draws the whole panel from the music:

* 11 frequency beams rising from the bottom, bass left to treble right, each
  showing its band's loudest moment since the last picture;
* a background that brightens on each beat and steps 60 degrees round the
  colour wheel on a beat (at most every 1.5 s); the beams take the opposite
  colour;
* a few white sparkles on strong treble.

Each band is measured above its own noise floor, so silence shows empty beams
over a dark background. The panel takes a new picture only about 2.4 times a
second, so the disco moves in steps.

**Status LED**

| LED | Meaning |
|---|---|
| Blinking blue | Scanning for a panel |
| Yellow | Connecting |
| Steady colour | Connected; the colour shows the current style (in style 6, the panel's average colour) |
| White flash | Button press received |

**Silence:** in styles 0-4, after 10 seconds of quiet the Atom stops sending, so
the panel goes back to whatever it normally shows. Bars come back as soon as
there is sound. Styles 5 and 6 keep drawing: style 5 shows its 1-pixel line,
style 6 empty beams.

**Reconnecting:** the panel's address is saved the first time it is found. Later
boots connect straight to it. After 3 failed attempts in a row the Atom scans
again, so swapping panels works.

## How it works

**Audio.** The ES8311 codec runs as an I2S slave clocked from the bit clock (the
Echo Base has no MCLK line). The mic is read at 16 kHz, 32-bit, in 512-sample
blocks (32 ms). Gain is 30 dB analog PGA plus 24 dB digital.

**Bands.** Each block is Hann-windowed and run through a 512-point FFT. Bins
are grouped into log-spaced bands from about 60 Hz to 8 kHz: 11 bands for the
panel's music screen, width / 3 bands for self-draw. Then:

* treble bands get a small boost, since music has less energy up high
  (16.5 dB across the bands for the music screen, 10 dB for style 5),
* an automatic gain follows the loudest recent sound and the noise floor, so
  quiet and loud rooms both fill the bars,
* bars rise instantly and fall over about half a second.

**Panel protocol.** Commands are written to the `fa02` characteristic of service
`fa01`. Replies arrive as notifications on `fa03`. All values are little-endian
and every command starts with its 2-byte length.

| Command | Bytes |
|---|---|
| Music screen (11 bars, 0-15) | `10 00 01 02 <style 0-4> <11 levels>` |
| Enter / leave live drawing | `05 00 04 01 <1 = enter on clear screen, 0 = leave>` |
| Draw pixels (up to 200) | `<len> 05 01 00 R G B x0 y0 x1 y1 …` |
| Live frame block (raw RGB) | `<len> 00 00 <0 first / 2 after> <total, 4 bytes = w x h x 3> <RGB…>` |
| Brightness 0-100 | `05 00 04 80 <level>` |
| Power on / off | `05 00 07 01 <1/0>` |
| Time sync (reply has the panel type) | `08 00 01 80 hh mm ss 00` |

**Self-draw.** Each frame only sends pixels that changed, at most 240 per
frame; changes that don't fit follow in the next frames. Pixel commands carry
up to 60 pixels and are written *with response*, so the panel confirms each
one before the next is sent, as ipixel-ble does. Fire-and-forget writes were
fine on a 48 x 24 panel but a 192 x 32 panel dropped most of them. The
trade-off is speed: self-draw runs at about 5-6 frames a second on a 192 x 32
panel, set by how fast the panel confirms. Each frame also rewrites one bar's
whole column when there is room, cycling through the bars, to clear any stale
pixel. If a write fails, only the bars involved are redrawn.

**Live disco frames.** Style 6 sends each picture as a *live frame*: raw RGB,
3 bytes a pixel row by row from the top, in blocks of up to 12288 bytes (two
blocks for 192 x 32). The total-length field is width x height x 3, as the
iPixel Color app sets it. The panel answers each block, `05 00 00 00 01` for
"next" and `05 00 00 00 02` at the end of a frame, and the Atom waits for each
answer. Before the first frame the panel is put into live drawing mode, as the
app's DIY image screen does; without it the panel shows its Bluetooth icon over
every frame. Entering style 6 first sends one music-screen frame and leaves
drawing mode, so the panel lets go of a saved program it may be playing.

These details come from the iPixel Color app: on wide panels it sends raw RGB
this way from its DIY image screen. A 192 x 32 panel answered JPEG live frames
too, but never showed them.

The panel takes about 2.4 frames a second (18 KB each). Blocks are written
without response but paced: when NimBLE runs out of buffers the Atom waits
instead of failing. Beats are detected in the audio task on every 32 ms block,
so beats and peaks between pictures are not lost.

**Panel size.** After connecting, the Atom turns on notifications and sends a
time sync. The reply's device-type byte gives the panel size (for example
137 = 48 x 24, 143 = 192 x 32). Some panels don't always answer, so the Atom
waits up to 6 s, asking every 2 s, before sending anything else. Each size a
panel reports is saved with its address; if a panel doesn't answer, the saved
size is used. A panel that has never reported its size shows the music screen
in style 5.

## Tuning

Settings are `#define`s at the top of these files:

| File | Setting | Effect |
|---|---|---|
| `main/bands.c` | `MAX_SPAN_DB` | How much quieter than the loudest recent sound a bar can be and still show |
| `main/bands.c` | `PEAK_FALL_DB` | How fast bars get taller again after a loud part |
| `main/bands.c` | `FALLS_PER_FULL` | Frames for a full bar to drop to zero |
| `main/bands.c` | `TOTAL_TILT_DB` | Treble boost across the music-screen bars |
| `main/main.c` | `DRAWN_TILT_DB` | Treble boost across the style 5 bars |
| `main/fx.c` | `GATE`, `FULL` | Disco: how far above its noise floor a band must rise to show, and to fill a beam |
| `main/fx.c` | beat test in `fx_listen` | Disco beat sensitivity and the shortest gap between beats |
| `main/disco.c` | `HUE_HOLD_S`, colours | Disco background colour steps and the look of the picture |
| `main/bands.c` | `LOW_HZ`, `HIGH_HZ` | Frequency range |
| `main/audio.c` | `MIC_PGA`, `MIC_DIGITAL_GAIN` | Microphone gain |
| `main/draw.c` | `RGB`, `zone()` | Self-draw colours and where they change |
| `main/draw.c` | `MAX_POINTS`, `DRAW_FRAME_POINTS` | Pixels per command and per frame in self-draw |
| `main/main.c` | `FRAME_MS` | Frame interval (50 ms = 20 fps) |
| `main/main.c` | `SILENCE_S` | Seconds of quiet before styles 0-4 pause |

## Limitations

* **The panel's clock is set to 00:00 on every connect.** The panel reports its
  size only in reply to a time sync, and the Atom has no clock. Set the time
  again from the phone app or Home Assistant if you use the panel's clock.
* Panels wider than 255 pixels (types 144-147) can't be self-drawn: pixel
  coordinates are single bytes. The panel's own music screen still works.
* Self-draw is slower on big panels (about 5-6 frames a second on 192 x 32)
  because each pixel command waits for the panel to confirm it.
* The live disco updates only about 2.4 times a second on a 192 x 32 panel;
  that is how fast the panel takes 18 KB pictures.
* If the panel has a password set in the phone app, it may ignore commands.
* The code is written for the AtomS3 Lite + Echo Base pins. Other ESP32-S3
  boards need the pins in `main/audio.c` and `main/ui.c` changed.

## Project layout

```
main/
  main.c          tasks: audio -> levels, send loop, button handling, silence pause
  audio.c         Echo Base: IO expander, ES8311 setup over I2C, I2S mic input
  bands.c         FFT, log-spaced bands, AGC and bar fall
  ipixel.c        NimBLE central: scan, connect, discover, notifications, panel size
  ipixel_proto.h  music-screen, brightness and power commands
  draw.c          self-draw: live drawing mode, changed-pixel updates, column refresh
  fx.c            disco music analysis: bands above their noise floor, beats, peaks
  disco.c         disco pictures: beams, beat-coloured background, sparkles
  live.c          live frames: raw RGB blocks with the panel's answers
  settings.c      NVS: style, brightness, panel address, panel size per address
  ui.c            button and status LED
plan.md           original plan and progress notes
```
