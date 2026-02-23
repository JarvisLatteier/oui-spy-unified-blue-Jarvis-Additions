// ============================================================================
// FLOCK-YOU: Surveillance Device Detector with Web Dashboard
// ============================================================================
// Detection methods (BLE only - WiFi radio used for AP):
//   1. BLE MAC prefix matching (known Flock Safety OUIs)
//   2. BLE device name pattern matching (case-insensitive substring)
//   3. BLE manufacturer company ID matching (0x09C8 XUNTONG) [from wgreenberg]
//   4. Raven gunshot detector service UUID matching
//   5. Raven firmware version estimation from service UUID patterns
//
// WiFi AP "flockyou" / "flockyou123" serves web dashboard at 192.168.4.1
// All detections stored in memory, exportable as JSON or CSV
// Optional WiFi STA connection for future features
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include "esp_wifi.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

#ifndef BUZZER_PIN
#define BUZZER_PIN 3
#endif

// Audio
#define LOW_FREQ 200
#define HIGH_FREQ 800
#define DETECT_FREQ 1000
#define HEARTBEAT_FREQ 600
#define BOOT_BEEP_DURATION 300
#define DETECT_BEEP_DURATION 150
#define HEARTBEAT_DURATION 100

// BLE scanning
#define BLE_SCAN_DURATION 2      // seconds per scan
#define BLE_SCAN_INTERVAL 3000   // ms between scans

// Detection storage
#define MAX_DETECTIONS 200

// WiFi AP credentials
#define FY_AP_SSID "flockyou"
#define FY_AP_PASS "flockyou123"

// ============================================================================
// DETECTION PATTERNS
// ============================================================================

// Known Flock Safety MAC address prefixes (OUIs)
static const char* mac_prefixes[] = {
    // FS Ext Battery devices
    "58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84",
    "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9",
    // Flock WiFi devices
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "14:5a:fc",
    "74:4c:a1", "08:3a:88", "9c:2f:9d", "94:08:53", "e4:aa:ea"
};

// BLE device name patterns (matched case-insensitive substring)
static const char* device_name_patterns[] = {
    "FS Ext Battery",
    "Penguin",
    "Flock",
    "Pigvision"
};

// BLE Manufacturer Company IDs
// Source: wgreenberg/flock-you - XUNTONG ID associated with Flock Safety devices
static const uint16_t ble_manufacturer_ids[] = {
    0x09C8   // XUNTONG
};

// ============================================================================
// RAVEN SURVEILLANCE DEVICE UUID PATTERNS
// ============================================================================

#define RAVEN_DEVICE_INFO_SERVICE   "0000180a-0000-1000-8000-00805f9b34fb"
#define RAVEN_GPS_SERVICE           "00003100-0000-1000-8000-00805f9b34fb"
#define RAVEN_POWER_SERVICE         "00003200-0000-1000-8000-00805f9b34fb"
#define RAVEN_NETWORK_SERVICE       "00003300-0000-1000-8000-00805f9b34fb"
#define RAVEN_UPLOAD_SERVICE        "00003400-0000-1000-8000-00805f9b34fb"
#define RAVEN_ERROR_SERVICE         "00003500-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_HEALTH_SERVICE    "00001809-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_LOCATION_SERVICE  "00001819-0000-1000-8000-00805f9b34fb"

static const char* raven_service_uuids[] = {
    RAVEN_DEVICE_INFO_SERVICE,
    RAVEN_GPS_SERVICE,
    RAVEN_POWER_SERVICE,
    RAVEN_NETWORK_SERVICE,
    RAVEN_UPLOAD_SERVICE,
    RAVEN_ERROR_SERVICE,
    RAVEN_OLD_HEALTH_SERVICE,
    RAVEN_OLD_LOCATION_SERVICE
};

// ============================================================================
// DETECTION STORAGE
// ============================================================================

struct FYDetection {
    char mac[18];
    char name[48];
    int rssi;
    char method[24];
    unsigned long firstSeen;
    unsigned long lastSeen;
    int count;
    bool isRaven;
    char ravenFW[16];
    // GPS from phone (wardriving)
    double gpsLat;
    double gpsLon;
    float gpsAcc;
    bool hasGPS;
    char alias[32];   // user-assigned label, persisted in registry
};

static FYDetection fyDet[MAX_DETECTIONS];
static int fyDetCount = 0;
static SemaphoreHandle_t fyMutex = NULL;

// ============================================================================
// GLOBALS
// ============================================================================

static bool fyBuzzerOn = true;
static unsigned long fyLastBleScan = 0;
static bool fyTriggered = false;
static bool fyDeviceInRange = false;
static unsigned long fyLastDetTime = 0;
static unsigned long fyLastHB = 0;
static NimBLEScan* fyBLEScan = NULL;
static AsyncWebServer fyServer(80);

// GPS state (fed by onboard GPS if HAS_GPS, or by phone browser via /api/gps)
static double fyGPSLat = 0;
static double fyGPSLon = 0;
static float  fyGPSAcc = 0;
static bool   fyGPSValid = false;
static unsigned long fyGPSLastUpdate = 0;
#define GPS_STALE_MS 30000  // GPS considered stale after 30s without update
static bool   fyGPSOnboard = false;  // true once hardware GPS provides a valid fix

#if HAS_GPS
static TinyGPSPlus fyGPS;
#endif

#if HAS_NEOPIXEL
static Adafruit_NeoPixel fyPixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
static bool fyPixelAlertMode = false;
static unsigned long fyPixelAlertStart = 0;
#endif

// Session persistence (SPIFFS)
#define FY_SESSION_FILE  "/session.json"
#define FY_PREV_FILE     "/prev_session.json"
#define FY_SAVE_INTERVAL 15000  // Auto-save every 15 seconds (prevent data loss on quick power-cycle)
static unsigned long fyLastSave = 0;
static int fyLastSaveCount = 0;  // Track changes to avoid unnecessary writes
static bool fySpiffsReady = false;

// ============================================================================
// AUDIO SYSTEM
// ============================================================================

static void fyBeep(int freq, int dur) {
    if (!fyBuzzerOn) return;
#if HAS_BUZZER
    tone(BUZZER_PIN, freq, dur);
#endif
    delay(dur + 50);
}

// Crow caw: harsh descending sweep with warble texture
static void fyCaw(int startFreq, int endFreq, int durationMs, int warbleHz) {
    if (!fyBuzzerOn) return;
#if HAS_BUZZER
    int steps = durationMs / 8;  // 8ms per step
    float fStep = (float)(endFreq - startFreq) / steps;
    for (int i = 0; i < steps; i++) {
        int f = startFreq + (int)(fStep * i);
        // Add warble: oscillate frequency +/- for raspy texture
        if (warbleHz > 0 && (i % 3 == 0)) {
            f += ((i % 6 < 3) ? warbleHz : -warbleHz);
        }
        if (f < 100) f = 100;
        tone(BUZZER_PIN, f, 10);
        delay(8);
    }
    noTone(BUZZER_PIN);
#else
    delay(durationMs);
#endif
}

static void fyBootBeep() {
    printf("[FLOCK-YOU] Boot sound (buzzer %s)\n", fyBuzzerOn ? "ON" : "OFF");
    if (!fyBuzzerOn) return;

    // === CROW CALL SEQUENCE ===
    // Caw 1: sharp descending caw
    fyCaw(850, 380, 180, 40);
    delay(100);

    // Caw 2: slightly lower, shorter
    fyCaw(780, 350, 150, 50);
    delay(100);

    // Caw 3: longer trailing caw with more rasp
    fyCaw(820, 280, 220, 60);
    delay(80);

    // Quick staccato ending "kk-kk"
#if HAS_BUZZER
    tone(BUZZER_PIN, 600, 25); delay(40);
    tone(BUZZER_PIN, 550, 25); delay(40);
    noTone(BUZZER_PIN);
#endif

    printf("[FLOCK-YOU] *caw caw caw*\n");
}

#if HAS_NEOPIXEL
// ============================================================================
// NEOPIXEL EFFECTS
// ============================================================================

static void fyHsvToRgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1));
    float m = v - c;
    float r1 = 0, g1 = 0, b1 = 0;
    if      (h < 60)  { r1 = c; g1 = x; }
    else if (h < 120) { r1 = x; g1 = c; }
    else if (h < 180) { g1 = c; b1 = x; }
    else if (h < 240) { g1 = x; b1 = c; }
    else if (h < 300) { r1 = x; b1 = c; }
    else              { r1 = c; b1 = x; }
    r = (uint8_t)((r1 + m) * 255);
    g = (uint8_t)((g1 + m) * 255);
    b = (uint8_t)((b1 + m) * 255);
}

// Slow purple breathing while idle
static void fyPixelBreathing() {
    float t = (millis() % 4000) / 4000.0f;
    float v = 0.3f + 0.7f * (0.5f + 0.5f * sinf(t * 2 * (float)M_PI));
    uint8_t r, g, b;
    fyHsvToRgb(270, 1.0f, v, r, g, b);
    fyPixel.setPixelColor(0, fyPixel.Color(r, g, b));
    fyPixel.show();
}

// 3 rapid red/pink flashes on detection (~750ms total)
static void fyPixelDetection() {
    unsigned long elapsed = millis() - fyPixelAlertStart;
    if (elapsed >= 750) {
        fyPixelAlertMode = false;
        fyPixel.setPixelColor(0, 0);
        fyPixel.show();
        return;
    }
    bool on = (elapsed % 250) < 150;
    if (on) {
        uint8_t r, g, b;
        fyHsvToRgb(340, 0.8f, 1.0f, r, g, b);  // red-pink
        fyPixel.setPixelColor(0, fyPixel.Color(r, g, b));
    } else {
        fyPixel.setPixelColor(0, 0);
    }
    fyPixel.show();
}

// Dim steady pink glow while device is actively in range
static void fyPixelHeartbeat() {
    uint8_t r, g, b;
    fyHsvToRgb(300, 0.7f, 0.4f, r, g, b);
    fyPixel.setPixelColor(0, fyPixel.Color(r, g, b));
    fyPixel.show();
}

static void fyUpdatePixel() {
    if (fyPixelAlertMode) {
        fyPixelDetection();
    } else if (fyDeviceInRange) {
        fyPixelHeartbeat();
    } else {
        fyPixelBreathing();
    }
}
#endif  // HAS_NEOPIXEL

static void fyDetectBeep() {
    printf("[FLOCK-YOU] Detection alert!\n");
#if HAS_NEOPIXEL
    fyPixelAlertMode = true;
    fyPixelAlertStart = millis();
#endif
    if (!fyBuzzerOn) return;
    // Alarm crow: two sharp ascending chirps then a caw
    fyCaw(400, 900, 100, 30);   // rising alarm chirp
    delay(60);
    fyCaw(450, 950, 100, 30);   // second chirp, higher
    delay(60);
    fyCaw(900, 350, 200, 50);   // descending caw
}

static void fyHeartbeat() {
    if (!fyBuzzerOn) return;
    // Soft double coo - like a distant crow
    fyCaw(500, 400, 80, 20);
    delay(120);
    fyCaw(480, 380, 80, 20);
}

// ============================================================================
// DETECTION HELPERS
// ============================================================================

static bool checkMACPrefix(const uint8_t* mac) {
    char mac_str[9];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
    for (size_t i = 0; i < sizeof(mac_prefixes)/sizeof(mac_prefixes[0]); i++) {
        if (strncasecmp(mac_str, mac_prefixes[i], 8) == 0) return true;
    }
    return false;
}

static bool checkDeviceName(const char* name) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < sizeof(device_name_patterns)/sizeof(device_name_patterns[0]); i++) {
        if (strcasestr(name, device_name_patterns[i])) return true;
    }
    return false;
}

static bool checkManufacturerID(uint16_t id) {
    for (size_t i = 0; i < sizeof(ble_manufacturer_ids)/sizeof(ble_manufacturer_ids[0]); i++) {
        if (ble_manufacturer_ids[i] == id) return true;
    }
    return false;
}

// ============================================================================
// RAVEN UUID DETECTION
// ============================================================================

static bool checkRavenUUID(NimBLEAdvertisedDevice* device, char* out_uuid = nullptr) {
    if (!device || !device->haveServiceUUID()) return false;
    int count = device->getServiceUUIDCount();
    if (count == 0) return false;
    for (int i = 0; i < count; i++) {
        NimBLEUUID svc = device->getServiceUUID(i);
        std::string str = svc.toString();
        for (size_t j = 0; j < sizeof(raven_service_uuids)/sizeof(raven_service_uuids[0]); j++) {
            if (strcasecmp(str.c_str(), raven_service_uuids[j]) == 0) {
                if (out_uuid) strncpy(out_uuid, str.c_str(), 40);
                return true;
            }
        }
    }
    return false;
}

static const char* estimateRavenFW(NimBLEAdvertisedDevice* device) {
    if (!device || !device->haveServiceUUID()) return "?";
    bool has_new_gps = false, has_old_loc = false, has_power = false;
    int count = device->getServiceUUIDCount();
    for (int i = 0; i < count; i++) {
        std::string u = device->getServiceUUID(i).toString();
        if (strcasecmp(u.c_str(), RAVEN_GPS_SERVICE) == 0)          has_new_gps = true;
        if (strcasecmp(u.c_str(), RAVEN_OLD_LOCATION_SERVICE) == 0) has_old_loc = true;
        if (strcasecmp(u.c_str(), RAVEN_POWER_SERVICE) == 0)        has_power = true;
    }
    if (has_old_loc && !has_new_gps) return "1.1.x";
    if (has_new_gps && !has_power)   return "1.2.x";
    if (has_new_gps && has_power)    return "1.3.x";
    return "?";
}

// ============================================================================
// GPS HELPERS
// ============================================================================

static bool fyGPSIsFresh() {
    return fyGPSValid && (millis() - fyGPSLastUpdate < GPS_STALE_MS);
}

static void fyAttachGPS(FYDetection& d) {
    if (fyGPSIsFresh()) {
        d.hasGPS = true;
        d.gpsLat = fyGPSLat;
        d.gpsLon = fyGPSLon;
        d.gpsAcc = fyGPSAcc;
    }
}

// ============================================================================
// PERSISTENT DEVICE REGISTRY (cross-session SD card tracking)
// ============================================================================

#if HAS_SD_CARD
static const uint16_t FY_REG_MAX        = 500;
static const char     FY_REG_PATH[]     = "/oui-spy/flockyou/registry.json";
static const char     FY_REG_TMP_PATH[] = "/oui-spy/flockyou/registry.tmp";

struct FYRegEntry {
    char     type[8];      // "flock" or "raven"
    uint32_t sightings;    // sessions seen across all power cycles
    double   lat;          // last GPS lat
    double   lon;          // last GPS lon
    bool     hasGPS;
    char     alias[32];    // user-assigned label
};

static std::map<std::string, FYRegEntry> fyRegistry;
static std::set<std::string>             fySessionMacs;   // MACs updated this session
static SemaphoreHandle_t                 fyRegMutex = nullptr;
static volatile bool                     fyRegistryDirty = false;

static void fyRegistryLoad() {
    File f = SD_MMC.open(FY_REG_PATH, "r");
    if (!f) { Serial.println("[FY] No registry yet"); return; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { Serial.printf("[FY] Registry parse error: %s\n", err.c_str()); return; }
    for (JsonObject e : doc["entries"].as<JsonArray>()) {
        const char* mac = e["mac"] | "";
        if (!mac[0]) continue;
        FYRegEntry r = {};
        r.sightings = e["s"]   | 1u;
        r.lat       = e["lat"] | 0.0;
        r.lon       = e["lon"] | 0.0;
        r.hasGPS    = e["gps"] | false;
        strncpy(r.type,  e["tp"] | "flock", sizeof(r.type)  - 1);
        strncpy(r.alias, e["al"] | "",      sizeof(r.alias) - 1);
        fyRegistry[mac] = r;
    }
    Serial.printf("[FY] Registry: %u known devices\n", (unsigned)fyRegistry.size());
}

// Called from loop() — SD write outside BLE callback context
static void fyRegistrySave() {
    if (!fyRegMutex || xSemaphoreTake(fyRegMutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    File f = SD_MMC.open(FY_REG_TMP_PATH, FILE_WRITE);
    if (!f) {
        Serial.println("[FY] Registry save: open failed");
        xSemaphoreGive(fyRegMutex);
        return;
    }
    JsonDocument doc;
    doc["v"] = 1;
    JsonArray arr = doc["entries"].to<JsonArray>();
    for (auto& kv : fyRegistry) {
        JsonObject e = arr.add<JsonObject>();
        e["mac"]  = kv.first.c_str();
        e["tp"]   = kv.second.type;
        e["s"]    = kv.second.sightings;
        e["lat"]  = kv.second.lat;
        e["lon"]  = kv.second.lon;
        e["gps"]  = kv.second.hasGPS;
        if (kv.second.alias[0]) e["al"] = kv.second.alias;
    }
    serializeJson(doc, f);
    f.close();
    SD_MMC.remove(FY_REG_PATH);
    SD_MMC.rename(FY_REG_TMP_PATH, FY_REG_PATH);
    fyRegistryDirty = false;
    xSemaphoreGive(fyRegMutex);
    Serial.printf("[FY] Registry saved (%u entries)\n", (unsigned)fyRegistry.size());
}

// Called from BLE callback — map update only, no SD I/O
// Returns lifetime sightings for this device (1=new, >1=repeat). 0 if mac empty.
// If alias_out is non-null and the registry has an alias for this device, copies it there.
static uint32_t fyRegistryUpdate(const char* mac, bool isRaven,
                                  double lat, double lon,
                                  char* alias_out = nullptr) {
    if (!mac || !mac[0]) return 0;
    if (!fyRegMutex || xSemaphoreTake(fyRegMutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    bool isNew     = !fyRegistry.count(mac);
    bool firstThis = !fySessionMacs.count(mac);

    // Evict lowest-sightings entry if at capacity
    if (isNew && fyRegistry.size() >= FY_REG_MAX) {
        auto evict = fyRegistry.begin();
        for (auto it = fyRegistry.begin(); it != fyRegistry.end(); ++it) {
            if (it->second.sightings < evict->second.sightings) evict = it;
        }
        fyRegistry.erase(evict);
    }

    FYRegEntry& e = fyRegistry[mac];
    if (isNew) {
        e = {};
        e.sightings = 1;
        strncpy(e.type, isRaven ? "raven" : "flock", sizeof(e.type) - 1);
        fySessionMacs.insert(mac);
        fyRegistryDirty = true;
    } else if (firstThis) {
        e.sightings++;
        fySessionMacs.insert(mac);
        fyRegistryDirty = true;
    }

    if (lat != 0.0 || lon != 0.0) {
        e.lat    = lat;
        e.lon    = lon;
        e.hasGPS = true;
    }

    uint32_t result = e.sightings;
    if (alias_out && e.alias[0]) {
        strncpy(alias_out, e.alias, 31);
        alias_out[31] = '\0';
    }
    xSemaphoreGive(fyRegMutex);
    return result;
}
#endif

// ============================================================================
// DETECTION MANAGEMENT
// ============================================================================

static int fyAddDetection(const char* mac, const char* name, int rssi,
                          const char* method, bool isRaven = false,
                          const char* ravenFW = "") {
    if (!fyMutex || xSemaphoreTake(fyMutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;

    // Update existing by MAC
    for (int i = 0; i < fyDetCount; i++) {
        if (strcasecmp(fyDet[i].mac, mac) == 0) {
            fyDet[i].count++;
            fyDet[i].lastSeen = millis();
            fyDet[i].rssi = rssi;
            if (name && name[0]) {
                strncpy(fyDet[i].name, name, sizeof(fyDet[i].name) - 1);
            }
            // Update GPS on every re-sighting (captures movement)
            fyAttachGPS(fyDet[i]);
            xSemaphoreGive(fyMutex);
            return i;
        }
    }

    // Add new
    if (fyDetCount < MAX_DETECTIONS) {
        FYDetection& d = fyDet[fyDetCount];
        memset(&d, 0, sizeof(d));
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
        // Sanitize name for JSON safety
        if (name) {
            for (int j = 0; j < (int)sizeof(d.name) - 1 && name[j]; j++) {
                d.name[j] = (name[j] == '"' || name[j] == '\\') ? '_' : name[j];
            }
        }
        d.rssi = rssi;
        strncpy(d.method, method, sizeof(d.method) - 1);
        d.firstSeen = millis();
        d.lastSeen = millis();
        d.count = 1;
        d.isRaven = isRaven;
        strncpy(d.ravenFW, ravenFW ? ravenFW : "", sizeof(d.ravenFW) - 1);
        // Attach GPS from phone
        fyAttachGPS(d);
        int idx = fyDetCount++;
        xSemaphoreGive(fyMutex);
        return idx;
    }

    xSemaphoreGive(fyMutex);
    return -1;
}

// ============================================================================
// BLE SCANNING
// ============================================================================

class FYBLECallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        NimBLEAddress addr = dev->getAddress();
        std::string addrStr = addr.toString();

        // Safe MAC byte extraction
        unsigned int m[6];
        sscanf(addrStr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
        uint8_t mac[6] = {(uint8_t)m[0], (uint8_t)m[1], (uint8_t)m[2],
                          (uint8_t)m[3], (uint8_t)m[4], (uint8_t)m[5]};

        int rssi = dev->getRSSI();
        std::string name = dev->haveName() ? dev->getName() : "";

        bool detected = false;
        const char* method = "";
        bool isRaven = false;
        const char* ravenFW = "";

        // 1. Check MAC prefix against known Flock Safety OUIs
        if (checkMACPrefix(mac)) {
            detected = true;
            method = "mac_prefix";
        }

        // 2. Check BLE device name patterns
        if (!detected && !name.empty() && checkDeviceName(name.c_str())) {
            detected = true;
            method = "device_name";
        }

        // 3. Check BLE manufacturer company IDs (from wgreenberg/flock-you)
        if (!detected) {
            for (int i = 0; i < (int)dev->getManufacturerDataCount(); i++) {
                std::string data = dev->getManufacturerData(i);
                if (data.size() >= 2) {
                    uint16_t code = ((uint16_t)(uint8_t)data[1] << 8) |
                                     (uint16_t)(uint8_t)data[0];
                    if (checkManufacturerID(code)) {
                        detected = true;
                        method = "ble_mfr_id";
                        break;
                    }
                }
            }
        }

        // 4. Check Raven gunshot detector service UUIDs
        if (!detected) {
            char detUUID[41] = {0};
            if (checkRavenUUID(dev, detUUID)) {
                detected = true;
                method = "raven_uuid";
                isRaven = true;
                ravenFW = estimateRavenFW(dev);
            }
        }

        if (detected) {
            int idx = fyAddDetection(addrStr.c_str(), name.c_str(), rssi,
                                     method, isRaven, ravenFW);

            // Human-readable log
            printf("[FLOCK-YOU] DETECTED: %s %s RSSI:%d [%s] count:%d\n",
                   addrStr.c_str(), name.c_str(), rssi, method,
                   idx >= 0 ? fyDet[idx].count : 0);

            // JSON serial output (Flask-compatible format for live ingestion)
            // Build GPS fragment if available
            char gpsBuf[80] = "";
            if (fyGPSIsFresh()) {
                snprintf(gpsBuf, sizeof(gpsBuf),
                    ",\"gps\":{\"latitude\":%.8f,\"longitude\":%.8f,\"accuracy\":%.1f}",
                    fyGPSLat, fyGPSLon, fyGPSAcc);
            }
            if (isRaven) {
                printf("{\"detection_method\":\"%s\",\"protocol\":\"bluetooth_le\","
                       "\"mac_address\":\"%s\",\"device_name\":\"%s\","
                       "\"rssi\":%d,\"is_raven\":true,\"raven_fw\":\"%s\"%s}\n",
                       method, addrStr.c_str(), name.c_str(), rssi, ravenFW, gpsBuf);
            } else {
                printf("{\"detection_method\":\"%s\",\"protocol\":\"bluetooth_le\","
                       "\"mac_address\":\"%s\",\"device_name\":\"%s\","
                       "\"rssi\":%d%s}\n",
                       method, addrStr.c_str(), name.c_str(), rssi, gpsBuf);
            }

            if (!fyTriggered) {
                fyTriggered = true;
                fyDetectBeep();
            }
            fyDeviceInRange = true;
            fyLastDetTime = millis();
            fyLastHB = millis();

#if HAS_SD_CARD
            char regAlias[32] = {};
            int lifetimeSightings = (int)fyRegistryUpdate(
                addrStr.c_str(), isRaven,
                fyGPSIsFresh() ? fyGPSLat : 0.0,
                fyGPSIsFresh() ? fyGPSLon : 0.0,
                regAlias
            );
            // Propagate alias into in-session detection record
            if (idx >= 0 && regAlias[0]) {
                if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    strncpy(fyDet[idx].alias, regAlias, sizeof(fyDet[idx].alias) - 1);
                    xSemaphoreGive(fyMutex);
                }
            }
#else
            char regAlias[32] = {};
            int lifetimeSightings = 0;
#endif
#ifdef ENABLE_TFT_DISPLAY
            display_flockyou(fyDetCount, fyDeviceInRange ? 1 : 0,
                             fyBuzzerOn, addrStr.c_str(), method, rssi
#if HAS_GPS
                             , (int)fyGPS.satellites.value()
#else
                             , 0
#endif
                             , name.c_str()
                             , lifetimeSightings
                             , isRaven
                             , isRaven ? ravenFW : nullptr
                             , idx >= 0 && fyDet[idx].hasGPS
                             , regAlias[0] ? regAlias : nullptr
                             );
#endif
#if HAS_SD_CARD
            { char buf[256];
              int n = snprintf(buf, sizeof(buf),
                "{\"ts\":%lu,\"mac\":\"%s\",\"rssi\":%d,\"method\":\"%s\",\"is_raven\":%s",
                millis(), addrStr.c_str(), rssi, method,
                isRaven ? "true" : "false");
              if (fyGPSIsFresh()) {
                n += snprintf(buf + n, sizeof(buf) - n,
                  ",\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f",
                  fyGPSLat, fyGPSLon, fyGPSAcc);
              }
              snprintf(buf + n, sizeof(buf) - n, "}");
              sdlog_write(4, buf); }
#endif
        }
    }
};

// ============================================================================
// JSON HELPER
// ============================================================================

static void writeDetectionsJSON(AsyncResponseStream *resp) {
    resp->print("[");
    if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        for (int i = 0; i < fyDetCount; i++) {
            if (i > 0) resp->print(",");
            resp->printf(
                "{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
                "\"first\":%lu,\"last\":%lu,\"count\":%d,"
                "\"raven\":%s,\"fw\":\"%s\"",
                fyDet[i].mac, fyDet[i].name, fyDet[i].rssi, fyDet[i].method,
                fyDet[i].firstSeen, fyDet[i].lastSeen, fyDet[i].count,
                fyDet[i].isRaven ? "true" : "false", fyDet[i].ravenFW);
            if (fyDet[i].alias[0]) {
                resp->printf(",\"alias\":\"%s\"", fyDet[i].alias);
            }
            // Append GPS if present
            if (fyDet[i].hasGPS) {
                resp->printf(",\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}",
                    fyDet[i].gpsLat, fyDet[i].gpsLon, fyDet[i].gpsAcc);
            }
            resp->print("}");
        }
        xSemaphoreGive(fyMutex);
    }
    resp->print("]");
}

// ============================================================================
// SESSION PERSISTENCE (SPIFFS)
// ============================================================================

static void fySaveSession() {
    if (!fySpiffsReady || !fyMutex) return;
    if (xSemaphoreTake(fyMutex, pdMS_TO_TICKS(300)) != pdTRUE) return;

    File f = SPIFFS.open(FY_SESSION_FILE, "w");
    if (!f) { xSemaphoreGive(fyMutex); return; }

    f.print("[");
    for (int i = 0; i < fyDetCount; i++) {
        if (i > 0) f.print(",");
        FYDetection& d = fyDet[i];
        f.printf("{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
                 "\"first\":%lu,\"last\":%lu,\"count\":%d,"
                 "\"raven\":%s,\"fw\":\"%s\"",
                 d.mac, d.name, d.rssi, d.method,
                 d.firstSeen, d.lastSeen, d.count,
                 d.isRaven ? "true" : "false", d.ravenFW);
        if (d.hasGPS) {
            f.printf(",\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}", d.gpsLat, d.gpsLon, d.gpsAcc);
        }
        f.print("}");
    }
    f.print("]");
    f.close();
    fyLastSaveCount = fyDetCount;
    printf("[FLOCK-YOU] Session saved: %d detections\n", fyDetCount);
    xSemaphoreGive(fyMutex);
}

static void fyPromotePrevSession() {
    // Copy current session to prev_session on boot, then delete original
    // NOTE: SPIFFS.rename() is unreliable on ESP32 — use copy+delete instead
    if (!fySpiffsReady) return;
    if (!SPIFFS.exists(FY_SESSION_FILE)) {
        printf("[FLOCK-YOU] No prior session file to promote\n");
        return;
    }

    File src = SPIFFS.open(FY_SESSION_FILE, "r");
    if (!src) {
        printf("[FLOCK-YOU] Failed to open session file for promotion\n");
        return;
    }
    String data = src.readString();
    src.close();

    if (data.length() == 0) {
        printf("[FLOCK-YOU] Session file empty, skipping promotion\n");
        SPIFFS.remove(FY_SESSION_FILE);
        return;
    }

    // Write to prev_session (overwrite any existing)
    File dst = SPIFFS.open(FY_PREV_FILE, "w");
    if (!dst) {
        printf("[FLOCK-YOU] Failed to create prev_session file\n");
        return;
    }
    dst.print(data);
    dst.close();

    // Delete the old session file so it doesn't get re-promoted next boot
    SPIFFS.remove(FY_SESSION_FILE);
    printf("[FLOCK-YOU] Prior session promoted: %d bytes\n", data.length());
}

// ============================================================================
// KML EXPORT
// ============================================================================

static void writeDetectionsKML(AsyncResponseStream *resp) {
    resp->print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
                "<name>Flock-You Detections</name>\n"
                "<description>Surveillance device detections with GPS</description>\n");

    // Detection pin style
    resp->print("<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
                "<scale>1.0</scale></IconStyle></Style>\n"
                "<Style id=\"raven\"><IconStyle><color>ff4444ef</color>"
                "<scale>1.2</scale></IconStyle></Style>\n");

    if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        for (int i = 0; i < fyDetCount; i++) {
            FYDetection& d = fyDet[i];
            if (!d.hasGPS) continue;  // Skip detections without GPS
            resp->print("<Placemark>\n");
            resp->printf("<name>%s</name>\n", d.mac);
            resp->printf("<styleUrl>#%s</styleUrl>\n", d.isRaven ? "raven" : "det");
            resp->print("<description><![CDATA[");
            if (d.name[0]) resp->printf("<b>Name:</b> %s<br/>", d.name);
            resp->printf("<b>Method:</b> %s<br/>"
                         "<b>RSSI:</b> %d dBm<br/>"
                         "<b>Count:</b> %d<br/>",
                         d.method, d.rssi, d.count);
            if (d.isRaven) resp->printf("<b>Raven FW:</b> %s<br/>", d.ravenFW);
            resp->printf("<b>Accuracy:</b> %.1f m", d.gpsAcc);
            resp->print("]]></description>\n");
            resp->printf("<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n",
                         d.gpsLon, d.gpsLat);
            resp->print("</Placemark>\n");
        }
        xSemaphoreGive(fyMutex);
    }
    resp->print("</Document>\n</kml>");
}

// All-time wardriving KML — generated from persistent registry (all sessions)
#if HAS_SD_CARD
static void writeRegistryKML(AsyncResponseStream *resp) {
    resp->print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
                "<name>Flock-You All-Time Wardriving</name>\n"
                "<description>All GPS-tagged detections across all sessions</description>\n");
    resp->print("<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
                "<scale>1.0</scale></IconStyle></Style>\n"
                "<Style id=\"raven\"><IconStyle><color>ff4444ef</color>"
                "<scale>1.2</scale></IconStyle></Style>\n");

    if (fyRegMutex && xSemaphoreTake(fyRegMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (auto& kv : fyRegistry) {
            if (!kv.second.hasGPS) continue;
            resp->print("<Placemark>\n");
            if (kv.second.alias[0])
                resp->printf("<name>%s (%s)</name>\n", kv.second.alias, kv.first.c_str());
            else
                resp->printf("<name>%s</name>\n", kv.first.c_str());
            resp->printf("<styleUrl>#%s</styleUrl>\n",
                         strcmp(kv.second.type, "raven") == 0 ? "raven" : "det");
            resp->print("<description><![CDATA[");
            resp->printf("<b>Type:</b> %s<br/>"
                         "<b>Sightings:</b> %u<br/>",
                         kv.second.type, kv.second.sightings);
            if (kv.second.alias[0])
                resp->printf("<b>Alias:</b> %s<br/>", kv.second.alias);
            resp->print("]]></description>\n");
            resp->printf("<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n",
                         kv.second.lon, kv.second.lat);
            resp->print("</Placemark>\n");
        }
        xSemaphoreGive(fyRegMutex);
    }
    resp->print("</Document>\n</kml>");
}
#endif // HAS_SD_CARD

// ============================================================================
// DASHBOARD HTML
// ============================================================================

static const char FY_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>FLOCK-YOU</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;overflow:hidden}
body{font-family:'Courier New',monospace;background:#0a0012;color:#e0e0e0;display:flex;flex-direction:column}
.hd{background:#1a0033;padding:10px 14px;border-bottom:2px solid #ec4899;flex-shrink:0}
.hd h1{font-size:22px;color:#ec4899;letter-spacing:3px}
.hd .sub{font-size:11px;color:#8b5cf6;margin-top:2px}
.st{display:flex;gap:8px;padding:8px 12px;background:rgba(139,92,246,.08);border-bottom:1px solid rgba(139,92,246,.19);flex-shrink:0}
.sc{flex:1;text-align:center;padding:6px;border:1px solid rgba(139,92,246,.25);border-radius:5px}
.sc .n{font-size:22px;font-weight:bold;color:#ec4899}
.sc .l{font-size:10px;color:#8b5cf6;margin-top:2px}
.tb{display:flex;border-bottom:1px solid #8b5cf6;flex-shrink:0}
.tb button{flex:1;padding:9px;text-align:center;cursor:pointer;color:#8b5cf6;border:none;background:none;font-family:inherit;font-size:13px;font-weight:bold;letter-spacing:1px}
.tb button.a{color:#ec4899;border-bottom:2px solid #ec4899;background:rgba(236,72,153,.08)}
.cn{flex:1;overflow-y:auto;padding:10px}
.pn{display:none}.pn.a{display:block}
.det{background:rgba(45,27,105,.4);border:1px solid rgba(139,92,246,.25);border-radius:7px;padding:10px;margin-bottom:8px}
.det .mac{color:#ec4899;font-weight:bold;font-size:14px}
.det .nm{color:#c084fc;font-size:13px;margin-left:4px}
.det .inf{display:flex;flex-wrap:wrap;gap:5px;margin-top:5px;font-size:12px}
.det .inf span{background:rgba(139,92,246,.15);padding:3px 6px;border-radius:4px}
.det .rv{background:rgba(239,68,68,.15)!important;color:#ef4444;font-weight:bold}
.pg{margin-bottom:12px}
.pg h3{color:#ec4899;font-size:14px;margin-bottom:4px;border-bottom:1px solid rgba(139,92,246,.19);padding-bottom:4px}
.pg .it{display:flex;flex-wrap:wrap;gap:4px;font-size:12px}
.pg .it span{background:rgba(139,92,246,.15);padding:3px 6px;border-radius:4px;border:1px solid rgba(139,92,246,.12)}
.btn{display:block;width:100%;padding:10px;margin-bottom:8px;background:#8b5cf6;color:#fff;border:none;border-radius:5px;cursor:pointer;font-family:inherit;font-size:14px;font-weight:bold}
.btn:active{background:#ec4899}
.btn.dng{background:#ef4444}
.empty{text-align:center;color:rgba(139,92,246,.5);padding:28px;font-size:14px}
.sep{border:none;border-top:1px solid rgba(139,92,246,.12);margin:12px 0}
h4{color:#ec4899;font-size:14px;margin-bottom:8px}
.br-panel{margin-top:12px;padding:10px;background:rgba(139,92,246,.08);border:1px solid rgba(139,92,246,.2);border-radius:7px}
.br-panel label{display:block;color:#c084fc;font-size:12px;margin-bottom:3px}
.br-panel input[type=range]{width:100%;margin-bottom:6px;accent-color:#ec4899}
.br-panel .val{color:#ec4899;font-weight:bold}
.br-panel .incog{margin-top:6px;display:flex;align-items:center;gap:6px;font-size:12px}
.br-panel .incog input{accent-color:#ec4899;width:14px;height:14px}
</style></head><body>
<div class="hd"><h1>FLOCK-YOU</h1><div class="sub">Surveillance Device Detector &bull; Wardriving + GPS</div></div>
<div class="st">
<div class="sc"><div class="n" id="sT">0</div><div class="l">DETECTED</div></div>
<div class="sc"><div class="n" id="sR">0</div><div class="l">RAVEN</div></div>
<div class="sc"><div class="n" id="sB">ON</div><div class="l">BLE</div></div>
<div class="sc" id="gpsCard" onclick="reqGPS()" style="cursor:pointer"><div class="n" id="sG" style="font-size:14px">TAP</div><div class="l" id="sGL">GPS</div></div>
</div>
<div class="tb">
<button class="a" onclick="tab(0,this)">LIVE</button>
<button onclick="tab(1,this)">PREV</button>
<button onclick="tab(2,this)">DB</button>
<button onclick="tab(3,this)">TOOLS</button>
<button onclick="tab(4,this)">HIST</button>
</div>
<div class="cn">
<div class="pn a" id="p0">
<div id="dL"><div class="empty">Scanning for surveillance devices...<br>BLE active on all channels</div></div>
</div>
<div class="pn" id="p1"><div id="hL"><div class="empty">Loading prior session...</div></div></div>
<div class="pn" id="p2"><div id="pC">Loading patterns...</div></div>
<div class="pn" id="p3">
<h4>EXPORT DETECTIONS</h4>
<p style="font-size:10px;color:#8b5cf6;margin-bottom:8px">Download current session to import into Flask dashboard</p>
<button class="btn" onclick="location.href='/api/export/json'">DOWNLOAD JSON</button>
<button class="btn" onclick="location.href='/api/export/csv'">DOWNLOAD CSV</button>
<button class="btn" onclick="location.href='/api/export/kml'" style="background:#22c55e">DOWNLOAD KML (GPS MAP)</button>
<button class="btn" onclick="location.href='/api/wardriving/kml'" style="background:#16a34a">DOWNLOAD ALL-TIME KML</button>
<hr class="sep">
<h4>PRIOR SESSION</h4>
<button class="btn" onclick="location.href='/api/history/json'" style="background:#6366f1">DOWNLOAD PREV JSON</button>
<button class="btn" onclick="location.href='/api/history/kml'" style="background:#22c55e">DOWNLOAD PREV KML</button>
<hr class="sep">
<button class="btn dng" onclick="if(confirm('Clear all detections?'))fetch('/api/clear').then(()=>refresh())">CLEAR ALL DETECTIONS</button>
<hr class="sep">
<h4>DISPLAY SETTINGS</h4>
<div class="br-panel">
<label>Display: <span class="val" id="dspVal">--</span></label>
<input type="range" id="dspBr" min="0" max="255" value="200" oninput="document.getElementById('dspVal').textContent=this.value" onchange="fySetBr()">
<label>LED Strip: <span class="val" id="ledVal">--</span></label>
<input type="range" id="ledBr" min="0" max="255" value="128" oninput="document.getElementById('ledVal').textContent=this.value" onchange="fySetBr()">
<div class="incog"><input type="checkbox" id="incog" onchange="fyIncog(this.checked)"><label style="margin:0;display:inline;color:#c084fc">Incognito (kill display + LED)</label></div>
</div>
</div>
<div class="pn" id="p4"><div id="regL"><div class="empty">Loading registry...</div></div></div>
</div>
<script>
let D=[],H=[];
function fySetBr(){fetch('/brightness?disp='+document.getElementById('dspBr').value+'&led='+document.getElementById('ledBr').value);}
function fyIncog(on){var d=on?0:document.getElementById('dspBr').value,l=on?0:document.getElementById('ledBr').value;fetch('/brightness?disp='+d+'&led='+l);}
fetch('/brightness').then(r=>r.json()).then(j=>{document.getElementById('dspBr').value=j.disp;document.getElementById('dspVal').textContent=j.disp;document.getElementById('ledBr').value=j.led;document.getElementById('ledVal').textContent=j.led;}).catch(()=>{});
function tab(i,el){document.querySelectorAll('.tb button').forEach(b=>b.classList.remove('a'));document.querySelectorAll('.pn').forEach(p=>p.classList.remove('a'));el.classList.add('a');document.getElementById('p'+i).classList.add('a');if(i===1&&!window._hL)loadHistory();if(i===2&&!window._pL)loadPat();if(i===4&&!window._rL)loadReg();}
function loadReg(){fetch('/api/registry').then(r=>r.json()).then(j=>{let E=j.entries||[];let el=document.getElementById('regL');if(!E.length){el.innerHTML='<div class="empty">No registry data yet.<br>Devices are tracked across sessions on SD card.</div>';window._rL=1;return;}E.sort((a,b)=>b.s-a.s);el.innerHTML='<div style="font-size:11px;color:#8b5cf6;margin-bottom:8px">'+E.length+' known devices (lifetime)</div>'+E.map(e=>'<div class="det"><div class="mac">'+e.mac+'<span class="nm">'+(e.tp==='raven'?'RAVEN':'FLOCK')+'</span>'+(e.al?'<div style="color:#22c55e;font-weight:bold;font-size:13px;margin-top:2px">'+e.al+'</div>':'')+'</div><div class="inf"><span style="color:#ec4899;font-weight:bold">&times;'+e.s+' sessions</span>'+(e.gps?'<span style="color:#22c55e">&#9673; '+e.lat.toFixed(5)+','+e.lon.toFixed(5)+'</span>':'<span style="color:#666">no gps</span>')+'</div></div>').join('');window._rL=1;}).catch(()=>{document.getElementById('regL').innerHTML='<div class="empty">Registry unavailable (no SD card)</div>';});}
function refresh(){fetch('/api/detections').then(r=>r.json()).then(d=>{D=d;render();stats();}).catch(()=>{});}
function render(){const el=document.getElementById('dL');if(!D.length){el.innerHTML='<div class="empty">Scanning for surveillance devices...<br>BLE active on all channels</div>';return;}
D.sort((a,b)=>b.last-a.last);el.innerHTML=D.map(card).join('');}
function stats(){document.getElementById('sT').textContent=D.length;document.getElementById('sR').textContent=D.filter(d=>d.raven).length;
fetch('/api/stats').then(r=>r.json()).then(s=>{let g=document.getElementById('sG');let gl=document.getElementById('sGL');let gc=document.getElementById('gpsCard');if(s.gps_onboard){window._hwGPS=true;g.textContent=s.gps_sats+' SAT';g.style.color='#22c55e';g.style.fontSize='14px';gl.textContent='HW GPS';gl.style.color='#22c55e';gc.style.cursor='default';gc.style.borderColor='#22c55e';}else if(s.gps_valid){g.textContent=s.gps_tagged+'/'+s.total;g.style.color='#22c55e';gl.textContent='PHONE';gl.style.color='#22c55e';}else{g.textContent='TAP';g.style.color='#ef4444';gl.textContent='GPS';gl.style.color='#8b5cf6';}}).catch(()=>{});}
function macId(m){return m.replace(/:/g,'_');}
function setAlias(m,v){fetch('/api/alias?mac='+encodeURIComponent(m)+'&alias='+encodeURIComponent(v)).then(()=>refresh()).catch(()=>{});}
function card(d){var mid=macId(d.mac);return '<div class="det"><div class="mac">'+d.mac+(d.name?'<span class="nm">'+d.name+'</span>':'')+(d.alias?'<div style="color:#22c55e;font-weight:bold;font-size:13px;margin-top:2px">'+d.alias+'</div>':'')+'</div><div class="inf"><span>RSSI: '+d.rssi+'</span><span>'+d.method+'</span><span style="color:#ec4899;font-weight:bold">&times;'+d.count+'</span>'+(d.raven?'<span class="rv">RAVEN '+d.fw+'</span>':'')+(d.gps?'<span style="color:#22c55e">&#9673; '+d.gps.lat.toFixed(5)+','+d.gps.lon.toFixed(5)+'</span>':'<span style="color:#666">no gps</span>')+'</div><div style="display:flex;gap:4px;margin-top:5px"><input id="al_'+mid+'" style="flex:1;background:#111;color:#e0e0e0;border:1px solid #8b5cf6;border-radius:4px;padding:3px 6px;font-size:12px;font-family:inherit" placeholder="Label this device..." value="'+(d.alias||'')+'" maxlength="31"><button onclick="setAlias(\''+d.mac+'\',document.getElementById(\'al_'+mid+'\').value)" style="background:#8b5cf6;color:#fff;border:none;border-radius:4px;padding:3px 8px;cursor:pointer;font-size:12px">SET</button></div></div>';}
function loadHistory(){fetch('/api/history').then(r=>r.json()).then(d=>{H=d;let el=document.getElementById('hL');if(!H.length){el.innerHTML='<div class="empty">No prior session data</div>';return;}
H.sort((a,b)=>b.last-a.last);el.innerHTML='<div style="font-size:11px;color:#8b5cf6;margin-bottom:8px">'+H.length+' detections from prior session</div>'+H.map(card).join('');window._hL=1;}).catch(()=>{document.getElementById('hL').innerHTML='<div class="empty">No prior session data</div>';});}
function loadPat(){fetch('/api/patterns').then(r=>r.json()).then(p=>{let h='';
h+='<div class="pg"><h3>MAC Prefixes ('+p.macs.length+')</h3><div class="it">'+p.macs.map(m=>'<span>'+m+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>BLE Device Names ('+p.names.length+')</h3><div class="it">'+p.names.map(n=>'<span>'+n+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>BLE Manufacturer IDs ('+p.mfr.length+')</h3><div class="it">'+p.mfr.map(m=>'<span>0x'+m.toString(16).toUpperCase().padStart(4,'0')+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>Raven UUIDs ('+p.raven.length+')</h3><div class="it">'+p.raven.map(u=>'<span style="font-size:8px">'+u+'</span>').join('')+'</div></div>';
document.getElementById('pC').innerHTML=h;window._pL=1;}).catch(()=>{});}
// GPS from phone -> ESP32 (wardriving)
// NOTE: Geolocation API needs secure context (HTTPS) on most browsers.
// HTTP works on: Android Chrome (local IPs), some Android browsers.
// Won't work on: iOS Safari (needs HTTPS always).
// We only request on user tap (gesture) for best permission prompt chance.
let _gW=null,_gOk=false,_gTried=false;
function sendGPS(p){_gOk=true;let g=document.getElementById('sG');g.textContent='OK';g.style.color='#22c55e';
fetch('/api/gps?lat='+p.coords.latitude+'&lon='+p.coords.longitude+'&acc='+(p.coords.accuracy||0)).catch(()=>{});}
function gpsErr(e){_gOk=false;let g=document.getElementById('sG');
var msg='ERR';if(e.code===1){msg='DENIED';g.style.color='#ef4444';alert('GPS permission denied. On iPhone, GPS requires HTTPS which this device cannot provide. On Android Chrome, tap the lock/info icon in the address bar and allow Location.');}
else if(e.code===2){msg='N/A';g.style.color='#ef4444';}
else if(e.code===3){msg='WAIT';g.style.color='#facc15';}
g.textContent=msg;}
function startGPS(){if(!navigator.geolocation){return false;}
if(_gW!==null){navigator.geolocation.clearWatch(_gW);_gW=null;}
let g=document.getElementById('sG');g.textContent='...';g.style.color='#facc15';
_gW=navigator.geolocation.watchPosition(sendGPS,gpsErr,{enableHighAccuracy:true,maximumAge:5000,timeout:15000});return true;}
function reqGPS(){if(window._hwGPS){alert('Hardware GPS is active on the device. No phone GPS needed!');return;}
if(!navigator.geolocation){alert('GPS not available in this browser.');return;}
if(_gOk){return;}
if(!window.isSecureContext){alert('GPS requires a secure context (HTTPS). This HTTP page may not get GPS permission.\\n\\nAndroid Chrome: try chrome://flags and enable "Insecure origins treated as secure", add http://192.168.4.1\\n\\niPhone: GPS will not work over HTTP.');}
startGPS();_gTried=true;}
refresh();setInterval(refresh,2500);
</script></body></html>
)rawliteral";

// ============================================================================
// WEB SERVER SETUP
// ============================================================================

static void fySetupServer() {
    // Dashboard
    fyServer.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/html", FY_HTML);
    });

    // API: Detection list
    fyServer.on("/api/detections", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        writeDetectionsJSON(resp);
        r->send(resp);
    });

    // API: Stats (includes GPS status)
    fyServer.on("/api/stats", HTTP_GET, [](AsyncWebServerRequest *r) {
        int raven = 0, withGPS = 0;
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < fyDetCount; i++) {
                if (fyDet[i].isRaven) raven++;
                if (fyDet[i].hasGPS) withGPS++;
            }
            xSemaphoreGive(fyMutex);
        }
        int gpsSats = 0;
#if HAS_GPS
        gpsSats = (int)fyGPS.satellites.value();
#endif
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"total\":%d,\"raven\":%d,\"ble\":\"active\","
            "\"gps_valid\":%s,\"gps_age\":%lu,\"gps_tagged\":%d,"
            "\"gps_onboard\":%s,\"gps_sats\":%d}",
            fyDetCount, raven,
            fyGPSIsFresh() ? "true" : "false",
            fyGPSValid ? (millis() - fyGPSLastUpdate) : 0UL,
            withGPS,
            fyGPSOnboard ? "true" : "false",
            gpsSats);
        r->send(200, "application/json", buf);
    });

    // API: Receive GPS from phone browser
    fyServer.on("/api/gps", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (r->hasParam("lat") && r->hasParam("lon")) {
#if HAS_GPS
            // Hardware GPS takes priority — ignore phone GPS once onboard fix acquired
            if (fyGPSOnboard) {
                r->send(200, "application/json", "{\"status\":\"ignored\",\"reason\":\"hw_gps_active\"}");
                return;
            }
#endif
            fyGPSLat = r->getParam("lat")->value().toDouble();
            fyGPSLon = r->getParam("lon")->value().toDouble();
            fyGPSAcc = r->hasParam("acc") ? r->getParam("acc")->value().toFloat() : 0;
            fyGPSValid = true;
            fyGPSLastUpdate = millis();
            r->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            r->send(400, "application/json", "{\"error\":\"lat,lon required\"}");
        }
    });

    // API: Pattern database
    fyServer.on("/api/patterns", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        resp->print("{\"macs\":[");
        for (size_t i = 0; i < sizeof(mac_prefixes)/sizeof(mac_prefixes[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", mac_prefixes[i]);
        }
        resp->print("],\"names\":[");
        for (size_t i = 0; i < sizeof(device_name_patterns)/sizeof(device_name_patterns[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", device_name_patterns[i]);
        }
        resp->print("],\"mfr\":[");
        for (size_t i = 0; i < sizeof(ble_manufacturer_ids)/sizeof(ble_manufacturer_ids[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("%u", ble_manufacturer_ids[i]);
        }
        resp->print("],\"raven\":[");
        for (size_t i = 0; i < sizeof(raven_service_uuids)/sizeof(raven_service_uuids[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", raven_service_uuids[i]);
        }
        resp->print("]}");
        r->send(resp);
    });

    // API: Export JSON (downloadable file)
    fyServer.on("/api/export/json", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_detections.json\"");
        writeDetectionsJSON(resp);
        r->send(resp);
    });

    // API: Export CSV (downloadable file, includes GPS)
    fyServer.on("/api/export/csv", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("text/csv");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_detections.csv\"");
        resp->println("mac,name,rssi,method,first_seen_ms,last_seen_ms,count,is_raven,raven_fw,latitude,longitude,gps_accuracy");
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            for (int i = 0; i < fyDetCount; i++) {
                FYDetection& d = fyDet[i];
                if (d.hasGPS) {
                    resp->printf("\"%s\",\"%s\",%d,\"%s\",%lu,%lu,%d,%s,\"%s\",%.8f,%.8f,%.1f\n",
                        d.mac, d.name, d.rssi, d.method,
                        d.firstSeen, d.lastSeen, d.count,
                        d.isRaven ? "true" : "false", d.ravenFW,
                        d.gpsLat, d.gpsLon, d.gpsAcc);
                } else {
                    resp->printf("\"%s\",\"%s\",%d,\"%s\",%lu,%lu,%d,%s,\"%s\",,,\n",
                        d.mac, d.name, d.rssi, d.method,
                        d.firstSeen, d.lastSeen, d.count,
                        d.isRaven ? "true" : "false", d.ravenFW);
                }
            }
            xSemaphoreGive(fyMutex);
        }
        r->send(resp);
    });

    // API: Export KML (GPS-tagged detections for Google Earth)
    fyServer.on("/api/export/kml", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/vnd.google-earth.kml+xml");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_detections.kml\"");
        writeDetectionsKML(resp);
        r->send(resp);
    });

#if HAS_SD_CARD
    // API: All-time wardriving KML (from persistent registry, all sessions)
    fyServer.on("/api/wardriving/kml", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/vnd.google-earth.kml+xml");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_wardriving.kml\"");
        writeRegistryKML(resp);
        r->send(resp);
    });
#endif

    // API: Prior session history (JSON)
    fyServer.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (fySpiffsReady && SPIFFS.exists(FY_PREV_FILE)) {
            r->send(SPIFFS, FY_PREV_FILE, "application/json");
        } else {
            r->send(200, "application/json", "[]");
        }
    });

    // API: Download prior session as JSON file
    fyServer.on("/api/history/json", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (fySpiffsReady && SPIFFS.exists(FY_PREV_FILE)) {
            AsyncWebServerResponse *resp = r->beginResponse(SPIFFS, FY_PREV_FILE, "application/json");
            resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_prev_session.json\"");
            r->send(resp);
        } else {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
        }
    });

    // API: Download prior session as KML (reads JSON from SPIFFS, converts)
    fyServer.on("/api/history/kml", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!fySpiffsReady || !SPIFFS.exists(FY_PREV_FILE)) {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
            return;
        }
        File f = SPIFFS.open(FY_PREV_FILE, "r");
        if (!f) { r->send(500, "text/plain", "read error"); return; }
        String content = f.readString();
        f.close();
        if (content.length() == 0) {
            r->send(404, "application/json", "{\"error\":\"prior session empty\"}");
            return;
        }
        AsyncResponseStream *resp = r->beginResponseStream("application/vnd.google-earth.kml+xml");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_prev_session.kml\"");
        resp->print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
                    "<name>Flock-You Prior Session</name>\n"
                    "<description>Surveillance device detections from prior session</description>\n"
                    "<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
                    "<scale>1.0</scale></IconStyle></Style>\n"
                    "<Style id=\"raven\"><IconStyle><color>ff4444ef</color>"
                    "<scale>1.2</scale></IconStyle></Style>\n");
        // Parse JSON array and emit placemarks
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, content);
        if (!err && doc.is<JsonArray>()) {
            int placed = 0;
            for (JsonObject d : doc.as<JsonArray>()) {
                JsonObject gps = d["gps"];
                if (!gps || !gps.containsKey("lat")) continue;
                bool isRaven = d["raven"] | false;
                resp->printf("<Placemark><name>%s</name>\n", d["mac"] | "?");
                resp->printf("<styleUrl>#%s</styleUrl>\n", isRaven ? "raven" : "det");
                resp->print("<description><![CDATA[");
                if (d["name"].is<const char*>() && strlen(d["name"] | "") > 0)
                    resp->printf("<b>Name:</b> %s<br/>", d["name"] | "");
                resp->printf("<b>Method:</b> %s<br/><b>RSSI:</b> %d<br/><b>Count:</b> %d",
                    d["method"] | "?", d["rssi"] | 0, d["count"] | 1);
                if (isRaven && d["fw"].is<const char*>())
                    resp->printf("<br/><b>Raven FW:</b> %s", d["fw"] | "");
                resp->print("]]></description>\n");
                resp->printf("<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n",
                    (double)(gps["lon"] | 0.0), (double)(gps["lat"] | 0.0));
                resp->print("</Placemark>\n");
                placed++;
            }
            printf("[FLOCK-YOU] Prior session KML: %d placemarks\n", placed);
        } else {
            printf("[FLOCK-YOU] Prior session KML: JSON parse failed\n");
        }
        resp->print("</Document>\n</kml>");
        r->send(resp);
    });

    // API: Clear all detections (saves current session first)
    fyServer.on("/api/clear", HTTP_GET, [](AsyncWebServerRequest *r) {
        fySaveSession();  // Persist before clearing
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            fyDetCount = 0;
            memset(fyDet, 0, sizeof(fyDet));
            fyTriggered = false;
            fyDeviceInRange = false;
            xSemaphoreGive(fyMutex);
        }
        r->send(200, "application/json", "{\"status\":\"cleared\"}");
        printf("[FLOCK-YOU] All detections cleared (session saved)\n");
    });

    // Brightness / incognito control endpoint
    fyServer.on("/brightness", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("disp")) {
            uint8_t d = (uint8_t)request->getParam("disp")->value().toInt();
            display_set_brightness(d);
            Preferences bp; bp.begin("ouispy-br", false);
            bp.putUChar("disp", d); bp.end();
        }
        if (request->hasParam("led")) {
            uint8_t l = (uint8_t)request->getParam("led")->value().toInt();
            ledStrip_setBrightness(l); ledStrip_show();
            Preferences bp; bp.begin("ouispy-br", false);
            bp.putUChar("led", l); bp.end();
        }
        Preferences bp; bp.begin("ouispy-br", true);
        uint8_t cd = bp.getUChar("disp", 200);
        uint8_t cl = bp.getUChar("led", 128);
        bp.end();
        char json[64];
        snprintf(json, sizeof(json), "{\"disp\":%d,\"led\":%d}", cd, cl);
        request->send(200, "application/json", json);
    });

    // API: Set/update a user alias for a device (persisted to registry)
    fyServer.on("/api/alias", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!r->hasParam("mac") || !r->hasParam("alias")) {
            r->send(400, "application/json", "{\"error\":\"mac and alias required\"}");
            return;
        }
        String mac   = r->getParam("mac")->value();
        String alias = r->getParam("alias")->value();
        if (alias.length() > 31) alias = alias.substring(0, 31);
#if HAS_SD_CARD
        if (fyRegMutex && xSemaphoreTake(fyRegMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            FYRegEntry& e = fyRegistry[mac.c_str()];
            strncpy(e.alias, alias.c_str(), sizeof(e.alias) - 1);
            e.alias[sizeof(e.alias) - 1] = '\0';
            fyRegistryDirty = true;
            xSemaphoreGive(fyRegMutex);
        }
#endif
        // Update in-session detection record too
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < fyDetCount; i++) {
                if (strcasecmp(fyDet[i].mac, mac.c_str()) == 0) {
                    strncpy(fyDet[i].alias, alias.c_str(), sizeof(fyDet[i].alias) - 1);
                    fyDet[i].alias[sizeof(fyDet[i].alias) - 1] = '\0';
                    break;
                }
            }
            xSemaphoreGive(fyMutex);
        }
        r->send(200, "application/json", "{\"status\":\"ok\"}");
        printf("[FLOCK-YOU] Alias set: %s -> %s\n", mac.c_str(), alias.c_str());
    });

    // API: Cross-session device registry (SD card)
    fyServer.on("/api/registry", HTTP_GET, [](AsyncWebServerRequest *r) {
#if HAS_SD_CARD
        File f = SD_MMC.open(FY_REG_PATH, "r");
        if (f) {
            AsyncResponseStream *resp = r->beginResponseStream("application/json");
            uint8_t buf[256];
            while (f.available()) {
                int n = f.read(buf, sizeof(buf));
                if (n > 0) resp->write(buf, n);
            }
            f.close();
            r->send(resp);
            return;
        }
#endif
        r->send(200, "application/json", "{\"v\":1,\"entries\":[]}");
    });

    fyServer.begin();
    printf("[FLOCK-YOU] Web server started on port 80\n");
}

// ============================================================================
// MAIN FUNCTIONS
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    // Read buzzer setting from OUI-SPY NVS
    Preferences bzP;
    bzP.begin("ouispy-bz", true);
    fyBuzzerOn = bzP.getBool("on", true);
    bzP.end();

#ifndef BOARD_CYD_S3
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
#ifdef SPEAKER_EN_PIN
    pinMode(SPEAKER_EN_PIN, OUTPUT);
    digitalWrite(SPEAKER_EN_PIN, LOW);  // Enable speaker amplifier (active-LOW)
#endif
#endif

#if HAS_NEOPIXEL
    fyPixel.begin();
    fyPixel.setBrightness(50);
    fyPixel.setPixelColor(0, fyPixel.Color(236, 72, 153));  // pink boot flash
    fyPixel.show();
    delay(400);
    fyPixel.setPixelColor(0, fyPixel.Color(139, 92, 246));  // purple settle
    fyPixel.show();
    delay(400);
    fyPixel.clear();
    fyPixel.show();
#endif

    fyMutex = xSemaphoreCreateMutex();

    // GPS Serial0 already initialized in main.cpp setup()

    // Init SPIFFS for session persistence
    if (SPIFFS.begin(true)) {
        fySpiffsReady = true;
        printf("[FLOCK-YOU] SPIFFS ready\n");
        // Promote last session to prev_session before we start a new one
        fyPromotePrevSession();
    } else {
        printf("[FLOCK-YOU] SPIFFS init failed - no persistence\n");
    }

#if HAS_SD_CARD
    fyRegMutex = xSemaphoreCreateMutex();
    if (sdlog_available()) fyRegistryLoad();
#endif

    printf("\n========================================\n");
    printf("  FLOCK-YOU Surveillance Detector\n");
    printf("  Buzzer: %s\n", fyBuzzerOn ? "ON" : "OFF");
    printf("========================================\n");

    // Init BLE scanner FIRST -- start scanning immediately
    NimBLEDevice::init("");
    fyBLEScan = NimBLEDevice::getScan();
    fyBLEScan->setAdvertisedDeviceCallbacks(new FYBLECallbacks());
    fyBLEScan->setActiveScan(true);
    fyBLEScan->setInterval(100);
    fyBLEScan->setWindow(99);

    // Kick off the first scan right away
    fyBLEScan->start(BLE_SCAN_DURATION, false);
    fyLastBleScan = millis();
    printf("[FLOCK-YOU] BLE scanning ACTIVE\n");

    // Crow calls play WHILE BLE is already scanning
    fyBootBeep();

    // Start WiFi AP (no need to connect to anything -- AP only)
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAP(FY_AP_SSID, FY_AP_PASS);
    printf("[FLOCK-YOU] AP: %s / %s\n", FY_AP_SSID, FY_AP_PASS);
    printf("[FLOCK-YOU] IP: %s\n", WiFi.softAPIP().toString().c_str());

    // Start web dashboard
    fySetupServer();

    printf("[FLOCK-YOU] Detection methods: MAC prefix, device name, manufacturer ID, Raven UUID\n");
    printf("[FLOCK-YOU] Dashboard: http://192.168.4.1\n");
    printf("[FLOCK-YOU] Ready - no WiFi connection needed, BLE + AP only\n\n");

#ifdef ENABLE_TFT_DISPLAY
    display_flockyou(0, 0, fyBuzzerOn, "", "", 0
#if HAS_GPS
                     , (int)fyGPS.satellites.value()
#endif
                     );
#endif
}

void loop() {
#if HAS_GPS
    // Feed onboard GPS from Serial0 (UART0)
    while (Serial0.available()) {
        if (fyGPS.encode(Serial0.read())) {
            if (fyGPS.location.isValid() && fyGPS.location.isUpdated()) {
                fyGPSLat = fyGPS.location.lat();
                fyGPSLon = fyGPS.location.lng();
                fyGPSAcc = (fyGPS.hdop.isValid() && fyGPS.hdop.hdop() < 99.0)
                           ? (float)(fyGPS.hdop.hdop() * 5.0) : 0;  // HDOP to ~meters
                fyGPSValid = true;
                fyGPSOnboard = true;
                fyGPSLastUpdate = millis();
            }
        }
    }
#endif

#if HAS_NEOPIXEL
    fyUpdatePixel();
#endif

    // BLE scanning cycle
    if (millis() - fyLastBleScan >= BLE_SCAN_INTERVAL && !fyBLEScan->isScanning()) {
        fyBLEScan->start(BLE_SCAN_DURATION, false);
        fyLastBleScan = millis();
    }

    if (!fyBLEScan->isScanning() && millis() - fyLastBleScan > BLE_SCAN_DURATION * 1000) {
        fyBLEScan->clearResults();
    }

    // Heartbeat tracking
    if (fyDeviceInRange) {
        if (millis() - fyLastHB >= 10000) {
            fyHeartbeat();
            fyLastHB = millis();
#ifdef ENABLE_TFT_DISPLAY
            display_flockyou(fyDetCount, 1, fyBuzzerOn, "", "", 0
#if HAS_GPS
                             , (int)fyGPS.satellites.value()
#endif
                             );
#endif
        }
        if (millis() - fyLastDetTime >= 30000) {
            printf("[FLOCK-YOU] Device out of range - stopping heartbeat\n");
            fyDeviceInRange = false;
            fyTriggered = false;
#ifdef ENABLE_TFT_DISPLAY
            display_flockyou(fyDetCount, 0, fyBuzzerOn, "", "", 0
#if HAS_GPS
                             , (int)fyGPS.satellites.value()
#endif
                             );
#endif
        }
    }

    // Periodic display refresh (GPS sats, idle status) every 5s
#ifdef ENABLE_TFT_DISPLAY
    static unsigned long fyLastDisplayRefresh = 0;
    if (!fyDeviceInRange && millis() - fyLastDisplayRefresh >= 5000) {
        fyLastDisplayRefresh = millis();
        display_flockyou(fyDetCount, 0, fyBuzzerOn, "", "", 0
#if HAS_GPS
                         , (int)fyGPS.satellites.value()
#endif
                         );
    }
#endif

#if HAS_SD_CARD
    // Flush registry to SD card if changed (done in loop to avoid blocking BLE callback)
    if (fyRegistryDirty) fyRegistrySave();
#endif

    // Auto-save session to SPIFFS every 15s if detections changed
    // Also triggers an early save 5s after first detection to minimize loss on power-cycle
    if (fySpiffsReady && millis() - fyLastSave >= FY_SAVE_INTERVAL) {
        if (fyDetCount > 0 && fyDetCount != fyLastSaveCount) {
            fySaveSession();
        }
        fyLastSave = millis();
    } else if (fySpiffsReady && fyDetCount > 0 && fyLastSaveCount == 0 &&
               millis() - fyLastSave >= 5000) {
        // Quick first-save: persist within 5s of first detection
        fySaveSession();
        fyLastSave = millis();
    }

    delay(100);
}
