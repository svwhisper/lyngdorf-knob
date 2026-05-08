# lyngdorf-knob session notes
_Last updated: 2026-05-08 — knob is fully functional; what follows is "more changes" work_

## Device
Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (SH8601 QSPI 360×360 round LCD)
- Local repo: `~/Developer/lyngdorf-knob`
- ESP-IDF 5.2.3 at `~/esp/esp-idf`
- Toolchain PATH (manual, needed for clean builds):
  `/Users/dw/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin`
- Python env: `~/.espressif/python_env/idf5.2_py3.14_env/bin/python`
- Build/flash: `idf.py build` / `idf.py -p /dev/cu.usbmodem* flash monitor`
- Exit serial monitor: `Ctrl+]`
- Two-line build invocation that handles PATH:
  ```
  export PATH="/Users/dw/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin:$PATH" && cd ~/Developer/lyngdorf-knob && ~/.espressif/python_env/idf5.2_py3.14_env/bin/python ~/esp/esp-idf/tools/idf.py -p /dev/cu.usbmodem* flash monitor
  ```

## Status (everything working)

| Feature | Status |
|---|---|
| Display (SH8601 QSPI, 50 kHz backlight PWM, no banding) | ✅ |
| Volume control (encoder → !VOLCH → amp; live, no lag) | ✅ |
| Volume sync from remote / app (5 s poll) | ✅ |
| Mute via tap (single tap = mute) | ✅ |
| Mute sync from remote / app | ✅ |
| Track metadata via amp HTTP API at port 8080 | ✅ |
| Web config (WiFi, amp IP, vol step, dim, sleep, haptic, meta poll) | ✅ |
| Power management (dim/sleep timers) | ✅ |
| Haptic (default OFF — knob has mechanical detents) | ✅ |
| WiFi STA + AP fallback for first-time setup | ✅ |

## Architecture (current)

Three FreeRTOS tasks under FreeRTOS, all sharing g_state under g_state_mutex
and reading/writing LVGL under g_lvgl_mutex:

| Task | Core | Pri | Stack | Purpose |
|---|---:|---:|---:|---|
| `ui` | 1 | 3 | 20 KB | LVGL render loop + encoder/touch event drain |
| `net` | 0 | 2 | 12 KB | Lyngdorf RIO TCP (port 84): VOL/MUTE poll & cmds |
| `metadata` | 0 | 1 | 8 KB | HTTP/JSON poll of amp:8080/api/getData every N s |

The encoder ISR-style timer (3 ms) accumulates clicks into atomic `s_delta`;
ui_task drains and posts CMD_VOL_CHANGE / CMD_MUTE_TOGGLE onto `g_cmd_queue`;
net_task blocks on the queue with a 20 ms timeout, coalesces queued
vol-deltas into one !VOLCH(N), and runs periodic !VOL? / !MUTE? polls every
5 s. metadata_task is fully separate so its HTTP fetch (~100–500 ms) never
delays encoder dispatch.

## Key file map

| File | Notes |
|---|---|
| `main/main.c` | app_main; spawns ui_task, net_task; calls metadata_init |
| `main/encoder.c` | 3 ms GPIO poll; switch-style decode (NOT quadrature) |
| `main/touch.c` | CST816D I2C; tap = mute (no double-tap) |
| `main/lyngdorf.c` | RIO TCP socket, !VOL/!MUTE only |
| `main/metadata.c` | esp_http_client + cJSON; HTTP API on port 8080 |
| `main/ui.c` | LVGL widgets; 3 centered text lines, vol overlay |
| `main/display.c` | SH8601 init, ISR flush callback |
| `main/esp_lcd_sh8601.[ch]` | Local SH8601 driver (from Waveshare demo) |
| `main/web_server.c` | http://device-ip/ config form |
| `main/app_config.[ch]` | NVS keys, defaults, shared g_state, mutexes, queue |
| `main/wifi_manager.c` | STA + AP fallback |
| `main/power.c` | dim/sleep timers |
| `main/haptic.c` | DRV2605 (off by default) |

## Important hardware quirks (don't relearn these)

- **Display chip is SH8601, not ST77916.** Use the local `esp_lcd_sh8601`
  driver with Waveshare's exact 180-cmd init sequence. ST77916 init also
  partially works (display lights up) but pixels are all hash.
- **Encoder is NOT a quadrature encoder.** It's two independent switches:
  rest at 0b11; one direction pulses A low (0b01 → back to 0b11), the
  other pulses B low (0b10 → 0b11). Don't try to gray-code-decode it.
- **Backlight needs PWM ≥ ~25 kHz.** 5 kHz aliases against the LCD scan
  and produces visible horizontal beat bands. Currently 50 kHz.
- **Lyngdorf mute syntax is `!MUTE(ON)` / `!MUTE(OFF)`** — not
  `!MUTE` / `!UNMUTE` and not `!MUTEON` / `!MUTEOFF`. Both query and
  set commands use this form.
- **Lyngdorf does not expose UPnP.** Don't reintroduce SSDP discovery.
  Use the HTTP/JSON API on port 8080 for track info.
- **Amp's HTTP API uses chunked transfer encoding.** Don't trust
  `Content-Length` from `esp_http_client_fetch_headers`; read until EOF.
- **JSON response shape**: top-level array; payload is the first array
  element that's an object containing a `trackRoles` key. There is no
  `"player:player/data"` marker string.

## Git state
- `main` is **2 commits ahead of `origin/main`** — not pushed.
- Latest: `8b9185b` "Working knob: encoder fixes, mute, metadata via amp HTTP API, UI polish"
- Push when the user explicitly asks. Iterate locally otherwise.

## Open / next-up work
User said "we have more changes". Awaiting their next ask. Potentially:
- Album-art / icon URL from the API payload
- Long-poll via `/api/pollQueue` for instant metadata updates instead of 3 s polling
- Source-name / sample-rate display
- Different layout iterations
- Other features
