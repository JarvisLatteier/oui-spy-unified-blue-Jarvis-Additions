# Jarvis' Additions

## 1. Brightness / LED / Incognito Controls in All Boot Modes

Previously, display brightness, LED strip brightness, and incognito mode controls only existed in the Selector (mode 0) web UI. Now these controls are available in every mode that has a web server:

- **Detector (mode 1)** — in the "Audio & Visual Settings" section
- **Foxhunter (mode 2)** — in the "Audio & Visual Settings" section
- **Flock-You (mode 4)** — in the TOOLS tab under "DISPLAY SETTINGS"

Sky Spy (mode 5) has no web server, so it is excluded.

### How It Works

Each mode registers a `/brightness` GET endpoint on its web server that:
- Accepts `?disp=N` and/or `?led=N` parameters (0-255)
- Applies changes immediately via `display_set_brightness()` / `ledStrip_setBrightness()`
- Persists values to NVS namespace `"ouispy-br"` (same namespace used by main.cpp at boot)
- Returns current values as JSON: `{"disp":N,"led":N}`

The web UI adds two range sliders (Display / LED Strip) and an Incognito Mode checkbox that sets both to 0. On page load, current brightness values are fetched from the device.

Settings persist across reboots since main.cpp already loads from the `"ouispy-br"` NVS namespace at boot.

### Files Modified

| File | Change |
|------|--------|
| `src/raw/detector.cpp` | Added `.br-panel` CSS, brightness panel HTML + JS, `/brightness` endpoint |
| `src/raw/foxhunter.cpp` | Added `.br-panel` CSS, brightness panel HTML + JS, `/brightness` endpoint |
| `src/raw/flockyou.cpp` | Added `.br-panel` CSS, brightness panel HTML + JS in TOOLS tab, `/brightness` endpoint on `fyServer` |
| `src/mode_foxhunter.cpp` | Added `#include "led_strip.h"` (was missing) |
| `src/mode_flockyou.cpp` | Added `#include "led_strip.h"` (was missing) |

---

## 2. TFT Display — Initial Screen After Boot

Previously, all modes showed the boot splash ("OUI-SPY / MODE NAME / Initializing...") and never updated the TFT until a specific event occurred (BLE detection, scan restart, etc.). This could leave the display stuck on "Initializing..." indefinitely.

Now each mode draws its proper initial screen immediately after setup completes:

| Mode | Initial Display |
|------|----------------|
| Detector (config mode) | Scanning screen with filter count |
| Detector (scanning mode) | Scanning screen with filter count |
| Foxhunter (config mode) | Foxhunter screen showing "CONFIG" status |
| Foxhunter (tracking mode) | Foxhunter screen showing "SEARCHING" status |
| Flock-You | Flock-You screen with zero detections |
| Sky Spy | Sky Spy scanning screen with zero drones |

### Files Modified

| File | Change |
|------|--------|
| `src/raw/detector.cpp` | Added `display_detector_scanning()` at end of `startConfigMode()` and `startScanningMode()` |
| `src/raw/foxhunter.cpp` | Added `display_foxhunter()` at end of `startConfigMode()` and `startTrackingMode()` |
| `src/raw/flockyou.cpp` | Added `display_flockyou()` at end of `setup()` |
| `src/raw/skyspy.cpp` | Added `display_skyspy_scanning()` at end of `setup()` |

---

## 3. Foxhunter Web Page CSS Fix

The Foxhunter config page was rendering with a plain white background and no styling. Root cause: the `generateConfigHTML()` function injected ~54KB of ASCII art into the HTML String via `getASCIIArt()`, which exhausted ESP32 heap memory during String concatenation. The detector already had this same fix applied (comment: `// Remove ASCII art - causes memory exhaustion on ESP32`).

### Fix

Removed the `getASCIIArt()` call from the foxhunter HTML template, leaving the `ascii-background` div empty (same approach as detector).

### File Modified

| File | Change |
|------|--------|
| `src/raw/foxhunter.cpp` | Replaced `getASCIIArt()` concatenation with empty div |

---

---

## 4. Freenove ESP32-S3 CYD Board Support

Added full support for the Freenove ESP32-S3 2.8" CYD (Cheap Yellow Display) as a third build target. The CYD runs Selector (mode 0), Flock-You (mode 4), and Sky Spy (mode 5) with a touch-based interface instead of physical buttons.

### Hardware

- **Display**: ILI9341 320x240, driven by TFT_eSPI with TFT_eSprite
- **Touch**: FT6336U capacitive touch via raw I2C (Wire, addr 0x38)
- **SD Card**: 4-bit SD_MMC (6-pin setPins)
- **GPS**: TinyGPS++ on Serial0 (UART0, GPIO 43/44, 9600 baud)
- **Speaker**: NS4150 amp (active-LOW) + ES8311 codec via I2S
- **NeoPixel**: WS2812 on GPIO 42

### CYD Audio System (I2S + ES8311)

The CYD uses an I2S audio path instead of direct PWM buzzer:
- ES8311 codec on I2C addr 0x18 (shares Wire bus with touch controller)
- Legacy `driver/i2s.h` API (ESP-IDF 4.x) — I2S pins: MCK=4, BCK=5, WS=7, DOUT=8, DIN=6
- FreeRTOS task generates sine wave samples, writes to I2S DMA buffer
- `cyd_tone(pin, freq, dur)` / `cyd_noTone(pin)` — non-blocking, pin param ignored
- Mode wrappers use `#define tone(p,f,d) cyd_tone(p,f,d)` to redirect raw file calls
- Volume (0-255) persisted in NVS `"ouispy-br"` namespace, adjustable via settings page

### Files Added

| File | Purpose |
|------|---------|
| `src/display_cyd.cpp` | CYD display implementation (ILI9341 320x240, touch, selector, settings) |
| `src/cyd_audio.cpp` | I2S audio engine with sine wave tone generation |
| `src/cyd_audio.h` | Audio API header |
| `src/es8311_driver.cpp` | ES8311 codec I2C driver (adapted from Freenove example) |
| `src/es8311_driver.h` | ES8311 driver header |

### Files Modified

| File | Change |
|------|--------|
| `platformio.ini` | Added `[env:freenove_cyd_s3]` with CYD-specific pins, TFT_eSPI config, build_src_filter |
| `src/board_config.h` | Added `BOARD_CYD_S3` section with pin definitions and feature flags |
| `src/main.cpp` | CYD-specific selector (touch-based, 3 modes), `#ifndef BOARD_CYD_S3` guards for Detector/Foxhunter |
| `src/sdlog.cpp` | CYD 4-bit SD_MMC init path |
| `src/mode_flockyou.cpp` | CYD pin overrides, tone macro redirect, `cyd_audio_init()` call |
| `src/mode_skyspy.cpp` | CYD pin overrides, tone macro redirect, `cyd_audio_init()` call |
| `src/raw/flockyou.cpp` | `#ifndef BOARD_CYD_S3` guard on buzzer/amp GPIO init |
| `src/raw/skyspy.cpp` | `#ifndef BOARD_CYD_S3` guard on buzzer/amp GPIO init |

---

## 5. CYD Audio Bug Fixes

Fixed two critical bugs that broke I2S audio when entering Flock-You or Sky Spy modes on the CYD.

### NS4150 Amp Polarity Fix

The NS4150 amplifier enable pin is active-LOW (LOW=on, HIGH=off). Both raw files were setting it HIGH (disabling the amp).

| File | Line | Fix |
|------|------|-----|
| `src/raw/flockyou.cpp` | ~1055 | `digitalWrite(SPEAKER_EN_PIN, HIGH)` -> `LOW` |
| `src/raw/skyspy.cpp` | ~398 | `digitalWrite(SPEAKER_EN_PIN, HIGH)` -> `LOW` |

### GPIO 8 Clobbering Fix

After `cyd_audio_init()` configures GPIO 8 as I2S DOUT, the raw files' `setup()` functions called `pinMode(BUZZER_PIN, OUTPUT)` which reconfigured GPIO 8 as a digital output, breaking I2S audio.

| File | Fix |
|------|-----|
| `src/raw/flockyou.cpp` | Wrapped buzzer + amp init block in `#ifndef BOARD_CYD_S3` |
| `src/raw/skyspy.cpp` | Wrapped buzzer + amp init block in `#ifndef BOARD_CYD_S3` |

---

## 6. Sky Spy WiFi Channel Hopping

Previously Sky Spy only scanned WiFi channel 6 (the NAN default). Now it hops across all 13 channels with weighted dwell times to maximize detection while prioritizing the NAN channel.

### Implementation

- Channel sequence: 6, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13
- CH6 dwell time: 500ms (NAN — most drone beacons)
- CH1-5, 7-13 dwell time: 100ms each
- Full cycle: ~1.7 seconds across all channels
- Current channel displayed in footer: `BLE:ON WiFi CH:N`

### Files Modified

| File | Change |
|------|--------|
| `src/raw/skyspy.cpp` | Added channel hop state machine, sequence array, dwell timers |
| `src/display.cpp` | Updated `display_skyspy()` / `display_skyspy_scanning()` to accept `wifiCh` param |
| `src/display_cyd.cpp` | Same display updates for CYD |
| `src/display.h` | Updated function signatures with `wifiCh`, `ledOn`, `toneOn` params |

---

## 7. Sky Spy LED & Tone Toggle Buttons (CYD)

Added touchable LED and TONE toggle buttons on the CYD Sky Spy display, allowing users to enable/disable the NeoPixel and speaker alerts independently.

### UI

- Two left-justified toggle buttons below the main content area
- LED button: toggles NeoPixel alerts (red flash on detection, dim green heartbeat when idle)
- TONE button: toggles speaker/buzzer alerts
- 6px touch padding around buttons for easier tapping
- Immediate display refresh on toggle to prevent perceived lag

### NeoPixel Behavior

| State | LED Behavior |
|-------|-------------|
| Drone detected | Red flash (synchronized with buzzer task) |
| Idle (LED on) | Dim green heartbeat blink every 10 seconds (80ms pulse, brightness 15) |
| Boot | Green flash |
| LED toggled off | Dark (no LED activity) |

### Persistence

Toggle states are saved to NVS (`ouispy-bz` namespace):
- `led` key — LED on/off state
- `ton` key — TONE on/off state

States are restored on boot so preferences persist across power cycles.

### Files Modified

| File | Change |
|------|--------|
| `src/raw/skyspy.cpp` | Added `ssLedOn` variable, NVS read/write, NeoPixel logic in buzzerTask, touch handling with immediate refresh, `refreshDisplay()` helper |
| `src/display_cyd.cpp` | Added `drawSsToggleButtons()`, `display_skyspy_touch()` with hit testing, button layout constants |
| `src/display.cpp` | Added `display_skyspy_touch()` stub (returns 0, no touch on T-Dongle) |
| `src/display.h` | Added `display_skyspy_touch()` declaration and inline no-op stub |

---

## 8. SD Card Improvements

### 64GB Card Support

Fixed SD card stats overflow on cards larger than 4GB. `SdLogStats` used `unsigned long` (32-bit on ESP32) which overflows at ~4GB. Changed to `uint64_t`.

Also fixed `sdlog_get_stats()` which was calling `SD_MMC.usedBytes()` instead of `SD_MMC.totalBytes()` for the total bytes field.

### Deferred Session File Creation

Previously, `sdlog_start_session()` created a new session file immediately on mode entry, resulting in empty `.jsonl` files when no detections occurred. Now files are only created on the first actual write via `openLogFileIfNeeded()`.

### Per-Mode File Count in Footer

Changed SD footer from `SD:N/NM` (used/total megabytes) to `SD:OK / Files:N` showing the number of session files for the current mode. Added `sdlog_mode_file_count(int mode)` API.

### Files Modified

| File | Change |
|------|--------|
| `src/sdlog.h` | Changed `SdLogStats` to `uint64_t`, added `sdlog_mode_file_count()` |
| `src/sdlog.cpp` | Fixed totalBytes bug, added deferred file creation, added `sdlog_mode_file_count()` |
| `src/main.cpp` | Updated `display_sd_status()` call to use per-mode file count |
| `src/display.cpp` | Updated SD footer format |
| `src/display_cyd.cpp` | Updated SD footer format |
| `src/display.h` | Updated `display_sd_status()` signature |

---

## 9. Code Cleanup for Release

### Include Ordering

Moved `board_config.h` to first include in all wrapper files for consistency with `mode_detector.cpp` and to prevent future conditional include issues.

| File | Change |
|------|--------|
| `src/mode_foxhunter.cpp` | Moved `board_config.h` to first include |
| `src/mode_flockyou.cpp` | Moved `board_config.h` to first include |
| `src/mode_skyspy.cpp` | Moved `board_config.h` to first include |

### Dead Code Removal

- Removed unused `writeTestTone()` function from `cyd_audio.cpp`
- Removed unused `BUZZER_DUTY` macro and `extern buzzerVolume` from `mode_skyspy.cpp`
- Removed unused `ssDispLed` / `ssDispTone` variables from `display_cyd.cpp`

### Debug Print Cleanup

- Trimmed 10 debug prints in `cyd_audio.cpp` to error lines + single `[CYD-AUDIO] Audio ready`
- Removed touch coordinate debug `Serial.printf` calls from CYD selector and settings
- Trimmed 5 heap diagnostic prints in `skyspy.cpp` to single summary line
- Fixed malformed JSON status message: `"scanning"` -> `{"status":"scanning"}`

### Display Logic Deduplication

Extracted `refreshDisplay()` helper in `skyspy.cpp` to eliminate ~45 lines of duplicated display update logic between the scanning and drone-detected display paths.

---

## Build Verification

All three environments build successfully with all changes:

| Environment | RAM | Flash |
|-------------|-----|-------|
| `seeed_xiao_esp32s3` | 25.3% | 18.0% |
| `lilygo_tdongle_s3` | 25.9% | 19.4% |
| `freenove_cyd_s3` | 26.5% | 19.0% |
