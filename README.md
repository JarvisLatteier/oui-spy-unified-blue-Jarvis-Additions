# OUI-SPY Unified Blue

Multi-mode WiFi/BLE surveillance detection firmware for ESP32-S3 boards — forked from [colonelpanichacks/oui-spy-unified-blue](https://github.com/colonelpanichacks/oui-spy-unified-blue) with multi-board support, CYD touch UI, I2S audio, Sky Spy channel hopping, and more.

---

## Board Support

| Board | Modes | Display | Audio | SD | GPS |
|-------|-------|---------|-------|----|-----|
| Seeed XIAO ESP32-S3 | 1, 2, 4, 5 | None | Piezo buzzer | No | No |
| LILYGO T-Dongle-S3 | 1, 2, 4, 5 | 0.96" ST7735 (160×80) | None | Yes (1-bit) | No |
| Freenove ESP32-S3 CYD | 4, 5 | 2.8" ILI9341 (320×240) touch | ES8311 I2S codec | Yes (4-bit) | Yes |

---

## Modes

| # | Name | Description |
|---|------|-------------|
| 0 | **Selector** | Boot menu — choose a mode, device reboots into it |
| 1 | **Detector** | BLE scanner that alerts on target OUI/MAC matches |
| 2 | **Foxhunter** | RSSI-based proximity tracker — beeps faster as you close in |
| 4 | **Flock-You** | Detects Flock Safety cameras and Raven gunshot detectors via BLE |
| 5 | **Sky Spy** | Passive FAA Remote ID drone detection via WiFi + BLE |

> Mode 0 (Selector) runs on all boards. Detector and Foxhunter are XIAO/T-Dongle only. Flock-You and Sky Spy run on all boards including CYD.

---

## Quick Start — Flash Pre-Built Binary

No PlatformIO needed. Just Python and a USB cable.

```bash
pip install esptool pyserial
```

Pick the right binary for your board from the `firmware/` folder:

| Board | Binary |
|-------|--------|
| Seeed XIAO ESP32-S3 | `firmware/oui-spy-xiao.bin` |
| LILYGO T-Dongle-S3 | `firmware/oui-spy-tdongle.bin` |
| Freenove ESP32-S3 CYD | `firmware/oui-spy-cyd.bin` |

Then flash:

```bash
python flash.py firmware/oui-spy-xiao.bin
```

The script auto-detects your serial port. Options:

```bash
python flash.py                              # auto-detect .bin from firmware/
python flash.py firmware/oui-spy-cyd.bin    # flash a specific file
python flash.py firmware/oui-spy-xiao.bin --erase   # full erase before flash
```

---

## Building from Source

Requires [PlatformIO](https://platformio.org/).

```bash
pio run -e seeed_xiao_esp32s3    # XIAO
pio run -e lilygo_tdongle_s3     # T-Dongle
pio run -e freenove_cyd_s3       # CYD

pio run -e freenove_cyd_s3 -t upload   # build + flash
pio device monitor                      # serial output (115200 baud)
```

Build output lands in `.pio/build/<env>/firmware.bin`.

---

## Hardware Pinouts

### Seeed XIAO ESP32-S3

| GPIO | Function |
|------|----------|
| 3 | Piezo buzzer |
| 4 | NeoPixel (WS2812) |
| 21 | LED_PIN (mode indicator, unused on XIAO) |
| 0 | BOOT button — hold 1.5s to return to Selector |

### LILYGO T-Dongle-S3

| GPIO | Function |
|------|----------|
| 3 | TFT MOSI |
| 4 | TFT CS |
| 5 | TFT SCLK |
| 2 | TFT DC |
| 1 | TFT RST |
| 38 | TFT backlight (PWM) |
| 39 | APA102 clock |
| 40 | APA102 data |
| 12 | SD_MMC CLK |
| 14 | SD_MMC D0 |
| 16 | SD_MMC CMD |
| 17 | SD_MMC D1 |
| 18 | SD_MMC D3 |
| 21 | SD_MMC D2 |
| 0 | BOOT button |

### Freenove ESP32-S3 CYD

| GPIO | Function |
|------|----------|
| 42 | NeoPixel (WS2812) |
| 1 | NS4150 amp enable (active-LOW) |
| 4 | I2S MCK (ES8311 codec) |
| 5 | I2S BCK |
| 6 | I2S DIN |
| 7 | I2S WS |
| 8 | I2S DOUT |
| 15 | Touch SCL (FT6336U) |
| 16 | Touch SDA |
| 17 | Touch INT |
| 18 | Touch RST |
| 38 | SD_MMC CLK |
| 39 | SD_MMC D0 |
| 40 | SD_MMC CMD |
| 41 | SD_MMC D1 |
| 43 | GPS TX (UART0) |
| 44 | GPS RX (UART0) |
| 45 | TFT backlight (PWM) |
| 47 | SD_MMC D3 |
| 48 | SD_MMC D2 |
| 0 | BOOT button |

---

## Key Additions in This Fork

See **[Jarvis' Additions.md](Jarvis'%20Additions.md)** for the full user guide, mode-by-mode instructions, and detailed technical changelog. Summary:

- **Multi-board support** — LILYGO T-Dongle-S3 and Freenove CYD as first-class build targets
- **CYD touch UI** — full selector, settings page, and in-mode touch controls for CYD
- **I2S audio (CYD)** — ES8311 codec + NS4150 amp with non-blocking FreeRTOS tone task
- **Sky Spy channel hopping** — scans WiFi CH1–13 with weighted CH6 dwell for NAN drone beacons
- **Sky Spy LED/TONE toggles** — CYD touch buttons with NVS persistence
- **SD card improvements** — deferred file creation, per-mode file count footer, 64GB support
- **Brightness/incognito controls** — available in all modes that have a web server
- **Initial display state** — modes draw their proper screen immediately after setup, not just on first event
- **Debug cleanup** — removed debug Serial.println calls, dead code, heap diagnostics

---

## Credits

**[colonelpanichacks](https://github.com/colonelpanichacks)** — original OUI-SPY Unified firmware and all upstream mode implementations.

**[Will Greenberg](https://github.com/wgreenberg)** — [flock-you](https://github.com/wgreenberg/flock-you) research: BLE manufacturer ID `0x09C8` (XUNTONG) detection method and Raven service UUID patterns used in Flock-You mode.

---

## Disclaimer

This tool is intended for security research, privacy auditing, and educational purposes. Detecting the presence of surveillance hardware in public spaces is legal in most jurisdictions. Always comply with local laws regarding wireless scanning and signal interception. The authors are not responsible for misuse.
