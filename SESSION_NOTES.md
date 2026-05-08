# lyngdorf-knob session notes
_Last updated: 2026-05-09 — power tiers + browser installer landed_

## Device
Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (SH8601 QSPI 360×360 round LCD)
- Local repo: `~/Developer/lyngdorf-knob`
- ESP-IDF 5.2.3 at `~/esp/esp-idf`
- Toolchain PATH (manual, needed for clean builds):
  `/Users/dw/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin`
- Python env: `~/.espressif/python_env/idf5.2_py3.14_env/bin/python`
- Two-line build/flash invocation (handles PATH):
  ```
  export PATH="/Users/dw/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin:$PATH" && cd ~/Developer/lyngdorf-knob && ~/.espressif/python_env/idf5.2_py3.14_env/bin/python ~/esp/esp-idf/tools/idf.py -p /dev/cu.usbmodem* flash monitor
  ```
- Exit serial monitor: `Ctrl+]`

## Status (everything working)

| Feature | Status |
|---|---|
| Display (SH8601 QSPI, 50 kHz backlight PWM, no banding) | ✅ |
| Volume control (encoder → !VOLCH → amp; sub-50 ms latency) | ✅ |
| Volume + mute sync from remote / app (5 s RIO poll) | ✅ |
| Mute via tap on speaker icon (always-visible glyph swap) | ✅ |
| Play/pause via tap on play icon (HTTP toggle + optimistic UI) | ✅ |
| Track metadata via amp HTTP API at port 8080 (3 s poll) | ✅ |
| Numeric volume + 3-line title/artist/album, all 16pt | ✅ |
| Battery percentage (ADC1 ch0, 10 s poll) | ✅ |
| Boot splash with QR code linking to repo | ✅ |
| WiFi status during boot ("Connecting to <ssid>...", "as <ip>") | ✅ |
| Haptic on icon tap (always on) | ✅ |
| Web config (WiFi / amp IP / vol step / dim / sleep / meta poll) | ✅ |
| **Tier 1 Active** (full power) | ✅ |
| **Tier 2 Idle** (panel + backlight off, modem-sleep, encoder ISR-driven) | ✅ |
| **Tier 3 Deep sleep** (~10 µA, wake on touch INT or encoder GPIO) | ✅ |
| Light-sleep + tickless idle (CPU clock-gates between events) | ✅ |
| ESP Web Tools installer scaffolding | ✅ committed; awaits GitHub Pages enable + v1.0.0 tag |

## Power architecture (3 tiers)

| Tier | Trigger | Behaviour |
|---|---|---|
| **1 Active** | any touch / encoder event | full UI, WiFi `MIN_MODEM` |
| **2 Idle** | no interaction for `sleep_secs` (120 s) | panel + backlight off, WiFi `MAX_MODEM`, Lyngdorf poll 5→30 s, metadata poll 3→60 s |
| **3 Deep sleep** | in Tier 2 ≥ 5 min **AND** amp not playing ≥ 2 min | WiFi disconnected, chip ~10 µA, wakes on TP_INT (GPIO 9) / ENC_A (GPIO 8) / ENC_B (GPIO 7) low |

Plus always-on light sleep via `esp_pm_configure` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE` so the CPU clock-gates whenever tasks are blocked.

## Three FreeRTOS tasks

| Task | Core | Pri | Purpose |
|---|---:|---:|---|
| `ui` | 1 | 3 | LVGL render loop, encoder + touch event drain, calls `power_tick()` |
| `net` | 0 | 2 | Lyngdorf RIO TCP (port 84): VOL/MUTE poll & cmds |
| `metadata` | 0 | 1 | HTTP/JSON poll of amp:8080/api/getData |

Encoder is now interrupt-driven (negedge on GPIO 7 + 8 with 5 ms per-pin debounce); no polling timer.

## Key file map

| File | Notes |
|---|---|
| `main/main.c` | app_main; spawns ui_task, net_task; calls battery_init, metadata_init; checks wake cause |
| `main/encoder.c` | GPIO ISR-driven, switch-style decode (NOT quadrature) |
| `main/touch.c` | CST816D I2C + INT pin config (GPIO 9), TP_RST pulse, region-aware tap dispatch |
| `main/lyngdorf.c` | RIO TCP socket, !VOL/!MUTE only |
| `main/metadata.c` | esp_http_client + cJSON; HTTP GET /api/getData; play/pause via /api/setData |
| `main/ui.c` | LVGL widgets; splash w/ QR code (skip on deep-sleep wake); 3 text lines + battery + status |
| `main/display.c` | SH8601 init, ISR flush callback, 50 kHz backlight PWM |
| `main/esp_lcd_sh8601.[ch]` | Local SH8601 driver |
| `main/power.c/h` | dim → panel-sleep → deep-sleep state machine; IRAM-safe activity signal |
| `main/battery.c/h` | ADC1 ch0 oneshot + curve-fit cali, 10 s periodic poll |
| `main/web_server.c` | http://device-ip/ config form |
| `main/wifi_manager.c` | STA + AP fallback; pushes ssid/ip to UI |
| `main/app_config.[ch]` | NVS keys, defaults, shared g_state, mutexes, queue |
| `docs/index.html` | ESP Web Tools install page (cross-platform Win/Mac/Linux) |
| `docs/manifest.json` | Manifest pointing at GitHub Releases binary URL |
| `.github/workflows/release.yml` | Auto-build + release on `git push --tags v*` |

## Important hardware quirks (don't relearn these)

- **Display chip is SH8601, not ST77916** — local driver in `main/esp_lcd_sh8601.[ch]`, Waveshare's exact 180-cmd init sequence.
- **Encoder is switch-style (NOT quadrature)** — rest at `0b11`, one direction pulses A low, the other pulses B low. ENC_B *can* rest at low in some mechanical positions; see deep-sleep gotcha below.
- **Backlight PWM ≥ 25 kHz** — 5 kHz aliases against the LCD scan and produces visible horizontal bands. Currently 50 kHz.
- **Lyngdorf mute syntax: `!MUTE(ON)` / `!MUTE(OFF)`** — not `!MUTE/!UNMUTE` and not `!MUTEON/!MUTEOFF`. Both query and set use parens.
- **Lyngdorf does not expose UPnP** — use the HTTP/JSON API on port 8080 for track info.
- **Amp's HTTP server uses chunked transfer encoding** — don't trust `Content-Length`; read until EOF.
- **JSON payload shape**: top-level array; the payload is the first array element that's an object containing a `trackRoles` key. No `"player:player/data"` marker string.
- **CST816D `TP_INT` is on GPIO 9, `TP_RST` on GPIO 10** — INT pulses low on touch (configured via REG 0xFA = 0x70). Used as deep-sleep wake source.
- **Battery sense: ADC1 ch0 (GPIO 1) via 10 kΩ / 10 kΩ divider (2:1)** — code multiplies by 2. Source rail is "5V" (battery or USB), so reads ~5 V on USB which clamps to 100 %.

## Deep-sleep gotchas (every one of these caught us)

1. **Tickless idle is required for light-sleep to engage.** `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` and `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3` in sdkconfig. Without it, `esp_pm_configure` returns `ESP_ERR_NOT_SUPPORTED` for `light_sleep_enable=true`.
2. **Wake-pin pull-ups must be set up via the RTC GPIO API** (`rtc_gpio_pullup_en`) — the regular `gpio_pullup_en` doesn't survive into deep sleep. Without this, floating wake pins trip immediate wake.
3. **PM-related wake sources persist into deep sleep.** Call `esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL)` *before* `esp_sleep_enable_ext1_wakeup()`, otherwise a leftover tickless-idle resume timer fires immediately and the chip wakes within ms (`wake_cause=4` = TIMER).
4. **Encoder pins can rest at low.** When the user clicks the knob, the mechanical state can leave one switch closed. Always read the actual GPIO levels just before deep sleep and exclude any low pin from the wake mask — otherwise we'd never sleep. `power.c::enter_deep_sleep` does this.
5. **CST816D may hold TP_INT low if there's an unread touch event.** Drain its data registers (read 7 bytes from reg 0x00) just before sleep to release INT.
6. **USB-CDC takes ~700 ms to renumerate after deep-sleep wake.** Without an early `vTaskDelay`, the post-wake `wake_cause=...` log line is dropped by the monitor. Currently 300 ms — bump to 1500 ms if you want guaranteed log visibility during development.

## Browser-based installer (ESP Web Tools)

Scaffolding is in place but not yet "live". Two one-time steps:

1. **Enable GitHub Pages**: settings → Pages → Source = `main` / `/docs`.
2. **Tag a release**: `git tag v1.0.0 && git push origin v1.0.0`. The Actions workflow at `.github/workflows/release.yml` will build, attach `lyngdorf-knob-merged.bin` to the release, and bump `docs/manifest.json` to point at it.

After both, `https://svwhisper.github.io/lyngdorf-knob/` is the public install page. Cross-platform: works in Chromium-based browsers on Windows / macOS / Linux.

## Git state
- All work pushed to `origin/main`.
- Latest commit: deep-sleep wake fixes + production timings restored.
- `v1.0.0` not yet tagged — pending GitHub Pages enable.

## Possible future work
- Album art (JPEG decode + LVGL image widget; URL is in `trackRoles.icon`)
- Long-poll via `/api/pollQueue` for instant metadata updates
- Source / sample-rate / bitrate display
- Tune deep-sleep entry on amp source change (e.g. enter sleep 30 s after switching to non-streaming input)
