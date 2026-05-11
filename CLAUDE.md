# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Read first

**`SESSION_NOTES.md`** is the live engineering log. Always read it before non-trivial work — it carries the current build/flash invocations for this machine, hardware quirks discovered the hard way, status of in-progress investigations, and a list of "deep-sleep gotchas" that you must not reintroduce.

For machine-specific build commands and personal overrides, see `CLAUDE.local.md` (gitignored).

There are no unit tests; verification is on-device. Build with `idf.py build` after `source $IDF_PATH/export.sh`.

## Architecture

ESP-IDF 5.2 firmware for the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** turning it into a controller for Lyngdorf amplifiers.

### Three FreeRTOS tasks

| Task | Core | Pri | Role |
|---|---:|---:|---|
| `ui_task` (`main.c`) | 1 | 3 | LVGL render loop; drains encoder + touch event queues; calls `power_tick()` |
| `net_task` (`main.c`) | 0 | 2 | Lyngdorf RIO TCP socket on port 84: `!VOLCH` / `!MUTE` poll & dispatch |
| `metadata_task` (`metadata.c`) | 0 | 1 | HTTP/JSON poll of amp:8080/api/getData every 3 s for now-playing |

Inter-task communication is via `g_cmd_queue` (encoder/touch → net) and shared `g_state` (guarded by `g_state_mutex`). All LVGL calls must be inside `g_lvgl_mutex`.

### Power state machine (`power.c`)

Three tiers gated by user idle time + amp playback state:
1. **Active** — full UI, WiFi MIN_MODEM, 5 s amp poll
2. **Idle** — panel + backlight off, WiFi MAX_MODEM, 30 s amp poll (after `sleep_secs`, default 120)
3. **Deep sleep** — WiFi off, panel SLPIN, peripherals shut down (after additional `DEEP_SLEEP_AFTER_S` = 300 s **and** amp not-playing for `PAUSED_GRACE_S` = 120 s); wakes only on encoder pin (GPIO 7/8) via EXT1

Deep-sleep entry sequence in `enter_deep_sleep()` is order-sensitive — read `SESSION_NOTES.md` "Deep-sleep gotchas" before modifying it. Several past changes there have caused panic loops.

### Dual-MCU board

The board has **two MCUs**: ESP32-S3 (this firmware) and a secondary ESP32-U4WDH that ships running Waveshare's stock audio firmware drawing ~50 mA continuously. The companion project [`lyngdorf-secondary-sleep`](https://github.com/svwhisper/lyngdorf-secondary-sleep) replaces stock with a do-nothing `esp_deep_sleep_start()` image. The secondary's flashing port is the same USB-C connector — **flip the cable 180°** to switch which MCU the host talks to (S3 = `/dev/cu.usbmodem*` native USB-CDC; secondary = `/dev/cu.usbserial-*` via on-board UART bridge).

### Hardware quirks (compact)

Full list with explanations is in `README.md` and `SESSION_NOTES.md`. The ones most likely to bite during code changes:
- Display driver is **SH8601** (local `esp_lcd_sh8601.[ch]`), not ST77916.
- Encoder is **switch-style, not quadrature**: rests at `0b11`, one direction pulses A low, the other pulses B low.
- Lyngdorf mute syntax is **`!MUTE(ON)` / `!MUTE(OFF)`**, parenthesised.
- The amp's HTTP server uses **chunked transfer encoding**; don't trust `Content-Length`.
- Battery sense reads the post-charger rail, not the cell directly. Curve in `battery.c` is calibrated empirically — see comment block.

## Deployment

Releases are cut by `git tag vX.Y.Z && git push origin vX.Y.Z`. A GitHub Actions workflow at `.github/workflows/release.yml` builds the merged binary, attaches it to the GitHub Release, and updates `docs/manifest.json` so the ESP Web Tools page at `https://svwhisper.github.io/lyngdorf-knob/` picks up the new version.
