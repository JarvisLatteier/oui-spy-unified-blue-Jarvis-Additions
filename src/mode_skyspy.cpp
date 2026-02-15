/*
 * Mode 5: Sky Spy - Open Drone ID Detector
 * Monitors WiFi and BLE for FAA Remote ID broadcasts from drones.
 * Ported from classic ESP32 BLE to NimBLE via compatibility macros.
 * Wraps the original Sky-Spy firmware in an anonymous namespace.
 */

// All includes (outside namespace) - NimBLE instead of classic BLE
#include "board_config.h"
#include <Arduino.h>
#include <HardwareSerial.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include "opendroneid.h"
#include "odid_wifi.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Preferences.h>
#include "modes.h"
#include "display.h"
#include "sdlog.h"
#include "led_strip.h"

// Pin overrides for T-Dongle-S3
#ifdef BOARD_TDONGLE_S3
  #undef BUZZER_PIN
  #define BUZZER_PIN -1
  #undef LED_PIN
  #define LED_PIN -1
#endif

// Pin overrides for CYD: buzzer enabled, no LED
#ifdef BOARD_CYD_S3
  #undef LED_PIN
  #define LED_PIN -1
#endif

// CYD: redirect tone()/noTone() to I2S-based cyd_tone()/cyd_noTone()
#ifdef BOARD_CYD_S3
#include "cyd_audio.h"
#define tone(p, f, d) cyd_tone(p, f, d)
#define noTone(p) cyd_noTone(p)
#endif

#if HAS_GPS
#include <TinyGPSPlus.h>
#endif

// Rename setup/loop
#define setup skyspy_ns_setup
#define loop  skyspy_ns_loop

namespace {
#include "raw/skyspy.cpp"
} // anonymous namespace

#undef setup
#undef loop

void skyspy_setup() {
#ifdef BOARD_CYD_S3
    cyd_audio_init();
#endif
    skyspy_ns_setup();
}
void skyspy_loop()  { skyspy_ns_loop(); }
