# lyngdorf-knob debug session notes
_Last updated: 2026-05-08 (display fixed)_

## Device
Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (SH8601 QSPI 360×360 round LCD)
- Local repo: `~/Developer/lyngdorf-knob`
- ESP-IDF 5.2.3 at `~/esp/esp-idf`
- Toolchain (manual PATH for clean builds):
  `/Users/dw/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin`
- Python env: `~/.espressif/python_env/idf5.2_py3.14_env/bin/python`
- Build: `idf.py build` (after sourcing PATH); flash: `idf.py -p /dev/cu.usbmodem* flash monitor`
- Exit monitor: `Ctrl+]`

---

## Status

| Feature | Status |
|---|---|
| Display (image quality) | ✅ Working — clean rendering |
| Volume control (encoder → Lyngdorf) | ❌ Not yet tested with working display |
| UPnP metadata | ❌ SSDP discovery fails on this network |
| Web config portal | ✅ Working |
| Haptic | ✅ Working (default OFF — knob has mechanical detents) |

---

## What was wrong with the display (resolved)

Three independent bugs stacked on top of each other:

1. **Wrong panel driver** — The ESP-IDF community component `esp_lcd_st77916` was
   close enough for the display to *initialise* (backlight on, some commands
   accepted) but the pixel-data path was completely wrong, producing total hash.
   The panel is actually **SH8601** (Waveshare's official Arduino demo confirmed
   this). Solution: dropped `esp_lcd_st77916`, added local component
   `main/esp_lcd_sh8601.[ch]` (extracted verbatim from Waveshare's
   `08_LVGL_Test` demo) with their full panel-tuned init sequence.

2. **Backlight PWM aliasing with LCD scan** — Backlight was driven by LEDC at
   5 kHz, which beats against the panel's ~60 Hz scan and produces a faint
   horizontal band of vertical stripes visible on solid colours. Solution:
   raised PWM to 50 kHz (well above any visible aliasing).

3. **AMOLED rounder cb on an LCD** — While following Waveshare's demo I added
   an `lvgl_rounder_cb` that aligned dirty areas to 2-pixel blocks (an AMOLED
   GRAM constraint). This panel is an LCD, so the rounder was unnecessary
   overhead and forced larger redraws than needed. Removed.

Other tweaks that contributed:
- LVGL draw buffers now allocated with `heap_caps_malloc(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)` so the SPI driver doesn't have to bounce-buffer them on every flush
- SPI clock currently 20 MHz (was 40 MHz). Could be raised back to 40 if you want, but 20 is plenty fast for this UI and conservative against signal-integrity issues.
- `on_color_trans_done` ISR signals `lv_disp_flush_ready` — async flush, no more hash from buffer-still-in-DMA reuse

### Verified at the panel level via 4-step diagnostic
Before final cleanup, a pre-LVGL diagnostic flashed:
1. solid black (`0x0000`)  → all black ✓
2. solid white (`0xFFFF`)  → all white ✓
3. solid `0x00F8`           → all red ✓ (confirms `LV_COLOR_16_SWAP=y` is correct)
4. solid `0xF800`           → all blue ✓

This diagnostic has now been **removed** from `display.c` since the display is
confirmed working. The `s_lvgl_ready` guard and `s_diag_sem` semaphore that
supported the diagnostic phase have also been removed.

---

## Open bug #1: Volume control not yet verified

Encoder rotation triggers haptic (when enabled). With the display now working,
the volume value on screen should change with rotation. Need to test on device
to confirm.

If it's not working, the most likely cause is that `net_task` blocks on SSDP
discovery (`upnp_init()`) for ~2 sec every 3 sec, starving the command queue.

### To diagnose if it's still broken
Add `ESP_LOGI` in `main.c`'s `net_task` after `lyngdorf_vol_delta(cmd.param)` to
confirm commands are being dispatched. Also add a log in `lyngdorf.c` after the
TCP send to confirm bytes reach the wire.

---

## Open bug #2: UPnP SSDP discovery

Lyngdorf TDAI-3400 does not respond to SSDP M-SEARCH on the user's network
(192.168.3.x). Sonos at 192.168.3.211 was responding earlier; that's filtered
out by the amp-IP filter in `upnp.c`.

### Workaround
Enter UPnP URL manually in the web config UI (`http://<device-ip>/`).
Field: "UPnP URL". Once stored, `upnp_init()` returns immediately without SSDP.

### To find the URL
Browse `http://192.168.3.243:PORT/` — try ports 80, 2869, 49152–49155 — and
look for an AVTransport `controlURL` in the device-description XML.

---

## Key files

| File | Notes |
|---|---|
| `main/display.c` | SH8601 init, LVGL setup, ISR flush callback |
| `main/esp_lcd_sh8601.[ch]` | Local SH8601 driver (extracted from Waveshare demo) |
| `main/app_config.h` | Pin assignments, NVS keys, defaults; `LCD_SPI_HZ = 20 MHz` |
| `main/upnp.c` | SSDP discovery, UPnP polling, play/pause |
| `main/lyngdorf.c` | TCP socket to amp:84, vol/mute commands |
| `main/main.c` | FreeRTOS tasks: ui_task (core 1, pri 3), net_task (core 0, pri 2) |
| `main/web_server.c` | Config portal at http://device-ip/ |
| `main/idf_component.yml` | Removed `esp_lcd_st77916` dependency |
| `main/CMakeLists.txt` | Added `esp_lcd_sh8601.c` to SRCS |
| `sdkconfig.defaults` | `LV_COLOR_16_SWAP=y`, `LV_COLOR_DEPTH_16=y` (verified correct) |

## Git state
Display fix committed once display was verified working. Volume / UPnP fixes
pending — to be committed separately once they're tested on device.
