# Related projects — useful peers on the same board / problem

These are projects that target the same Waveshare ESP32-S3-Knob-Touch-LCD-1.8
hardware or the same Lyngdorf control surfaces. Worth a look before
implementing anything new — chances are someone has already solved it.

## Same hardware

### [joshuacant/BlueKnob](https://github.com/joshuacant/BlueKnob)
Waveshare knob as a BLE media remote (Bluetooth keyboard/mouse). Most
relevant peer for this project: closest match for "physical media remote
on this exact board".

**What we borrowed:**
- **LVGL gesture-event pattern** for swipe detection. They register
  `LV_EVENT_GESTURE` callbacks on screen objects rather than reading the
  CST816D's gesture register directly. LVGL detects swipes from the X/Y
  point stream that the touch indev already produces, so no changes are
  needed to `touch_read_cb`. Their callbacks look like:

  ```c
  void screenMediaControls_onGestureLeft(lv_event_t *e) {
      set_screen(ui_screenDpad);
  }
  ```

  In the callback you read direction with
  `lv_indev_get_gesture_dir(lv_indev_get_act())` and act on it.

**Wake-from-deep-sleep tradeoff confirmed:**
> "30 minutes after entering device sleep, the knob will enter
> hibernation. At this point knob turns and screen taps will no longer
> wake it. The power switch has to be turned off and back on again to
> boot. This is to maximize battery life."

Another developer reached the same conclusion we did: the CST816D's
~1.5 mA active draw isn't worth keeping alive across deep sleep just
for touch-wake. We at least wake on rotation; they require a power
cycle.

### [EmbeddedWizardGUI/ESP32-S3-Knob-Touch-LCD-1.8-EN](https://github.com/EmbeddedWizardGUI/ESP32-S3-Knob-Touch-LCD-1.8-EN)
Embedded Wizard GUI demo on the same board. Different framework
(Embedded Wizard rather than LVGL) but useful as a reference for
peripheral init sequences.

### [nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518](https://github.com/nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518)
ESPHome configuration covering Waveshare and its Guition K5 clone. Worth
referencing for the YAML hardware-description style if we ever expose a
Home Assistant integration.

### [nkinnan/manufacturer-firmware-and-sd-card-contents_Waveshare-ESP32-S3-Knob-Touch-LCD-1.8](https://github.com/nkinnan/manufacturer-firmware-and-sd-card-contents_Waveshare-ESP32-S3-Knob-Touch-LCD-1.8)
Stock Waveshare firmware binaries for both ESP chips. Useful if anyone
ever wants to revert.

### [SuperEugen/esp32-s3-knob-hardware-explorer](https://github.com/SuperEugen/esp32-s3-knob-hardware-explorer)
Hardware reference and test app — handy for verifying individual chips
in isolation.

### [UnkMihai/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8-first-successful-screen-test](https://github.com/UnkMihai/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8-first-successful-screen-test)
Standalone working SH8601 driver (no LVGL). Useful for sanity-checking
the display init sequence if anything breaks.

### [gilphilbert/waveshare_esp32s3_knob_touch_platformio](https://github.com/gilphilbert/waveshare_esp32s3_knob_touch_platformio)
PlatformIO build template for the same board. Reference for PlatformIO
config if anyone wants to move off ESP-IDF.

## Same control protocol (Lyngdorf / Steinway streaming module)

### [siegeld/steinway_lyngdorf](https://github.com/siegeld/steinway_lyngdorf)
Python control library for the Steinway P100, which uses the same
streaming module as Lyngdorf TDAI. **Most authoritative source** for
the `player:player/control` JSON values — see `lib/steinway_p100/api/client.py`.

### [thejens/lyngdorf-mcp](https://github.com/thejens/lyngdorf-mcp)
MCP server with the most complete RIO TCP command coverage we've found.
Reference for source/RoomPerfect/voicing/balance commands if we ever
expose them.

### [fishloa/lyngdorf](https://github.com/fishloa/lyngdorf)
Python library. Notable for committing the official PDF spec files for
TDAI-3400 / TDAI-2170 / TDAI-1120 / MP-40 / MP-50 / MP-60 in its `spec/`
directory — handy when Lyngdorf's website is being weird about PDF
downloads.

### [homeassistant-projects/hass-lyngdorf](https://github.com/homeassistant-projects/hass-lyngdorf)
Home Assistant custom component. Doesn't implement track navigation (no
need for it in HA), but a useful reference for source selection,
RoomPerfect, and zone-2 handling.

### [DaftMunk/tdai2170pi](https://github.com/DaftMunk/tdai2170pi)
TDAI-2170 over RS-232. Older protocol generation but command syntax
matches.

### Same family / same streaming API
Useful when a Lyngdorf-specific implementation is missing a feature —
the streaming module is shared:

- [N0ciple/pykefcontrol](https://github.com/N0ciple/pykefcontrol) — KEF speakers
- [hilli/go-kef-w2](https://github.com/hilli/go-kef-w2) — KEF W2 in Go
- [nickvanw/KEFControl](https://github.com/nickvanw/KEFControl) — KEF in Swift
- [codecarve/kefconnect](https://github.com/codecarve/kefconnect) — KEF in TypeScript
- [ilia-ae/klipsch_flexus](https://github.com/ilia-ae/klipsch_flexus) — Klipsch Flexus (same API)
