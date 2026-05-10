# lyngdorf-knob session notes
_Last updated: 2026-05-10 — deep sleep now stable (USB-pin-reset + esp_sleep_pd_config bugs both fixed); battery test in progress_

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
| **3 Deep sleep** | in Tier 2 ≥ 5 min **AND** amp not playing ≥ 2 min | WiFi off, SH8601 SLPIN, DRV2605 standby, CST816D deep sleep, LEDC stopped + BL held low. Wakes on ENC_A (GPIO 8) / ENC_B (GPIO 7) low. (Touch wake removed so CST816D can fully sleep.) |

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
6. **Peripheral shutdown order before `esp_deep_sleep_start()`** matters for current draw:
   1. `display_sleep(true)` → DISPOFF, then `display_enter_low_power()` → SLPIN (0x10). Without SLPIN the SH8601's internal regulator + boost stay on (~5–15 mA).
   2. `display_backlight_off()` stops the LEDC peripheral and pins BL low so the backlight driver IC isn't being switched.
   3. `haptic_standby()` puts DRV2605 into standby (~1.5 mA → few µA).
   4. `cst816_deep_sleep()` writes 0x03 to reg 0xA5 (~1.5 mA → ~5 µA). Side effect: TP_INT no longer wakes the chip — encoder is the sole wake path.
7. **USB-CDC takes ~700 ms to renumerate after deep-sleep wake.** Without an early `vTaskDelay`, the post-wake `wake_cause=...` log line is dropped by the monitor. Currently 300 ms — bump to 1500 ms if you want guaranteed log visibility during development.
8. **NEVER `gpio_reset_pin(19)` or `gpio_reset_pin(20)` while USB-CDC is the active system console.** Those are the native USB-DN/USB-DP pins. Resetting them yanks the pads out from under the active console driver and the chip panics on the next log call. Symptom we saw: deep-sleep entry log cuts off mid-line at "GPIO[19]| ... OpenD" exactly when USB CDC dies, then `boot_count` increments with `prev_reset=PANIC`. The USJ peripheral manages its own pad state across deep sleep; sub-mA savings aren't worth the crash. Layer-3 patch had these in the tri-state list — removed in commit after observing the panic loop (35 cycles in one battery session).
9. **`esp_sleep_pd_config(domain, ESP_PD_OPTION_OFF)` is a refcount API — do not call OFF without a balancing prior ON.** ESP-IDF tracks per-domain reference counts; OFF decrements, ON increments. Calling OFF on a domain whose count is already 0 trips `assert(refs >= 0)` in `sleep_modes.c:1983` and panics. Was crashing intermittently — esp_pm's light-sleep config sometimes increments the count and masks the bug, so it looked like one cycle would succeed before the next panicked. In deep sleep the chip already powers down everything except RTC slow memory, RTC IO, and the configured wake source, so the OFF calls were redundant anyway. Removed in commit after backtrace pointed at `esp_sleep_pd_config` from power.c:194.

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
- ~~Tune deep-sleep entry on amp source change~~ — **explicitly NOT wanted.** Even on analogue sources the knob's volume control remains useful. Track-info will simply be blank for non-streaming inputs; that is acceptable. Deep-sleep gating should remain based on user idle time + amp not-playing, not on source type.

---

## Power-consumption investigation (in progress, **uncommitted patch on disk**)

### Dual-MCU board — flash via USB-C orientation flip

**The single USB-C port routes to BOTH MCUs depending on cable orientation** (Waveshare uses the connector's mirrored pin pairs as a mux):
- Cable orientation A → ESP32-S3 → enumerates as `/dev/cu.usbmodem*` (native USB-CDC)
- Cable orientation B (flipped 180°) → ESP32-U4WDH → enumerates as `/dev/cu.usbserial-*` (via USB-UART bridge)

No BOOT buttons or jumpers. The cable flip is the only selector. (Confirmed retrospectively: an earlier `esptool` attempt that reported "This chip is ESP32, not ESP32-S3" on `/dev/cu.usbserial-1210` was the cable in secondary orientation.)

### Secondary ESP32 — sleep-forever firmware flashed (2026-05-10)

**Status:** flashed and running. Awaiting overnight battery test on full charge to confirm savings.

**Project location:** `~/Developer/lyngdorf-secondary-sleep/` — minimal ESP-IDF app, target `esp32`. Single `app_main` calls `esp_deep_sleep_start()` with no wake sources. Secondary now sits in deep sleep until power-cycled.

**Flash procedure (for re-flash or rollback):**
```
# Flip USB-C 180° so cable talks to secondary
ls /dev/cu.usbserial-*    # confirm enumeration
~/.espressif/python_env/idf5.2_py3.14_env/bin/python ~/esp/esp-idf/tools/idf.py \
  -C ~/Developer/lyngdorf-secondary-sleep \
  -p /dev/cu.usbserial-* flash
# Flip cable back to resume normal S3 work
```

To restore stock Waveshare audio firmware on the secondary: same flip, then flash whatever stock binary Waveshare provides (not currently archived locally).

### ⚠️ Prime suspect for the ~50 mA gap: secondary ESP32-U4WDH

The board has **two MCUs**, not one. Schematic page 3 (`3_ESP32-CHIP.png`) shows an **ESP32-U4WDH** alongside the ESP32-S3. Confirmed via `~/Downloads/power_optimisation_schematic_context.json`. Earlier sessions assumed single-MCU — wrong.

**Secondary's role on stock firmware:** audio streaming MCU (own I2S → PCM5100A DAC) + reads the second encoder (`EC2_A/B` on its GPIO 21/22). We use neither.

**Wiring of secondary's enable pin (`ESP32_EN`):**
- Driven by the USB-UART bridge **RTS#** (for flash auto-reset) plus a pull-up
- **NOT connected to any ESP32-S3 GPIO** — S3 cannot turn it off in software
- A UART link exists (S3 `ESP32S3_RX/TX` ↔ secondary GPIO 23/18) but stock firmware almost certainly doesn't listen for a sleep command

An awake ESP32 with WiFi/CPU active draws 30–100 mA — consistent with the unexplained 50 mA we keep seeing.

**Diagnostic step (no code change, no rework):** physically hold the secondary's EN pin to GND (jumper or tweezer). Measure current. If it drops by tens of mA, the secondary is the culprit.

**Permanent fix options, ranked (revised):**
1. **Reflash secondary with "sleep forever" firmware** — no rework needed. Waveshare routes the single USB-C port to *both* MCUs via the connector's orientation-dependent pins: cable in one orientation → S3 (`/dev/cu.usbmodem*`, native USB-CDC); cable flipped 180° → secondary (`/dev/cu.usbserial-*`, via USB-UART bridge). Confirmed retrospectively: an earlier `esptool` attempt that reported "This chip is ESP32, not ESP32-S3" on `/dev/cu.usbserial-1210` was the cable in secondary orientation. Procedure: flip cable, `esptool --chip esp32 write_flash` a tiny `esp_deep_sleep_start()` app, flip back. Reversible by reflashing stock. Saves up to ~50 mA. Note: `erase_flash` alone leaves the ROM bootloader running (~5–15 mA) — a real deep-sleep app is needed for µA-level idle.
2. **HW mod**: lift secondary `ESP32_EN`, jumper to a free S3 GPIO (38 or 48 are unused), drive low + `gpio_hold_en`. Up to ~50 mA saved. Reversible. Useful if we ever want runtime control rather than always-off.
3. Reverse-engineer stock firmware UART protocol for a sleep command. Lowest priority.

**Diagnostic shortcut before reflashing:** physically hold the secondary's EN pin to GND with a jumper, measure current. Confirms the secondary is the ~50 mA culprit before committing to anything.

The previously-planned L8/SGM2036 DAC depower mod is now lower priority — its expected savings (~1–2 mA) are dwarfed by the secondary-MCU draw.



### What's been measured

| Test | Result |
|---|---|
| 30 min idle on battery, after Tier-2 + deep-sleep peripheral shutdown landed (commit `ff574cd`) | 100 % → 100 % (no change visible) |
| 6 h idle on battery, amp powered off, deep sleep was entered (`UI ready (no splash — wake from deep sleep)` confirmed) | 100 % → 57 % at 3530 mV → ~57 mA average |

The 30-min "no drop" is misleading — the gauge resolution doesn't show < 1 % swings, and `battery_pct` after deep-sleep wake is freshly sampled. The 6 h test is the real signal: ~344 mAh consumed in 6 h ≈ 57 mA average. Same order of magnitude as the pre-fix 53 mA.

### Schematic walk (from `~/Downloads/ESP32-S3-Knob-Touch-LCD-1.8-schematic/*.png`)

Hardware power floor we **cannot** reach from software:

| Chip | Rail | Quiescent | Software EN? |
|---|---|---|---|
| ESP32-S3 deep sleep | 3V3 | ~10 µA | n/a |
| **PCM5100A audio DAC** (page 5) | **3V3_DAC** (separate SGM2036 LDO, EN tied to 5V) | **~3–6 mA** quiescent, ~30 mA if I2S clocks present | ❌ no — LDO `EN` hard-wired to 5V |
| **DRV2605L haptic** (page 5) | 3V3 | ~1.7 mA active | ❌ — `HAPTIC_EN` hard-wired to 3V3 (only software standby is available, ≤ ~few µA) |
| **PDM microphone** MICM05D050NK1CPM (page 4) | 3V3 | ~0.5–1 mA | ❌ no enable pin — always on |
| CST816D (deep sleep) | 3V3 | ~5 µA | ✅ already done |
| SH8601 (after SLPIN) | 3V3, BL gated by AO3400 MOSFET via GPIO 47 | ~80 µA | ✅ already done |
| TLV62569 buck (3V3 itself) | 5V | ~22 µA | EN tied to 5V (must stay on) |
| SGM2036 LDO (3V3_DAC) | 5V | ~50 µA | ❌ EN tied to 5V |
| W25Q128 flash | VDD_SPI | ~1 µA in DPD | ESP-IDF auto-DPD |

**Sum of mandatory hardware floor ≈ 5–8 mA.** That's what the device should average if everything is in its lowest state. For an 800 mAh cell that's still only ~6–10 days idle — not weeks.

### GPIO-to-net mapping discovered (from `2_ESP32S3-R8.png`, right half)

| GPIO | Net | Notes |
|---|---|---|
| 0 | I2S_SWITCH_IN | **CH445P pin 13 EN# (active-low)**. HIGH disconnects I2S clocks from PCM5100A. Also boot-mode strap → must be HIGH at every reset/wake. |
| 1 | BATT_ADC | already used |
| 2–6, 42 | SDMMC_D0–D3 / CMD / SCK | TF card slot — unused by firmware, no chip to disable |
| 7, 8 | EC1_B, EC1_A | encoder, used as deep-sleep wake source |
| 9, 10 | TP_INT, TP_RST | CST816D |
| 11, 12 | TP_SDA / HAPTIC_SDA, TP_SCL / HAPTIC_SCL | shared I2C bus |
| 13–18 | LCD_QSPI_SCL / CS / D0–D3 | display |
| 19, 20 | USB_DN, USB_DP | native USB CDC |
| 21 | LCD_RST | display reset |
| 38, 48 | ESP32S3_TX, ESP32S3_RX | debug UART (probably unused) |
| 39, 40, 41 | S3_I2S_DAC_BCK, LRCK/WS, DIN | I2S to PCM5100A |
| 45, 46 | PDM_MIC_SCK, PDM_MIC_DATA | microphone |
| 47 | LCD_BLK | backlight MOSFET gate |

### What's currently on disk (**uncommitted, built clean, not yet flashed for the latest layer**)

Three layers of changes, all built and ready to flash:

1. **Persistent wake diagnostics** in `main.c` using `RTC_DATA_ATTR`.
   - `s_wake_count`: incremented every `app_main()`. Survives deep sleep, wiped on cold reset.
   - `s_wake_hist[16]`: ring buffer of recent `{cause, ext1_mask, prev_uptime_seconds}`
   - `diag_record_pre_sleep()` exported from `main.c`, called by `power.c::enter_deep_sleep` right before `esp_deep_sleep_start()` to stamp this boot's uptime into the next event
   - All dumped via `ESP_LOGI` at boot, so visible in `curl /log` after wake

2. **GPIO 0 driven HIGH and held through deep sleep** in `main.c` — `gpio_hold_en(GPIO_NUM_0) + gpio_deep_sleep_hold_en()`. Disables the I2S muxer (CH445P EN# active-low) feeding the always-powered PCM5100A; also nails down the boot-strap pin.

3. **Final-pass software power-cuts in `power.c::enter_deep_sleep`**:
   - `esp_wifi_deinit()` after stop — tears down PHY/RF beyond just halting
   - Tri-state every peripheral GPIO we drove: LCD QSPI + RST + BL, touch I2C + INT + RST, I2S DAC clocks (39/40/41), PDM mic (45/46), unused SDMMC pads (2/3/4/5/6/42), native USB (19/20). Each `gpio_reset_pin` returns the pad to "input, no pull, no drive".
   - `esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, OFF)` and `CPU OFF`. (Only two domains S3 supports explicit PD on. `ESP_PD_DOMAIN_RTC_FAST_MEM` and `XTAL` constants don't exist on this SoC — `SOC_PM_SUPPORT_RTC_FAST_MEM_PD` and `SOC_PM_SUPPORT_XTAL_PD` are unset. RTC slow mem deliberately left ON so RTC_DATA_ATTR survives.)

Files modified:
- `main/main.c` — diag struct + GPIO 0 hold
- `main/power.c` — extern `diag_record_pre_sleep` + WiFi deinit + GPIO tri-state + sleep_pd_config

### Test results so far

| Test | Wake count | Result |
|---|---|---|
| Pre-fix baseline (commit `ff574cd`) | n/a | 100 % → 57 % over 6 h ≈ 57 mA avg |
| Layer 1+2 flashed (diag + GPIO 0 hold), 30 min test | **2** (cold + 1 wake) — confirmed single deep-sleep entry, **no spurious wake loop** | 62 % → 53 % over 30 min (≈ 7 min awake + 23 min sleep). Roughly the same draw as before. **Conclusion: S3 deep sleep is real but the *board* draws ~50–60 mA continuously — almost certainly because the secondary ESP32-U4WDH is still awake (see "Prime suspect" section above).** |
| Layer 3 + secondary-sleep firmware (2026-05-10) | 35 cycles in one battery session — every deep-sleep attempt panicked | Root cause: `gpio_reset_pin(19/20)` was crashing the USB-CDC console mid-entry. Fix: removed 19/20 from the tri-state list. After fix, device cleared the 7-min mark without rebooting — first time deep sleep has actually engaged with all power optimisations active **and** the secondary MCU sleeping. Overnight battery test in progress. |
| Layer 3 (this final-pass) | not yet flashed | TBD |

### How to resume

1. Flash the on-disk build: `idf.py -p /dev/cu.usbmodem* flash`
2. Full power-cycle (unplug battery + USB so wake counter resets to 0)
3. Note starting battery % from boot log (`battery: NNNN mV → NN%`)
4. Leave on battery, ideally overnight, with the amp powered off (so deep-sleep gate is satisfied)
5. Wake by rotating the encoder, immediately `curl http://<device-ip>/log`
6. Examine: `wake history: N boots`, the `cause/mask/prev_uptime` rows, and the new `battery: NNNN mV → NN%` line. Compute average mA: `((start_pct - end_pct) / 100) × 800 / hours`.

### Decision tree after the next test

| Average current after Layer 3 | Diagnosis | Next move |
|---|---|---|
| ≤ ~10 mA | Layer 3 worked. Hardware floor reached. | Commit, update README's "battery life" claim, done. |
| ~20–40 mA | Partial improvement. Some peripheral still alive. | Suspect: PCM5100A still drawing ~5 mA × N some multiplier; or the DRV2605 LDO; or the PDM mic. None are software-fixable. |
| Still ~50+ mA | Layer 3 had no effect. | Hardware mod is the only remaining lever. Evaluate the SGM2036 EN-pin lift. |

### Hardware mod option (user is "thinking about it")

Single biggest software-impossible win: lift **U20 SGM2036 pin 3 (EN)** off the 5V trace, run a wire to a free GPIO. With software control of `3V3_DAC`, the entire PCM5100A audio DAC + its LDO go away — should remove ~5–6 mA. Combined with the existing software shutdown that brings deep sleep into the µA range. Permanent mod; needs a fine-tip iron and good light. Decision pending until user examines the board.

The DRV2605 EN pin is also tied to 3V3, but lifting it is harder (smaller package, less accessible) and the gain is smaller (~3–5 mA).

### Where the code lives

- Last committed: `ff574cd` — peripheral shutdown for Tier 2 + deep sleep
- After that: `099b663` — README hero photo
- After that: `154efab` — README copy edit (done by user on github)
- All on-disk changes since `154efab` are uncommitted. `git stash` will park them cleanly if needed.

### How to interpret the diagnostic on resume

Flash the patch, do a **full power cycle** (battery + USB unplugged; or hold a reset button) so `s_wake_count` resets to 0. Leave the device alone overnight on battery, then `curl http://<device-ip>/log`. The first lines of output will be the wake history.

| Observation | Diagnosis | Next move |
|---|---|---|
| `wake history: 1 boots` (only the wake to read the log) | Slept once for hours. The hardware floor + whatever else is real. | If draw is ≤ ~10 mA: stop, that's the floor. If still ~50 mA: must be the DAC running (not just quiescent) — check `S3_I2S_DAC_BCK/LRCK/DIN` GPIO states |
| `wake history: 50+ boots` with small `prev_uptime` values | Spurious wake loop. Each cycle: cold boot → WiFi → idle to Tier 2 → deep sleep → woken again | Inspect `cause` and `mask` of recent entries. `cause=4 mask=0x80` = ENC_B; `0x100` = ENC_A; `mask=0x0` = TIMER (means we missed a wake source) |
| Long `prev_uptime` (~7 min) but many entries | Sleep is entered, but something wakes it within seconds and that triggers a fresh tier-2-to-deep cycle | Floating encoder line picking up noise; consider 100 nF cap to ground on ENC_A/B |

### Things to try if GPIO 0 hold + diag don't tell the full story

1. **Tri-state the I2S clock pins (39, 40, 41) before deep sleep.** Currently they're left in their post-init state. If they're driving levels into the (disconnected) DAC, that's wasted current. `gpio_reset_pin` on each before sleeping.
2. **Drive PDM mic clock pin (GPIO 45) low.** With clock missing, the mic should park.
3. **Check actual draw with a USB power meter** inline — separates "device draws X" from "battery gauge says Y". The on-board battery sense reads the same rail USB feeds, so plugging in for the multimeter masks the question.
4. **Physical: lift the SGM2036 EN pin** (pin 3) and tie to a free GPIO — gives software shutdown of the entire DAC LDO. Last-resort hack but biggest single win available.
5. **Tag a v1.0.x release** once power story is settled. Currently `v1.0.0` is the only tag and predates these changes.

### Pre-existing baseline before any of this work

`commit ff574cd` ("Cut idle current draw — peripheral shutdown in Tier 2 + deep sleep") is the last committed state. Everything since then is uncommitted diagnostic + GPIO 0 work. If the diagnostic exposes a different problem and we want to back out, `git stash` will park the diag cleanly.
