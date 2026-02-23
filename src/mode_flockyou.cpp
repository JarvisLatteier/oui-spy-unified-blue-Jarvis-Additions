/*
 * Mode 4: Flock-You
 * Surveillance device detector with web dashboard.
 * Scans BLE for Flock Safety, Raven, and surveillance device patterns.
 * Serves detection dashboard via WiFi AP "flockyou" / "flockyou123".
 * Detections stored in memory; exportable as JSON or CSV.
 */

// All includes from flock-you (outside namespace for proper linkage)
#include "board_config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include "modes.h"
#include "display.h"
#include "sdlog.h"
#include "led_strip.h"

#if HAS_NEOPIXEL
#include <Adafruit_NeoPixel.h>
#endif

#if HAS_GPS
#include <TinyGPSPlus.h>
#endif

#if HAS_SD_CARD
#include <SD_MMC.h>
#include <map>
#include <set>
#include <string>
#endif

// Pin overrides for T-Dongle-S3
#ifdef BOARD_TDONGLE_S3
  #undef BUZZER_PIN
  #define BUZZER_PIN -1
#endif

// CYD: buzzer enabled (BUZZER_PIN 8 from board_config.h)

// CYD: redirect tone()/noTone() to I2S-based cyd_tone()/cyd_noTone()
#ifdef BOARD_CYD_S3
#include "cyd_audio.h"
#define tone(p, f, d) cyd_tone(p, f, d)
#define noTone(p) cyd_noTone(p)
#endif

// Volume-scaled buzzer duty cycle
extern uint8_t buzzerVolume;
#define BUZZER_DUTY ((int)(buzzerVolume / 2))

// Rename setup/loop
#define setup flockyou_ns_setup
#define loop  flockyou_ns_loop

namespace {
#include "raw/flockyou.cpp"
} // anonymous namespace

#undef setup
#undef loop

void flockyou_setup() {
#ifdef BOARD_CYD_S3
    cyd_audio_init();
#endif
    flockyou_ns_setup();
}
void flockyou_loop()  { flockyou_ns_loop(); }
