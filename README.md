# lyngdorf-knob

ESP32-S3 firmware for the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** board, turning it into a physical controller for the **Lyngdorf TDAI-3400** amplifier.

## Features

| Interaction | Action |
|---|---|
| Rotate encoder | Volume up/down (default 0.5 dB/detent) |
| Single tap (touch) | Mute / unmute |
| Double tap (touch) | Play / pause (UPnP) — also mutes for non-UPnP sources |

**Display** (360×360 round):
- 270° volume arc (teal, dims when muted)
- Scrolling artist / album / track title (from UPnP metadata)
- Volume dB overlay on rotation (fades after 2 s)
- Play/pause and mute icons
- Connection status
- Dims after configurable idle timeout, sleeps (panel off) after a second timeout; any encoder or touch wakes instantly

## Hardware

| Board | Waveshare ESP32-S3-Knob-Touch-LCD-1.8 |
|---|---|
| MCU | ESP32-S3 (240 MHz, 16 MB flash, 8 MB PSRAM) |
| Display | 360×360 round LCD, ST77916, QSPI |
| Touch | CST816D capacitive, I2C |
| Encoder | EC11-style quadrature, GPIO 7/8 |
| WiFi | 2.4 GHz 802.11 b/g/n |

### Pin assignments

| Function | GPIO |
|---|---|
| LCD CLK | 13 |
| LCD CS | 14 |
| LCD D0–D3 | 15–18 |
| LCD RST | 21 |
| LCD Backlight | 47 |
| Touch SDA | 11 |
| Touch SCL | 12 |
| Encoder A | 8 |
| Encoder B | 7 |

## Architecture

```
main.c               — app_main(), ui_task (core 1, pri 3), net_task (core 0, pri 2)
display.c/h          — ST77916 QSPI init, LVGL 8 buffers, 2 ms tick timer, backlight PWM
touch.c/h            — CST816D I2C driver, LVGL indev, edge-based tap/double-tap detection
encoder.c/h          — 3 ms polling timer, gray-code quadrature decode → cmd queue
lyngdorf.c/h         — Persistent TCP socket to amp:84, !VOLCH / !MUTE / poll state
upnp.c/h             — SSDP discovery, AVTransport SOAP play/pause + GetPositionInfo
ui.c/h               — LVGL widgets: volume arc, scrolling labels, icons, vol overlay
power.c/h            — Dim/sleep state machine, activity signalling, WiFi modem-sleep
wifi_manager.c/h     — STA mode, AP fallback after 5 retries, event-driven
web_server.c/h       — HTTP config form at http://<device-ip>/
app_config.c/h       — NVS get/set, shared lk_state_t, g_state_mutex, g_cmd_queue
```

### Task model

- **ui_task** (core 1, priority 3): holds `g_lvgl_mutex`, calls `lv_timer_handler()` every ≤10 ms, processes encoder events, applies state diffs to LVGL widgets.
- **net_task** (core 0, priority 2): drains `g_cmd_queue` (volume/mute/play-pause commands), polls Lyngdorf state every 5 s, polls UPnP metadata every 3 s.
- **web_server**: started by `wifi_manager` on WiFi connect (or in AP mode); runs inside `esp_httpd` task.

### Amp control (Lyngdorf TCP protocol)

The TDAI-3400 listens on **TCP port 84**. Commands are ASCII terminated with `\r`. Responses echo back with `#` prefix.

| Command | Effect |
|---|---|
| `!VOLCH(n)` | Relative volume change in 0.1 dB units |
| `!VOL?` | Query current volume → `!VOL(-300)` = -30.0 dB |
| `!MUTE` / `!UNMUTE` | Mute toggle |

### UPnP (play/pause + metadata)

The TDAI-3400 is a UPnP renderer. The firmware:
1. Sends SSDP M-SEARCH multicast to discover the amp's AVTransport endpoint.
2. Fetches the device description XML to extract the control URL.
3. Polls `GetPositionInfo` every 3 s for artist/album/title (DIDL-Lite).
4. Sends `Play` / `Pause` SOAP actions on double-tap.

A manually configured UPnP URL (from the web UI) bypasses SSDP discovery.

## Building

### Prerequisites

- [ESP-IDF 5.2+](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)
- Python 3.8+

### Build & flash

```bash
cd lyngdorf-knob
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodem* flash monitor
```

The ESP-IDF Component Manager will automatically fetch:
- `lvgl/lvgl` 8.3.x
- `espressif/esp_lcd_st77916`
- `espressif/esp_lcd_touch`

### First boot

On first boot (no WiFi credentials stored), the device starts a WiFi access point named **LyngdorfKnob**. Connect to it and navigate to **http://192.168.4.1** to configure:

- WiFi SSID / password
- Amplifier IP address
- Volume step (default: 5 = 0.5 dB per encoder detent)
- UPnP Control URL (optional — leave blank for auto-discovery)
- Dim display after N seconds idle (default: 30, 0 to disable)
- Sleep display after N seconds idle (default: 120, 0 to disable)

After saving, the device reboots and connects to your network.

The config page is also available at `http://<device-ip>/` any time while connected to WiFi.

## Reference projects

- [muness/roon-knob](https://github.com/muness/roon-knob) — same board family (AMOLED variant), excellent reference for FreeRTOS patterns, LVGL architecture, WiFi provisioning.
- [modi12jin/IDF-S3_ST77916-QSPI_CST816T-I2C_LVGL](https://github.com/modi12jin/IDF-S3_ST77916-QSPI_CST816T-I2C_LVGL) — exact hardware match, reference for display driver init.
