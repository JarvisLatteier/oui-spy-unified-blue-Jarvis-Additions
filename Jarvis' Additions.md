# Jarvis' Additions

---

# User Guide

## Supported Hardware

| Board | Modes | Display | Controls |
|-------|-------|---------|----------|
| Seeed XIAO ESP32-S3 | All (1, 2, 4, 5) | None | BOOT button, NeoPixel LED, buzzer |
| LILYGO T-Dongle-S3 | All (1, 2, 4, 5) | 0.96" ST7735 (160x80) | BOOT button, APA102 LED, SD card |
| Freenove ESP32-S3 CYD | Flock-You (4), Sky Spy (5) | 2.8" ILI9341 (320x240) touch | Touch screen, BOOT button, NeoPixel, speaker, SD card, GPS |

> 📷 **[Photo: all three boards side by side — XIAO, T-Dongle, and CYD]**

## Getting Started

### First Boot

On first power-up the device enters **Selector Mode** (mode 0). From here you choose which firmware mode to run.

### Returning to Selector Mode

**From any mode**, hold the **BOOT button** (GPIO0) for **1.5 seconds** to return to the Selector menu. The device will beep 3 times to confirm and reboot.

- On XIAO/T-Dongle: the BOOT button is the small tactile button on the board
- On CYD: the BOOT button is on the back of the board
- Works even if a mode's setup hangs (a background watchdog monitors the button)

## Mode 0: Selector

### XIAO / T-Dongle (Button + Web UI)

1. Connect to WiFi AP **oui-spy** (password: **ouispy123**)
2. Open **http://192.168.4.1** in a browser
3. Tap a mode to select it — the device reboots into that mode

**TFT menu (T-Dongle only):**
- **Short press** BOOT button to cycle through modes on the display
- **Long press** (1.5s) BOOT button to confirm and boot into the highlighted mode

**Web UI controls:**
- **SSID / PASSWORD fields** — customize the AP name and password, tap SET to save (reboots)
- **BZR checkbox** — enable/disable buzzer for all modes
- **INCOGNITO checkbox** — kills display and LED for covert operation
- **DSP slider** — display backlight brightness (0-255)
- **LED slider** — LED strip brightness (0-255)

### CYD (Touch Screen)

The CYD shows 3 large touch buttons:
- **FLOCK-YOU** — tap to boot into Flock-You mode
- **SKY SPY** — tap to boot into Sky Spy mode
- **SETTINGS** — brightness, volume, buzzer, and incognito controls

> 📷 **[Photo: CYD selector screen showing the three touch buttons]**

**Settings page controls:**
- **DSP +/-** — display backlight brightness
- **LED +/-** — NeoPixel brightness
- **VOL +/-** — speaker volume
- **BZR** — buzzer enable/disable toggle
- **INCOGNITO** — kills display and LED
- **BACK** — return to mode selector

> 📷 **[Photo: CYD settings page with all sliders and toggles]**

All settings persist across reboots.

## Mode 1: Detector (XIAO / T-Dongle only)

Scans for specific BLE devices by MAC address or OUI prefix and alerts when they're detected.

1. Connect to WiFi AP **snoopuntothem** (password: **astheysnoopuntous**)
2. Open **http://192.168.4.1**
3. Enter OUI prefixes (XX:XX:XX) and/or full MAC addresses to monitor
4. Configure buzzer and LED alerts
5. Tap **Save** — scanning begins immediately

**Web dashboard features:**
- Live detection list with MAC, RSSI, alias, and timestamp
- Assign custom aliases to detected devices
- Clear device history
- Lock configuration to prevent accidental edits
- Brightness sliders (display + LED)

## Mode 2: Foxhunter (XIAO / T-Dongle only)

Track down a single BLE device using RSSI signal strength — beeps get faster as you get closer.

1. Connect to WiFi AP **foxhunter** (password: **foxhunter**)
2. Open **http://192.168.4.1**
3. Enter the target MAC address and tap **Save**
4. Walk around — beep speed indicates proximity:

| Signal Strength | Beep Interval |
|----------------|---------------|
| Very weak (-95 to -85 dBm) | 3 seconds |
| Weak (-85 to -75 dBm) | 500-1000ms |
| Medium (-75 to -55 dBm) | 100-200ms |
| Strong (-55 to -35 dBm) | 10-50ms |

**T-Dongle display** shows a large RSSI number with a color-coded proximity bar (red → yellow → green).

> 📷 **[Photo: T-Dongle display showing Foxhunter RSSI bar and signal strength]**

If no configuration is entered within 20 seconds, Foxhunter auto-switches to tracking mode with the last saved target.

## Mode 4: Flock-You

Detects Flock Safety surveillance cameras and Raven gunshot detectors via BLE.

1. Device creates WiFi AP **flockyou** (password: **flockyou123**)
2. Open **http://192.168.4.1** for the live dashboard

**Detection methods:**
- 20 known Flock Safety MAC OUI prefixes
- BLE device name matching (Flock, FS Ext Battery, Penguin, Pigvision)
- BLE manufacturer ID (0x09C8 / XUNTONG)
- Raven gunshot detector service UUIDs (8 patterns)

**Dashboard features:**
- Real-time detection list with signal strength
- Detection statistics and Raven count
- **Export options**: JSON, CSV, or KML (for Google Earth)
- Session history with archiving
- **History tab** — persistent cross-session device registry
- GPS wardriving — your phone's browser can share its location via the dashboard, tagging each detection with coordinates

> 📷 **[Photo: Flock-You web dashboard main detection tab]**

> 📷 **[Photo: Flock-You web dashboard HIST tab showing persistent registry]**

**GPS (CYD only):** The onboard GPS module automatically tags detections with coordinates without needing a phone.

### CYD Detection Display

The CYD shows each detection with a **NEW** or **×N** badge:
- **NEW** (yellow) — first time this camera has been seen across all sessions
- **×N** (dim) — camera has been seen N total times in previous sessions

> 📷 **[Photo: CYD showing a Flock-You detection with NEW badge]**

### Persistent Device Registry

Flock-You maintains a cross-session camera database on the SD card at `/oui-spy/flockyou/registry.json`. This lets you build a picture of camera density in an area over multiple sessions.

**Per-device record:**
- MAC address (primary key)
- Device type (`flock` or `raven`)
- Total lifetime sightings
- GPS coordinates (if available)
- First seen / last seen timestamps

**How it works:**
- Registry loads at mode startup; new detections update it in memory and write to SD
- Writes are atomic: temporary file written first, then renamed over the live registry
- Registry capped at ~1000 entries; oldest (by last_seen) are evicted if over limit
- Web dashboard exposes `/api/registry` — the HIST tab uses this to display the full history

## Mode 5: Sky Spy

Monitors for FAA Remote ID (Open Drone ID) broadcasts from drones over WiFi and BLE.

**What it detects:**
- DJI drones (OcuSync 3.0+) and all FAA-compliant commercial UAVs
- Extracts: drone ID, operator ID, GPS coordinates, altitude, speed, heading

**WiFi channel hopping:**
- Prioritizes channel 6 (NAN discovery, 500ms dwell)
- Scans channels 1-13 (100ms each) for non-NAN beacons
- Full sweep every ~1.7 seconds

**Display footer:** `BLE:ON WiFi CH:N` shows the current WiFi channel being scanned.

**On drone detection:**
- Display shows drone ID, MAC, altitude, RSSI, and GPS coordinates
- Buzzer sounds alert tone
- NeoPixel flashes red (if LED enabled)
- Footer updates to `Total:N BLE:ON WiFi CH:N`

### CYD Sky Spy — Scanning Screen

When no drone is active the CYD scanning screen shows:
- Left half: scan stats, drone count, GPS coordinates (when GPS fix acquired)
- Right half: **LIVE MAP QR** — scan to connect and open the live drone dashboard

> 📷 **[Photo: CYD Sky Spy scanning screen with LIVE MAP QR in right panel]**

### CYD Sky Spy — Drone Detected Screen

When a drone is detected:
- Drone ID, MAC, altitude, RSSI, and GPS coordinates fill the left panel
- Right panel shows an **Apple Maps QR code** — scan to open turn-by-turn directions from pilot launch point to current drone position (or a single pin centered on the pilot if no drone position is available)
- **NEW DRONE** (yellow) / **×N REPEAT** (dim) badge indicates whether this operator has been seen before across sessions

> 📷 **[Photo: CYD Sky Spy drone detected screen with NEW or REPEAT badge]**

### CYD Live Tracking — WiFi Dashboard

The CYD creates a WiFi access point **skyspy-live** (password: `skyspy123`) on channel 6 alongside drone scanning. Connect your phone and open `http://192.168.4.1` to see a live auto-refreshing drone dashboard.

**The dashboard shows:**
- All drones seen in the last 30 seconds with ID, MAC, altitude, RSSI, speed, heading
- Pilot launch position (blue) when available
- **OPEN IN MAPS** button → Google Maps directions from pilot to drone
- Current WiFi channel and GPS satellite count in the header

**Note — phone loses internet while connected to skyspy-live** because the ESP32 AP has no upstream internet. Two options:

- **Android:** After joining, tap the "No internet" notification → **Stay connected**. Your phone will use cellular data for internet while talking to the ESP32 over WiFi.
- **iOS:** Go to Settings → Wi-Fi → tap `ⓘ` next to **skyspy-live** → **Use Without Internet**. This persists until you forget the network.

> 📷 **[Photo: Sky Spy live dashboard on phone browser showing drone card with OPEN IN MAPS]**

### CYD Live Tracking — BLE Push (recommended: no internet interruption)

The CYD also runs a **BLE GATT peripheral** (Nordic UART Service) named **skyspy-live** that pushes drone data directly to your phone every 2 seconds over Bluetooth — no WiFi required, your phone keeps its internet connection the entire time.

**BLE payload** is a JSON notification pushed on every refresh:
```json
{"id":"UA12345","mac":"aa:bb:cc:dd:ee:ff","lat":34.1234,"lon":-118.1234,
 "alt":100,"rssi":-65,"spd":5,"hdg":270,"plat":34.1235,"plon":-118.1235,"ch":6,"sats":8}
```
When no drone is present: `{"id":"","ch":6,"sats":8}`

#### Android — Web Bluetooth (recommended)

The CYD serves a Web Bluetooth client page at `http://192.168.4.1/ble`. Download it once while connected to the WiFi AP, then use it forever from local storage without any WiFi connection.

1. Connect your phone to **skyspy-live** WiFi
2. Open `http://192.168.4.1/ble` in **Chrome**
3. Use Chrome's menu (⋮) → **Download** to save `skyspy-ble.html` to your phone
4. Disconnect from skyspy-live WiFi — your internet is restored
5. Open the **Files** app → tap `skyspy-ble.html` → opens in Chrome
6. Tap **CONNECT BLE** → select **skyspy-live** from the picker
7. Drone data appears and updates live; tap **OPEN IN MAPS** for Google Maps directions

> 📷 **[Photo: Sky Spy BLE web page on Android showing drone card with OPEN IN MAPS]**

#### iOS — nRF Connect or LightBlue

Web Bluetooth is not supported in iOS Safari. Use the free **nRF Connect** (Nordic Semiconductor) or **LightBlue** (PunchThrough) app:

1. Open **nRF Connect** → **Scanner** tab → find **skyspy-live**
2. Tap **Connect**
3. Expand **Unknown Service** (UUID: `6E400001-...`)
4. Find characteristic `6E400003-...` (TX — ESP32 → phone)
5. Tap the **▼ (subscribe)** or **notify** button
6. Drone JSON data appears in the notification log every 2 seconds
7. Copy coordinates from the JSON and paste into Maps manually

> 📷 **[Photo: nRF Connect app showing skyspy-live connected with drone JSON notification]**

#### BLE Service Details

| Field | Value |
|-------|-------|
| Device name | `skyspy-live` |
| Service UUID | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` (Nordic UART) |
| TX Characteristic | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` (notify, ESP32→phone) |
| Update rate | Every 2 seconds |
| MTU | 247 bytes |

### CYD Touch Controls (Sky Spy)

Two toggle buttons appear on the CYD display:
- **LED** — toggle NeoPixel alerts on/off
- **TONE** — toggle speaker alerts on/off

Both settings persist across reboots.

**NeoPixel behavior when LED is enabled:**
| State | LED |
|-------|-----|
| Drone detected | Red flash |
| Idle | Dim green heartbeat every 10 seconds |
| Boot | Green flash |

### Persistent Drone Registry

Sky Spy maintains a cross-session drone operator database on the SD card at `/oui-spy/skyspy/registry.json`.

**Per-operator record:**
- UAS ID (FAA-assigned, primary key — stable across flights)
- Operator ID
- Sessions seen (count of separate power-cycle detection sessions)
- First seen / last seen timestamps
- Pilot location centroid (averaged across all sessions)
- Most recent pilot launch location
- Last seen BLE MAC

**How it works:**
- Registry loads at mode startup
- First detection of an operator per session increments their session count and updates the centroid
- Write-back is atomic (tmp → rename) and only triggered on new entries or new session detections, not every display refresh
- The **×N** badge on the CYD drone screen shows how many separate sessions this operator has been seen in — after a few passes in an area, repeat operators become obvious immediately

## SD Card Logging

The T-Dongle and CYD support SD card logging. Insert a FAT32-formatted micro SD card (up to 64GB tested).

**Log structure:**
```
/oui-spy/
  detector/session_000001.jsonl
  foxhunter/session_000001.jsonl
  flockyou/
    session_000001.jsonl
    registry.json
  skyspy/
    session_000001.jsonl
    registry.json
```

- Logs are JSONL format (one JSON object per line)
- Session numbers auto-increment each boot
- Files are only created when a detection occurs (no empty files)
- `registry.json` files are persistent cross-session databases (see Flock-You and Sky Spy sections above)
- Display footer shows `SD:OK / Files:N` with per-mode file count

**Sky Spy track log format** — each line is one drone position snapshot:
```json
{"t":12453,"mac":"aa:bb:cc:dd:ee:ff","rssi":-68,
 "drone_lat":37.123456,"drone_long":-122.123456,
 "drone_altitude":45,"alt_agl":30,
 "spd":3,"hdg":180,
 "pilot_lat":37.121000,"pilot_long":-122.121000,
 "basic_id":"HX12345"}
```
`t` is milliseconds since session start — sort by `t` to replay the flight in order.

**How fast do files grow?**

FAA Remote ID mandates 1 broadcast per second per drone. At ~220 bytes per record:

| Session | Approximate size |
|---------|-----------------|
| 1 drone, 30 minutes | ~400 KB |
| 1 drone, 1 hour | ~800 KB |
| 100 sessions (heavy field use) | ~40 MB |

A 4 GB card at normal usage rates would take years to fill.

**Low-space protection:**

The firmware monitors free space at the start of each session:
- Below **50 MB free** — footer changes from `SD:OK` to `SD:LOW`. Logging continues; delete old session files soon.
- Below **5 MB free** — footer shows `SD:FULL`. Writes are suspended to prevent filesystem corruption.

To free space, remove the SD card, connect to a computer, and delete session files from `/oui-spy/skyspy/` or other mode folders. The `registry.json` files are small and worth keeping.

**Accessing logs:**
- Remove the SD card and read on a computer
- In Selector mode: use the web API endpoints:
  - `http://192.168.4.1/sd/list` — list all log files as JSON
  - `http://192.168.4.1/sd/download?file=skyspy/session_000001.jsonl` — download a file
  - `http://192.168.4.1/sd/clear?confirm=yes` — delete all logs
- In Flock-You mode: use the dashboard export buttons (JSON/CSV/KML) or the HIST tab for registry data

## Privacy Features

- **MAC randomization** — the device generates a random MAC address on every boot
- **Incognito mode** — kills display and LED for covert operation
- **No cloud** — all data stays on the device and SD card, nothing is transmitted

## AP Credentials Quick Reference

| Mode | Default SSID | Default Password | IP Address | Notes |
|------|-------------|-----------------|------------|-------|
| Selector | oui-spy | ouispy123 | 192.168.4.1 | SSID/password configurable via web UI |
| Detector | snoopuntothem | astheysnoopuntous | 192.168.4.1 | |
| Foxhunter | foxhunter | foxhunter | 192.168.4.1 | |
| Flock-You | flockyou | flockyou123 | 192.168.4.1 | |
| Sky Spy (CYD) | skyspy-live | skyspy123 | 192.168.4.1 | WiFi dashboard + BLE GATT push |
| Sky Spy (XIAO/T-Dongle) | *(none)* | — | — | Passive scanning only |

**Sky Spy CYD also advertises as a BLE peripheral** named `skyspy-live`. Connecting via BLE (nRF Connect on iOS, Web Bluetooth on Android) lets your phone receive live drone data without losing its internet connection.

---

# Technical Changes

## 1. Brightness / LED / Incognito Controls in All Boot Modes

Previously, display brightness, LED strip brightness, and incognito mode controls only existed in the Selector (mode 0) web UI. Now these controls are available in every mode that has a web server:

- **Detector (mode 1)** — in the "Audio & Visual Settings" section
- **Foxhunter (mode 2)** — in the "Audio & Visual Settings" section
- **Flock-You (mode 4)** — in the TOOLS tab under "DISPLAY SETTINGS"

Sky Spy (mode 5) uses its own separate web server (CYD only); the `/brightness` endpoint is not wired to it.

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
- Removed 4 DEBUG Serial prints from `foxhunter.cpp` (solid beep, beep ON/OFF, target RSSI)
- Fixed malformed JSON status message: `"scanning"` -> `{"status":"scanning"}`

### Display Logic Deduplication

Extracted `refreshDisplay()` helper in `skyspy.cpp` to eliminate ~45 lines of duplicated display update logic between the scanning and drone-detected display paths.

---

## 10. Sky Spy Persistent Drone Registry

Upgraded Sky Spy from a flat in-session set to a rich JSON registry persisted across power cycles on the SD card.

### Registry File

`/oui-spy/skyspy/registry.json` — atomically written (tmp → rename for crash safety).

### Data Model

```json
{
  "abc123": {
    "sessions": 4,
    "pilot_lat": 37.7749,
    "pilot_lon": -122.4194,
    "pilot_lat_last": 37.7751,
    "pilot_lon_last": -122.4190,
    "samples": 12,
    "op_id": "FAAoperatorXX",
    "mac": "AA:BB:CC:DD:EE:FF"
  }
}
```

**Per-operator fields:**
- `sessions` — count of separate power-cycle sessions this operator was detected in
- `pilot_lat/lon` — running centroid of all pilot GPS readings (builds over time to reveal habitual launch sites)
- `pilot_lat/lon_last` — most recent pilot GPS from last detection
- `samples` — total pilot location samples used in centroid
- `op_id` — FAA operator ID (from Remote ID broadcast)
- `mac` — last seen BLE MAC

### CYD Display Badge

The drone detection screen shows a session badge in the top-right corner:
- **NEW DRONE** (yellow) — UAS ID not seen in any previous session
- **×N** (dim) — operator seen in N previous sessions

### Implementation Details

- `ssRegistry`: `std::map<std::string, SsRegEntry>` loaded at mode startup
- `ssSessionIds`: `std::set<std::string>` tracking which operators have been registered in the current session (prevents double-counting sessions on re-detection)
- `ssRegistryUpdate()` returns `sessions_seen` (uint32_t); saves only on new entry or first detection per session (not every 2-second display refresh)
- STL headers (`<map>`, `<set>`, `<string>`, `<ArduinoJson.h>`) included in `mode_skyspy.cpp` before `namespace {}` — critical for correct linkage in anonymous namespace pattern

### Files Modified

| File | Change |
|------|--------|
| `src/raw/skyspy.cpp` | `SsRegEntry` struct, `ssRegistry` map, `ssSessionIds` set, `ssRegistryLoad/Save/Update()` |
| `src/mode_skyspy.cpp` | Added STL + ArduinoJson includes before `namespace {}` |
| `src/display.h` | `bool isNew` → `int sessionsSeen` in `display_skyspy()` signature |
| `src/display.cpp` | Updated parameter in T-Dongle stub |
| `src/display_cyd.cpp` | Badge rendering: 0=none, 1=NEW (yellow), >1=×N (dim) |

---

## 11. Flock-You Persistent Device Registry

Added a cross-session camera database to Flock-You, persisted on the SD card and exposed via a new HIST tab in the web dashboard.

### Registry File

`/oui-spy/flockyou/registry.json` — atomically written (tmp → rename for crash safety).

### Data Model

```json
{
  "AA:BB:CC:DD:EE:FF": {
    "type": "flock",
    "sightings": 47,
    "lat": 37.7749,
    "lon": -122.4194,
    "hasGPS": true
  }
}
```

**Per-device fields:**
- `type` — `"flock"` or `"raven"`
- `sightings` — total detections across all sessions
- `lat/lon` — GPS coordinates from last geotagged sighting
- `hasGPS` — whether coordinates are available

### CYD Display Badge

Each detection on the CYD shows:
- **NEW** (yellow) — MAC not seen in any previous session
- **×N** (dim) — seen N total times across all sessions

### Web Dashboard — HIST Tab

A fifth tab added to the Flock-You dashboard loads the persistent registry from `/api/registry` and displays each known device with its MAC, type, total sightings, and GPS coordinates (if available).

> 📷 **[Photo: Flock-You HIST tab showing the persistent device registry table]**

### BLE-Safe SD Writes

BLE `onResult` callbacks run on the BLE task. Performing SD I/O from that context risks blocking the BLE stack. Solution:
- `fyRegistryUpdate()` (called from BLE callback): updates in-memory map only, sets `fyRegistryDirty` flag
- `fyRegistrySave()` (called from `loop()`): performs the actual SD write when dirty flag is set

### Files Modified

| File | Change |
|------|--------|
| `src/raw/flockyou.cpp` | `FYRegEntry` struct, `fyRegistry` map, `fyRegMutex`, `fyRegistryDirty` flag, `fyRegistryLoad/Save/Update()`, `/api/registry` endpoint, HIST tab HTML + JS |
| `src/mode_flockyou.cpp` | Added STL includes (`<map>`, `<set>`, `<string>`) before `namespace {}` |
| `src/display_cyd.cpp` | NEW/×N badge in `display_flockyou()` |

---

## 12. QR Code Layout Fix (CYD Sky Spy)

The GPS coordinate QR code on both Sky Spy screens (scanning and drone-detected) was vertically centered in the right panel with the label below it. Repositioned to:
- Bottom-anchored — sits just above the footer, matching the visual weight of the SETTINGS button on the selector screen
- "DEVICE GPS" label rendered above the QR (font 2) instead of below it

### Calculation

```
qrWrTop  = FTR_Y - 4 - (qrPixels + 8)   // white border rect top
qrY      = qrWrTop + 4                   // first module row
qrLabel  = qrWrTop - 20                  // label Y (above rect)
```

### Files Modified

| File | Change |
|------|--------|
| `src/display_cyd.cpp` | Updated `display_skyspy_scanning()` and `display_skyspy()` QR layout |

---

## 13. Sky Spy CYD — WiFi Dashboard + BLE GATT Live Tracking

Added two complementary live-tracking channels to Sky Spy on the CYD. Previously Sky Spy was fully passive (no AP, no outbound data). The CYD now simultaneously:

1. **Serves a WiFi dashboard** via a softAP on channel 6 (`skyspy-live` / `skyspy123`), auto-refreshing every 2 seconds at `http://192.168.4.1`
2. **Advertises a BLE GATT peripheral** (Nordic UART Service) named `skyspy-live` that pushes drone JSON notifications every 2 seconds to any subscribed phone — no WiFi required, phone keeps its internet connection

### Why Both?

| Method | Internet preserved | iOS | Android | UX |
|--------|-------------------|-----|---------|-----|
| WiFi AP | No (bypass available) | ✓ | ✓ | Rich map dashboard |
| BLE GATT | **Yes** | nRF Connect | Web BT / nRF Connect | Best long-term |

### WiFi AP

- `WIFI_AP_STA` mode — AP fixed on channel 6 (NAN dwell channel), promiscuous STA continues channel hopping
- `AsyncWebServer` on port 80: `/` (WiFi dashboard), `/api/live` (JSON array of all drones seen < 30s ago), `/ble` (downloadable Web BT client page)
- Dashboard HTML (`SS_HTML`) stored in `PROGMEM` (~1.7KB)
- QR on scanning screen changed from GPS coordinates → `http://192.168.4.1` (version 2, 4px modules, always shown, labeled "LIVE MAP")
- QR on drone screen links to Apple Maps (`maps.apple.com/?saddr=PILOT&daddr=DRONE`), falling back to single-point pin on pilot position if drone coords unavailable

### BLE GATT Push

- NimBLE concurrent central (scanning) + peripheral (GATT server) — both run simultaneously
- Device name changed to `"skyspy-live"` on CYD (was `"DroneID"`)
- Nordic UART Service (NUS) — universally recognized by nRF Connect, LightBlue, and Web Bluetooth
- `NimBLEDevice::setMTU(247)` requested for full JSON payload headroom
- `ssDroneChar->notify()` called in `refreshDisplay()` on every 2s timer tick
- Drone present: full JSON with id, mac, lat, lon, alt, rssi, spd, hdg, plat, plon, ch, sats
- No drone: `{"id":"","ch":N,"sats":N}` (keeps phone UI alive with channel info)
- On phone disconnect: advertising restarts automatically (`SsGattCallbacks::onDisconnect`)

### Web Bluetooth Client Page (`/ble`)

Served from the ESP32 at `/ble` with `Content-Disposition: inline; filename="skyspy-ble.html"`. User downloads once while on the WiFi AP, then opens from local storage (`file://`) in Chrome on Android — Web Bluetooth is permitted in secure contexts including `file://` origins. Page detects `navigator.bluetooth` being undefined and shows platform-specific fallback instructions automatically.

### Files Modified

| File | Change |
|------|--------|
| `src/mode_skyspy.cpp` | Added `#include <AsyncTCP.h>`, `<ESPAsyncWebServer.h>`, `<NimBLEServer.h>` before namespace |
| `src/raw/skyspy.cpp` | `WIFI_AP_STA` + `softAP`, GATT globals + `SsGattCallbacks`, `SS_BLE_HTML`, `ssSetupGatt()`, `ssSetupServer()` `/ble` endpoint, BLE notify in `refreshDisplay()` |
| `src/display_cyd.cpp` | Scanning QR → `http://192.168.4.1` (v2, 4px, LIVE MAP, always drawn). Drone QR → Google Maps |

---

## 14. Sky Spy Flight Track Logging + SD Low-Space Guard

### Flight Track Log Fields

Expanded the Sky Spy session JSONL to include a full set of flight track fields. Previously logged: `mac`, `rssi`, `drone_lat/long`, `drone_altitude`, `pilot_lat/long`, `basic_id`. Now also logs:

| Field | Description |
|-------|-------------|
| `t` | Milliseconds since session start — enables chronological replay and gap detection |
| `alt_agl` | Height above ground level (metres) |
| `spd` | Horizontal speed (m/s) |
| `hdg` | Heading (degrees, 0–360) |

JSON buffer increased from 256 to 320 bytes to accommodate the wider format.

### SD Card Low-Space Guard

Added proactive free-space monitoring to prevent silent data loss when the SD card fills up.

**Thresholds:**
- `SD_WARN_MB` = 50 MB — sets `_sdLow` flag, footer shows `SD:LOW / Files:N`
- `SD_CRIT_MB` = 5 MB — sets `_sdFull` flag, writes suspended, footer shows `SD:FULL`

Space is checked at the start of each session (`sdlog_start_session()`). Two new public functions expose the state:
- `sdlog_is_low()` — returns true below 50 MB
- `sdlog_is_full()` — returns true below 5 MB

`sdlog_write()` returns early (no write, no crash) when `_sdFull` is set.

### Files Modified

| File | Change |
|------|--------|
| `src/raw/skyspy.cpp` | Added `ssSessionStartMs`, `t`/`alt_agl`/`spd`/`hdg` to `send_json_fast()`, buffer 256→320 |
| `src/sdlog.h` | Added `sdlog_is_low()`, `sdlog_is_full()` declarations and inline no-op stubs |
| `src/sdlog.cpp` | Added `SD_WARN_MB`/`SD_CRIT_MB` thresholds, `_sdLow`/`_sdFull` flags, `checkSpace()`, guard in `sdlog_write()` |
| `src/display.h` | Updated `display_sd_status()` signature with `low`/`full` params (defaulted) |
| `src/display.cpp` | Updated T-Dongle stub signature |
| `src/display_cyd.cpp` | Added `sd_low`/`sd_full` state, `SD:LOW` / `SD:FULL` footer rendering |
| `src/main.cpp` | Passes `sdlog_is_low()`, `sdlog_is_full()` to `display_sd_status()` |

---

## Build Verification

All three environments build successfully with all changes:

| Environment | RAM | Flash |
|-------------|-----|-------|
| `seeed_xiao_esp32s3` | 27.3% | 18.1% |
| `lilygo_tdongle_s3` | 27.9% | 19.7% |
| `freenove_cyd_s3` | 28.4% | 19.6% |
