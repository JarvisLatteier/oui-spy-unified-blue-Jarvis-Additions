/*
 * Display Abstraction API
 *
 * When ENABLE_TFT_DISPLAY is defined, calls go to the TFT_eSPI implementation.
 * Otherwise all functions are no-op inline stubs (zero overhead on XIAO builds).
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

#ifdef ENABLE_TFT_DISPLAY

void display_init();
void display_boot_splash(const char* modeName);
void display_selector(int index, const char* ssid, const char* ip);
void display_detector_event(const char* mac, int rssi, const char* alias,
                            const char* type, int count);
void display_detector_scanning(int filters, int devices);
void display_foxhunter(int rssi, const char* mac, bool detected,
                       const char* status);
void display_flockyou(int count, int inRange, bool buzzer, const char* mac,
                      const char* method, int rssi, int gpsSats = 0,
                      const char* name = nullptr, int sightings = 0,
                      bool isRaven = false, const char* ravenFW = nullptr,
                      bool gpsTagged = false, const char* alias = nullptr);
void display_skyspy(const char* mac, const char* id, double lat, double lon,
                    int alt, int rssi, int total,
                    double pilotLat = 0, double pilotLon = 0,
                    int gpsSats = 0, int wifiCh = 6, int sessionsSeen = 0);
void display_skyspy_scanning(int count, double devLat = 0, double devLon = 0,
                             int gpsSats = 0, int wifiCh = 6);
void display_sd_status(bool inserted, int fileCount, bool low = false, bool full = false);
int  display_skyspy_touch();  // returns 1=SETTINGS, 0=none
void display_skyspy_settings(uint8_t dispBr, uint8_t ledBr, uint8_t vol,
                             bool bleOn, bool incognito);
void display_clear();
void display_backlight(bool on);
void display_set_brightness(uint8_t level);

// Settings page (CYD only, no-op on other boards)
void display_settings(uint8_t dispBr, uint8_t ledBr, uint8_t vol,
                      bool buzzer, bool incognito);
int  display_settings_touch();  // returns action code (0=none, 9=back)

// Sprite lifecycle (CYD: free 150KB before BLE init, recreate after)
void display_free_sprite();
void display_create_sprite();

// Touch support (CYD only, no-op on other boards)
void display_touch_init();
bool display_touch_read(int16_t &x, int16_t &y);
int  display_selector_touch();  // returns mode number if tapped, -1 if no tap, 99 if settings
bool display_touch_calibration_load();
void display_touch_calibrate();

#else // No display — inline no-ops

inline void display_init() {}
inline void display_boot_splash(const char*) {}
inline void display_selector(int, const char*, const char*) {}
inline void display_detector_event(const char*, int, const char*,
                                   const char*, int) {}
inline void display_detector_scanning(int, int) {}
inline void display_foxhunter(int, const char*, bool, const char*) {}
inline void display_flockyou(int, int, bool, const char*, const char*, int,
                             int = 0, const char* = nullptr, int = 0,
                             bool = false, const char* = nullptr,
                             bool = false, const char* = nullptr) {}
inline void display_skyspy(const char*, const char*, double, double,
                           int, int, int,
                           double = 0, double = 0, int = 0, int = 6, int = 0) {}
inline void display_skyspy_scanning(int, double = 0, double = 0, int = 0, int = 6) {}
inline void display_sd_status(bool, int, bool = false, bool = false) {}
inline int  display_skyspy_touch() { return 0; }
inline void display_skyspy_settings(uint8_t, uint8_t, uint8_t, bool, bool) {}
inline void display_clear() {}
inline void display_backlight(bool) {}
inline void display_set_brightness(uint8_t) {}
inline void display_settings(uint8_t, uint8_t, uint8_t, bool, bool) {}
inline int  display_settings_touch() { return 0; }
inline void display_free_sprite() {}
inline void display_create_sprite() {}
inline void display_touch_init() {}
inline bool display_touch_read(int16_t &x, int16_t &y) { return false; }
inline int  display_selector_touch() { return -1; }
inline bool display_touch_calibration_load() { return false; }
inline void display_touch_calibrate() {}

#endif // ENABLE_TFT_DISPLAY

#endif // DISPLAY_H
