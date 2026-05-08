# lyngdorf-knob

ESP-IDF firmware for the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** board, turning it into a physical volume / play-pause / mute controller for the **Lyngdorf TDAI-3400** amplifier.

## What it does

| Interaction | Action |
|---|---|
| Rotate encoder | Volume up / down (default **1.0 dB / detent**, configurable) |
| Tap the speaker icon (left, teal) | Mute / unmute |
| Tap the play/pause icon (right, teal) | Toggle play / pause |

Volume is sent to the amp via Lyngdorf's RIO TCP protocol on port 84 with sub-50 ms latency. Mute and play/pause flip the on-screen icon optimistically and confirm via the amp's reply / next state poll. The on-screen state mirrors changes made from the Lyngdorf remote, app, or front panel within a few seconds.

## Display layout (360 × 360 round LCD)

```
                –NN.N dB        ← always-visible volume readout (top)
                Artist
                Track Title
                Album
                🔊  ▶          ← tappable mute & play/pause icons
                status line     ← WiFi / amp connection
```

- **Volume arc** — 270° teal ring around the perimeter, dims when muted
- **Numeric volume** — top of display, always visible, updates live during rotation
- **Track info** — artist / title / album, three centered lines (Montserrat 16pt white), pulled from the amp's HTTP/JSON API every 3 s
- **Icons** — speaker (🔊 teal / 🔇 red) and play/pause (▶ / ⏸ teal), 32 pt, tap to toggle
- **Idle** — display dims after `dim_secs` of inactivity, then panel-sleeps after `sleep_secs`. Any rotation or touch wakes instantly.
- **Haptic** — DRV2605 LRA buzz on every icon tap (encoder uses its mechanical detents; no electronic haptic on rotation)

## Hardware

| Part | Detail |
|---|---|
| MCU | ESP32-S3 (240 MHz, 16 MB flash, 8 MB PSRAM) |
| Display | SH8601 360×360 round LCD over QSPI ([not ST77916 despite some docs](#hardware-quirks)) |
| Touch | CST816D capacitive over I2C (port 0) |
| Encoder | Switch-style, two GPIOs ([not standard quadrature](#hardware-quirks)) |
| Haptic | DRV2605 LRA driver over I2C (shared bus with touch) |
| WiFi | 2.4 GHz 802.11 b/g/n |

### Pin assignments

| Function | GPIO | | Function | GPIO |
|---|---:|---|---|---:|
| LCD CLK | 13 | | Touch SDA | 11 |
| LCD CS | 14 | | Touch SCL | 12 |
| LCD D0..D3 | 15..18 | | Encoder A | 8 |
| LCD RST | 21 | | Encoder B | 7 |
| LCD Backlight | 47 | | | |

## Architecture

### Three FreeRTOS tasks

| Task | Core | Pri | Stack | Purpose |
|---|---:|---:|---:|---|
| `ui` | 1 | 3 | 20 KB | LVGL render loop, encoder + touch event drain |
| `net` | 0 | 2 | 12 KB | Lyngdorf RIO TCP (port 84): VOL/MUTE poll & cmds |
| `metadata` | 0 | 1 | 8 KB | HTTP/JSON poll of amp:8080/api/getData |

The metadata task is deliberately on its own task at lower priority than `net_task`, so the HTTP fetch (~100–500 ms) never delays encoder commands.

### Files

```
main/main.c            — app_main(), ui_task, net_task
main/display.c/h       — SH8601 QSPI init, LVGL flush callback, backlight PWM
main/esp_lcd_sh8601.c/h — local SH8601 panel driver (extracted from Waveshare demo)
main/touch.c/h         — CST816D I2C driver, region-aware tap dispatch
main/encoder.c/h       — 3 ms GPIO poll, switch-style decode → command queue
main/lyngdorf.c/h      — RIO TCP socket, !VOL?/!MUTE? poll, !VOLCH(N) / !MUTE(ON|OFF)
main/metadata.c/h      — esp_http_client + cJSON track-info fetch + play/pause toggle
main/ui.c/h            — LVGL widgets: arc, vol/artist/title/album labels, icons
main/power.c/h         — dim/sleep timers, activity signalling
main/haptic.c/h        — DRV2605 LRA driver, single-effect playback
main/wifi_manager.c/h  — STA mode with AP fallback after 5 failed retries
main/web_server.c/h    — HTTP config form at http://<device-ip>/
main/app_config.c/h    — NVS get/set, shared lk_state_t, g_state_mutex, g_cmd_queue
```

## Amp protocol

### RIO over TCP (port 84) — used for volume and mute

ASCII commands, `\r`-terminated. The amp echoes back with `#` and replies with `!`.

| TX | Effect | RX format |
|---|---|---|
| `!VOLCH(n)` | Change volume by *n* tenths of a dB | (no reply) |
| `!VOL?` | Query current volume | `!VOL(-300)` = −30.0 dB |
| `!MUTE(ON)` / `!MUTE(OFF)` | Set mute on/off | (no reply) |
| `!MUTE?` | Query mute state | `!MUTE(ON)` or `!MUTE(OFF)` |

The firmware sends a single coalesced `!VOLCH(N)` per net_task drain cycle, summing all queued encoder ticks — so a fast rotation doesn't flood the amp.

### HTTP JSON API (port 8080) — used for track info and play/pause

```
GET /api/getData?path=player:player/data&roles=title,mediaData,value
GET /api/setData?path=player:player/control&roles=activate&value={"control":"pause"}
```

The first returns a JSON array; the payload object is the first element with a `trackRoles` key. The firmware extracts `state` ("playing" / "paused"), `trackRoles.title`, and `trackRoles.mediaData.metaData.{artist,album}`.

The second is a toggle — same URL whether currently playing or paused; the amp picks the right direction.

## Building

### Prerequisites
- [ESP-IDF 5.2+](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)
- Python 3.9+

### Build & flash

```bash
cd lyngdorf-knob
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodem* flash monitor
```

The component manager auto-fetches `lvgl/lvgl 8.3.x` and `espressif/esp_lcd_touch`. The SH8601 driver and cJSON are bundled (cJSON via the `json` IDF component).

### First boot

With no WiFi credentials stored, the device starts an open access point named **LyngdorfKnob**. Connect to it and visit `http://192.168.4.1` to configure:

| Field | Default | Notes |
|---|---|---|
| WiFi SSID / Password | — | Required |
| Amp IP | — | Static IP recommended (DHCP reservation is fine) |
| Vol Step | 10 (= 1.0 dB / detent) | In 0.1 dB units; range 1–50 |
| Track-info refresh | 3 s | Range 1–60 s |
| Dim display after | 30 s | 0 = never |
| Sleep display after | 120 s | 0 = never |

After saving, the device reboots and joins the configured network. The config page is then available at `http://<device-ip>/`.

## Hardware quirks

Things this firmware learned the hard way; useful if you're forking or adapting:

- **The display chip is SH8601, not ST77916.** Some references / Waveshare docs say ST77916 — the ST77916 driver partially works (display lights up) but pixels render as hash. The local `esp_lcd_sh8601` driver with Waveshare's exact 180-cmd init sequence is required.
- **The encoder is *not* a quadrature encoder.** It's two independent switches: rests at `0b11`; one direction pulses A low (`0b11→0b01→0b11`), the other pulses B low (`0b11→0b10→0b11`). Don't try to gray-code-decode it — direction is just "which line went low".
- **Backlight PWM must be ≥ ~25 kHz.** 5 kHz aliases against the LCD scan and produces visible horizontal beat bands. Currently 50 kHz.
- **Lyngdorf mute syntax is `!MUTE(ON)` / `!MUTE(OFF)`** — not `!MUTE` / `!UNMUTE` and not `!MUTEON` / `!MUTEOFF`. Both query and set commands use the parenthesised form.
- **The amp does not expose UPnP.** Don't add SSDP/AVTransport code; use the HTTP/JSON API on port 8080 instead.
- **The amp's HTTP server uses chunked transfer encoding.** `Content-Length` is absent; read until EOF.
- **JSON response shape:** top-level array; the payload is the first array element that's an object with a `trackRoles` key. There is no `"player:player/data"` marker string between the placeholder slots and the payload, despite what some references claim.

## License / acknowledgements

The local `esp_lcd_sh8601.c` / `.h` are extracted from Waveshare's official Arduino demo for this board (Espressif Apache-2.0 origin). Everything else under MIT.
