# lyngdorf-knob session notes
_Last updated: 2026-05-11 — power investigation closed at ~7 mA idle (10× improvement). Firmware is feature-complete; only hardware mods remain as a further power lever._

## Device

Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (SH8601 QSPI 360×360 round LCD) — **dual-MCU board** (ESP32-S3 + secondary ESP32-U4WDH on a shared USB-C port).

- Local repo: `~/Developer/lyngdorf-knob`
- Secondary-chip sleep firmware: `~/Developer/lyngdorf-secondary-sleep`
- ESP-IDF 5.2.3 at `~/esp/esp-idf`
- Toolchain PATH (needed because this machine's `export.sh` resolves to a non-existent py3.9 env):
  `/Users/dw/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin`
- Python env: `~/.espressif/python_env/idf5.2_py3.14_env/bin/python`

Build/flash invocation (see also `CLAUDE.local.md`):
```
export PATH="/Users/dw/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin:$PATH" && \
  ~/.espressif/python_env/idf5.2_py3.14_env/bin/python ~/esp/esp-idf/tools/idf.py \
  -C ~/Developer/lyngdorf-knob -p /dev/cu.usbmodem* flash monitor
```
Exit serial monitor: `Ctrl+]`.

## Status (everything working)

| Feature | Status |
|---|---|
| Display (SH8601 QSPI, 50 kHz backlight PWM, no banding) | ✅ |
| Volume control (encoder → !VOLCH → amp; sub-50 ms latency) | ✅ |
| Volume + mute sync from remote / app (5 s RIO poll) | ✅ |
| Mute via tap on speaker icon | ✅ |
| Play/pause via tap on play icon (HTTP toggle + optimistic UI) | ✅ |
| Track metadata via amp HTTP API at port 8080 | ✅ |
| Numeric volume + 3-line title/artist/album, all 16pt | ✅ |
| Battery percentage (ADC1 ch0, 10 s poll, piecewise Li-ion curve) | ✅ |
| Boot splash with QR code linking to repo (skipped on deep-sleep wake) | ✅ |
| WiFi status during boot ("Connecting to <ssid>...", "as <ip>") | ✅ |
| Haptic on icon tap | ✅ |
| Web config form (WiFi / amp IP / vol step / dim / sleep / **deep_after_s** / **paused_grace_s** / meta poll) | ✅ |
| **Tier 1 Active / Tier 2 Idle / Tier 3 Deep sleep** state machine | ✅ |
| Light-sleep + tickless idle (CPU clock-gates between events) | ✅ |
| ESP Web Tools installer (live at `https://svwhisper.github.io/lyngdorf-knob/`) | ✅ |
| Secondary ESP32 deep-sleep firmware (separate repo + release) | ✅ |
| Persistent boot counter + reset reason + RTC wake history | ✅ |
| In-memory log buffer exposed at `http://<device-ip>/log` | ✅ |

## Power investigation: closed (2026-05-11)

**Final result: ~7 mA average deep-sleep idle current.** 10× improvement over the original ~50–60 mA baseline. With an 800 mAh cell, gives ~3 days pure idle / ~1.5–2 days with light daily use.

### Measurement (2026-05-11, 15 h on battery)

- Battery 100% → 50% = ~400 mAh consumed
- Wake history: 8 boots from cold reset, all `cause=3` (EXT1 encoder wake) — zero panics
- Active time decomposed from `prev_uptime` values:
  - 1 cold boot
  - 6 brief 7-min wake-and-resleep cycles (encoder bumps): ~42 min
  - 1 long use session: 2 h 29 min
  - Total: ~3.2 h active, ~11.8 h true deep sleep
- Active draw assumption ~100 mA → 320 mAh → deep-sleep floor ≈ ~7 mA average

### What got us here (in order of impact)

| Fix | Approx. savings |
|---|---:|
| Reflash secondary ESP32-U4WDH with sleep-forever firmware (USB-C orientation flip) | ~40–45 mA |
| Layer 3 deep-sleep entry (peripheral GPIO tri-state, panel SLPIN, BL gated, haptic standby, CST816D deep-sleep, WiFi deinit, encoder-aware wake mask) | ~5 mA |
| Bug fix: don't `gpio_reset_pin(19/20)` while USB-CDC console is active | enabled deep-sleep entry at all |
| Bug fix: don't call `esp_sleep_pd_config(domain, OFF)` without balancing ON | enabled deep-sleep entry at all |

### Software ceiling reached

The 7 mA floor is consistent with the schematic-derived hardware-mandatory load (~5–8 mA):

- ESP32-S3 deep sleep: ~10 µA
- Secondary ESP32-U4WDH in deep sleep (our firmware): ~10 µA
- TLV62569 buck quiescent: ~22 µA
- SGM2036 LDO quiescent: ~50 µA (always on, EN tied to 5V)
- PCM5100A DAC quiescent (no clocks): ~0.6–1.2 mA
- DRV2605 in standby: ~few µA
- SH8601 after SLPIN: ~80 µA
- Battery divider 10k/10k from 5V: 0.25 mA continuous
- Charge IC + leakage + measurement uncertainty: residual

No further savings are possible from firmware. Everything that can be turned off in software has been turned off.

### Hardware mods for further reduction (not yet attempted)

Listed in order of effort/payoff. None are needed to ship.

| Mod | Estimated savings | Effort | Notes |
|---|---:|---|---|
| **Lift L8 (BLM18AG102SN1D ferrite feeding U20)** | ~1–2 mA | 0603 ferrite removal — moderate iron skill | Kills SGM2036 LDO + entire PCM5100A DAC. Cleanest single mod. Reversible. Preferred over lifting U20 EN pin (smaller, harder, same payoff). After removal, GPIOs 39/40/41 and the secondary's I2S pins should not drive into the now-unpowered DAC inputs — already handled by Layer 3 GPIO tri-state. |
| **Replace battery divider R62/R63 (10k/10k → 100k/100k or 1M/1M)** | ~0.2 mA | Two 0603 swaps | Higher resistance reduces continuous 0.25 mA loss; 100k/100k is the sweet spot vs. ESP32-S3 ADC input impedance. |
| **Lift secondary ESP32-U4WDH EN pin, tie to GND** | up to ~10 µA | Pin lift + bridge | Sleep-forever firmware already gets to ~10 µA. Barely moves the needle. |
| **Lift DRV2605 EN pin from 3V3, route to S3 GPIO** | ~few µA | Small package, hard | DRV2605 in software standby already draws ~few µA. Marginal. |
| **Add 100 nF caps to GND on ENC_A (GPIO 8) and ENC_B (GPIO 7)** | indirect | Two caps | Suppresses spurious encoder wakes (saw 6 in 15 h, each costs ~9 mAh in active tail). |

The L8 lift is the single highest-confidence next step if someone wants to push below 5 mA.

## Power architecture (3 tiers)

| Tier | Trigger | Behaviour |
|---|---|---|
| **1 Active** | any touch / encoder event | full UI, WiFi `MIN_MODEM`, 5 s amp poll |
| **2 Idle** | no interaction for `sleep_secs` (default 120 s, NVS-configurable) | panel + backlight off, WiFi `MAX_MODEM`, Lyngdorf poll 30 s, metadata poll 60 s |
| **3 Deep sleep** | in Tier 2 ≥ `deep_after_s` (default 60 s, NVS) **AND** amp not playing ≥ `paused_grace_s` (default 60 s, NVS) | WiFi off, SH8601 SLPIN, DRV2605 standby, CST816D deep sleep, LEDC stopped + BL held low. Wakes only on ENC_A (GPIO 8) / ENC_B (GPIO 7) low (touch wake removed so CST816D fully sleeps). |

Set `deep_after_s = 0` in the web form to disable deep sleep entirely.

Plus always-on light sleep via `esp_pm_configure` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE` so the CPU clock-gates whenever tasks are blocked.

### Wake/sleep energy tradeoff

- Wake-from-deep-sleep + WiFi reassoc costs ~0.2 mAh (boot + association burst)
- Idle (Tier 2) → deep sleep saves ~0.5 mAh/min
- Breakeven: ~25 s of additional deep sleep per wake. Defaults (60 s deep_after, 60 s paused_grace) sit comfortably above breakeven without making the knob feel laggy.

## Three FreeRTOS tasks

| Task | Core | Pri | Purpose |
|---|---:|---:|---|
| `ui` | 1 | 3 | LVGL render loop; encoder + touch event drain; calls `power_tick()` |
| `net` | 0 | 2 | Lyngdorf RIO TCP (port 84): VOL/MUTE poll & cmds |
| `metadata` | 0 | 1 | HTTP/JSON poll of `amp:8080/api/getData`; HTTP play/pause via `/api/setData` |

Encoder is interrupt-driven (negedge on GPIO 7 + 8 with 5 ms per-pin debounce); no polling timer.

Inter-task: `g_cmd_queue` (encoder/touch → net), shared `g_state` guarded by `g_state_mutex`, all LVGL calls under `g_lvgl_mutex`.

## Key file map

| File | Notes |
|---|---|
| `main/main.c` | app_main; persistent wake history (RTC_DATA_ATTR) + NVS boot counter + reset-reason log; spawns ui_task + net_task |
| `main/encoder.c` | GPIO ISR-driven, switch-style decode (NOT quadrature) |
| `main/touch.c` | CST816D I2C + INT pin config (GPIO 9), TP_RST pulse, region-aware tap dispatch |
| `main/lyngdorf.c` | RIO TCP socket, !VOL/!MUTE only |
| `main/metadata.c` | esp_http_client + cJSON; HTTP GET /api/getData; play/pause via /api/setData |
| `main/ui.c` | LVGL widgets; splash w/ QR code (skip on deep-sleep wake); 3 text lines + battery + status |
| `main/display.c` | SH8601 init, ISR flush callback, 50 kHz backlight PWM |
| `main/esp_lcd_sh8601.[ch]` | Local SH8601 driver |
| `main/power.c/h` | dim → panel-sleep → deep-sleep state machine; runtime `deep_after_s` / `paused_grace_s` from NVS; IRAM-safe activity signal |
| `main/battery.c/h` | ADC1 ch0 oneshot + curve-fit cali; 10-point piecewise Li-ion SoC curve |
| `main/web_server.c` | http://device-ip/ config form; static-buffer'd to avoid httpd task stack overflow |
| `main/wifi_manager.c` | STA + AP fallback; pushes ssid/ip to UI |
| `main/log_buffer.c/h` | In-memory log ring exposed at /log |
| `main/app_config.[ch]` | NVS keys, defaults, shared g_state, mutexes, queue |
| `docs/index.html` | ESP Web Tools install page |
| `docs/manifest.json` | Manifest pointing at GitHub Releases binary URL |
| `.github/workflows/release.yml` | Auto-build + release on `git push --tags v*` |

## Important hardware quirks (don't relearn these)

- **Dual-MCU board** — secondary ESP32-U4WDH on same USB-C port. Cable orientation A → S3 (`/dev/cu.usbmodem*`); cable flipped 180° → secondary (`/dev/cu.usbserial-*`). No buttons or jumpers. If `esptool` ever reports "This chip is ESP32, not ESP32-S3", flip the cable.
- **Display chip is SH8601, not ST77916** — local driver in `main/esp_lcd_sh8601.[ch]`, Waveshare's exact 180-cmd init sequence.
- **Encoder is switch-style (NOT quadrature)** — rest at `0b11`, one direction pulses A low, the other pulses B low. ENC_B *can* rest at low in some mechanical positions; deep-sleep code masks any pin currently low.
- **Backlight PWM ≥ 25 kHz** — 5 kHz aliases against the LCD scan and produces visible horizontal bands. Currently 50 kHz.
- **Lyngdorf mute syntax: `!MUTE(ON)` / `!MUTE(OFF)`** — not `!MUTE/!UNMUTE` and not `!MUTEON/!MUTEOFF`. Both query and set use parens.
- **Lyngdorf does not expose UPnP** — use the HTTP/JSON API on port 8080 for track info.
- **Amp's HTTP server uses chunked transfer encoding** — don't trust `Content-Length`; read until EOF.
- **JSON payload shape**: top-level array; the payload is the first array element that's an object containing a `trackRoles` key.
- **CST816D `TP_INT` is on GPIO 9, `TP_RST` on GPIO 10** — INT pulses low on touch (configured via REG 0xFA = 0x70).
- **Battery sense: ADC1 ch0 (GPIO 1) via 10k/10k divider from the 5V rail** — divider sees post-charger voltage, not cell voltage directly. SoC curve in `battery.c` is a piecewise Li-ion table; linear approximation badly over-reports at low charge.

## Deep-sleep gotchas (every one of these caught us)

1. **Tickless idle is required for light-sleep to engage.** `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` and `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3` in sdkconfig. Without it, `esp_pm_configure` returns `ESP_ERR_NOT_SUPPORTED` for `light_sleep_enable=true`.
2. **Wake-pin pull-ups must be set up via the RTC GPIO API** (`rtc_gpio_pullup_en`) — the regular `gpio_pullup_en` doesn't survive into deep sleep.
3. **PM-related wake sources persist into deep sleep.** Call `esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL)` *before* `esp_sleep_enable_ext1_wakeup()`, otherwise a leftover tickless-idle resume timer fires immediately (`wake_cause=4` = TIMER).
4. **Encoder pins can rest at low.** Mechanical state can leave one switch closed. Always read actual GPIO levels just before deep sleep and exclude any low pin from the wake mask. `power.c::enter_deep_sleep` does this.
5. **CST816D may hold TP_INT low if there's an unread touch event.** Drain its data registers (read 7 bytes from reg 0x00) just before sleep to release INT.
6. **Peripheral shutdown order before `esp_deep_sleep_start()`** matters for current draw:
   1. `display_sleep(true)` → DISPOFF, then `display_enter_low_power()` → SLPIN (0x10). Without SLPIN the SH8601's internal regulator + boost stay on (~5–15 mA).
   2. `display_backlight_off()` stops the LEDC peripheral and pins BL low.
   3. `haptic_standby()` puts DRV2605 into standby.
   4. `cst816_deep_sleep()` writes 0x03 to reg 0xA5. Side effect: TP_INT no longer wakes — encoder is sole wake path.
7. **USB-CDC takes ~700 ms to renumerate after deep-sleep wake.** Without an early `vTaskDelay`, the post-wake `wake_cause=...` log line is dropped by the monitor. Currently 300 ms — bump to 1500 ms for guaranteed log visibility during development.
8. **NEVER `gpio_reset_pin(19)` or `gpio_reset_pin(20)` while USB-CDC is the active system console.** Those are the native USB-DN/USB-DP pins. Resetting them yanks the pads out from under the active console driver and the chip panics on the next log call. Symptom we saw: deep-sleep entry log cuts off mid-line at "GPIO[19]| ... OpenD" exactly when USB CDC dies. The USJ peripheral manages its own pad state across deep sleep; sub-mA savings aren't worth the crash.
9. **`esp_sleep_pd_config(domain, ESP_PD_OPTION_OFF)` is a refcount API — do not call OFF without a balancing prior ON.** ESP-IDF tracks per-domain reference counts; OFF decrements. Calling OFF on a domain whose count is already 0 trips `assert(refs >= 0)` in `sleep_modes.c:1983` and panics. Was crashing intermittently because esp_pm's light-sleep config sometimes increments the count and masks the bug. In deep sleep the chip already powers down everything except RTC slow memory, RTC IO, and the configured wake source, so the OFF calls were redundant.
10. **`/log` HTTP endpoint can never show panic info** — panic prints bypass the buffer and the reset clears RAM. Serial monitor (`idf.py monitor`) is the only way to see backtraces. Coredump partition would be an alternative if we ever set it up.

## Schematic reference

From `~/Downloads/ESP32-S3-Knob-Touch-LCD-1.8-schematic/*.png` + the structured JSON walk in `~/Downloads/power_optimisation_schematic_context.json`.

### GPIO map (ESP32-S3 side)

| GPIO | Net | Notes |
|---|---|---|
| 0 | I2S_SWITCH_IN | CH445P pin 13 EN# (active-low). HIGH disconnects I2S clocks from PCM5100A. Also boot-mode strap — must be HIGH at every reset/wake. Driven HIGH + held through deep sleep. |
| 1 | BATT_ADC | battery sense (post-charger 5V rail, 2:1 divider) |
| 2–6, 42 | SDMMC_D0–D3 / CMD / SCK | TF card slot — unused by firmware |
| 7, 8 | EC1_B, EC1_A | primary encoder, deep-sleep wake source |
| 9, 10 | TP_INT, TP_RST | CST816D touch |
| 11, 12 | TP_SDA / HAPTIC_SDA, TP_SCL / HAPTIC_SCL | shared I2C bus (touch + haptic) |
| 13–18 | LCD_QSPI_SCL / CS / D0–D3 | SH8601 display |
| 19, 20 | USB_DN, USB_DP | native USB CDC (do NOT reset before sleep) |
| 21 | LCD_RST | display reset |
| 38, 48 | ESP32S3_TX, ESP32S3_RX | debug UART (currently unused) |
| 39, 40, 41 | S3_I2S_DAC_BCK, LRCK/WS, DIN | I2S to PCM5100A |
| 45, 46 | PDM_MIC_SCK, PDM_MIC_DATA | microphone |
| 47 | LCD_BLK | backlight MOSFET gate |

### GPIO map (secondary ESP32-U4WDH)

| GPIO | Net | Notes |
|---|---|---|
| 18, 23 | ESP32S3_TX, ESP32S3_RX | UART link to S3 |
| 21, 22 | EC2_A, EC2_B | second encoder (unused) |
| 25, 26, 27 | ESP32_I2S_DAC_BCK / DIN / LRCK/WS | I2S to PCM5100A |
| 32 | XSMT | PCM5100A soft mute |

### Rail / power domains

| Rail | Source | Always on? | Software control |
|---|---|---|---|
| 5V | USB-C / battery | yes | none |
| 3V3 | U19 TLV62569 buck from 5V | yes | none (EN tied) |
| 3V3_DAC / A3V3 | U20 SGM2036 LDO from 5V (through L8 ferrite) | yes | none (EN tied to input) |
| VDD_SPI | ESP32-S3 internal | yes | ESP-IDF auto-DPD of flash |

The 3V3_DAC rail being uncontrollable is what makes the L8 lift the highest-impact remaining mod.

## Browser-based installer (ESP Web Tools)

Live. `https://svwhisper.github.io/lyngdorf-knob/` serves the install page. `v1.0.0` is the current tag. `release.yml` rebuilds and re-attaches the merged binary on every `v*` tag push.

The companion `lyngdorf-secondary-sleep` repo has its own release workflow + `v1.0.0` tag — the secondary's `secondary_sleep-merged.bin` is attached to the GitHub Release so end users can flash without ESP-IDF.

## Possible future work

- Album art (JPEG decode + LVGL image widget; URL is in `trackRoles.icon`)
- Long-poll via `/api/pollQueue` for instant metadata updates
- Source / sample-rate / bitrate display
- Coredump partition for off-device panic backtraces (currently requires serial monitor)
- ~~Tune deep-sleep entry on amp source change~~ — **explicitly NOT wanted.** Even on analogue sources the knob's volume control remains useful. Track-info will simply be blank for non-streaming inputs; that is acceptable. Deep-sleep gating remains based on user idle time + amp not-playing.
