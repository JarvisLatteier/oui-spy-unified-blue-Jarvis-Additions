/*
 * Mode 2: OUI Spy Foxhunter
 * Single-target RSSI proximity tracker with real-time beeping.
 * Wraps the original foxhunter firmware in an anonymous namespace.
 */

// All includes from the original foxhunter (outside namespace)
#include "board_config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <esp_wifi.h>
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

// Volume-scaled buzzer duty cycle
extern uint8_t buzzerVolume;
#define BUZZER_DUTY ((int)(buzzerVolume / 2))

// Rename setup/loop
#define setup foxhunter_ns_setup
#define loop  foxhunter_ns_loop

namespace {
#include "raw/foxhunter.cpp"
} // anonymous namespace

#undef setup
#undef loop

void foxhunter_setup() { foxhunter_ns_setup(); }
void foxhunter_loop()  { foxhunter_ns_loop(); }
