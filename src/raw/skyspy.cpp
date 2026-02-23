#if !defined(ARDUINO_ARCH_ESP32)
  #error "This program requires an ESP32S3"
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
// BLE headers provided by wrapper (NimBLE)
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include "opendroneid.h"
#include "odid_wifi.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Buzzer configuration
#ifndef BUZZER_PIN
#define BUZZER_PIN 3  // GPIO3 (D2) - PWM capable pin on Xiao ESP32 S3
#endif

// LED configuration
#ifndef LED_PIN
#define LED_PIN 21    // GPIO21 - Built-in orange LED on Xiao ESP32 S3 (inverted logic)
#endif

// Audio Configuration
#define DETECT_FREQ 1000  // Detection alert - high pitch (faster beeps)
#define HEARTBEAT_FREQ 600 // Heartbeat pulse frequency
#define DETECT_BEEP_DURATION 150 // Detection beep duration (faster)
#define HEARTBEAT_DURATION 100   // Short heartbeat pulse

struct id_data {
  uint8_t  mac[6];
  int      rssi;
  uint32_t last_seen;
  char     op_id[ODID_ID_SIZE + 1];
  char     uav_id[ODID_ID_SIZE + 1];
  double   lat_d;
  double   long_d;
  double   base_lat_d;
  double   base_long_d;
  int      altitude_msl;
  int      height_agl;
  int      speed;
  int      heading;
  int      flag;
};

void callback(void *, wifi_promiscuous_pkt_type_t);
void send_json_fast(const id_data *UAV);
void buzzerTask(void *parameter);

#define MAX_UAVS 8
id_data uavs[MAX_UAVS] = {0};
NimBLEScan* pBLEScan = nullptr;
ODID_UAS_Data UAS_data;
unsigned long last_status = 0;
unsigned long last_heartbeat = 0;
static unsigned long ssSessionStartMs = 0;  // set in setup(), used for track timestamps

// Channel hopping state
// Dwell 500ms on CH6 (NAN discovery), 100ms on each other channel (beacons)
static const int CH_HOP_SEQUENCE[] = {6, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13};
static const int CH_HOP_COUNT = sizeof(CH_HOP_SEQUENCE) / sizeof(CH_HOP_SEQUENCE[0]);
static const unsigned long CH_DWELL_NAN = 500;   // ms on CH6
static const unsigned long CH_DWELL_OTHER = 100;  // ms on other channels
static int ssChIdx = 0;
static int ssCurrentChannel = 6;
static unsigned long ssChSwitchTime = 0;

// Live web dashboard + BLE GATT push (CYD only)
#ifdef BOARD_CYD_S3
static AsyncWebServer ssServer(80);

// BLE GATT peripheral — Nordic UART Service (NUS), phone subscribes to drone data
#define SS_BLE_SVC "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define SS_BLE_TX  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
static NimBLEServer*         ssGattSrv   = nullptr;
static NimBLECharacteristic* ssDroneChar = nullptr;
static volatile bool         ssPhoneConn = false;

class SsGattCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    ssPhoneConn = true;
    Serial.println("[SKY-SPY] BLE client connected");
  }
  void onDisconnect(NimBLEServer*) override {
    ssPhoneConn = false;
    Serial.println("[SKY-SPY] BLE client disconnected, restarting adv");
    NimBLEDevice::startAdvertising();
  }
};
#endif

// Per-mode display/LED/volume settings (loaded from NVS "ouispy-br")
static uint8_t ssDispBrightness = 200;
static uint8_t ssLedBrightness  = 50;
static uint8_t ssVolume         = 128;
static volatile bool ssBleOn    = true;  // BLE scan enable (NVS "ouispy-bz"/"blen")

// Settings page state
static bool ssInSettings    = false;
static bool ssIncognito     = false;
static uint8_t ssSavedDisp  = 200;   // saved disp/led for incognito restore
static uint8_t ssSavedLed   = 50;

// Device GPS state (CYD onboard GPS)
#if HAS_GPS
static TinyGPSPlus ssGPS;
static double ssDevLat = 0;
static double ssDevLon = 0;
static int    ssDevSats = 0;
#endif

// Persistent drone registry (cross-session tracking via JSON on SD card)
#if HAS_SD_CARD
static const uint8_t SS_REG_MAX          = 100;
static const uint8_t SS_REG_SAMPLE_MAX   = 50;     // max pilot GPS points stored per operator
static const double  SS_SAMPLE_MIN_DIST  = 0.0002; // ~22m in degrees — dedup radius
static const char    SS_REG_PATH[]       = "/oui-spy/skyspy/registry.json";
static const char    SS_REG_TMP_PATH[]   = "/oui-spy/skyspy/registry.tmp";

struct SsRegEntry {
  uint32_t sessions;           // total sessions seen across all power cycles
  double   pilot_lat;          // running centroid of pilot launch position
  double   pilot_lon;
  double   pilot_lat_last;     // most recent pilot position
  double   pilot_lon_last;
  uint32_t samples;            // sample count for centroid averaging
  char     op_id[ODID_ID_SIZE + 1];
  char     mac[18];
  std::vector<std::pair<double,double>> pilot_pts;  // individual launch-site samples
};

static std::map<std::string, SsRegEntry> ssRegistry;    // loaded at startup
static std::set<std::string>             ssSessionIds;   // IDs incremented this session

static void ssRegistryLoad() {
  File f = SD_MMC.open(SS_REG_PATH, "r");
  if (!f) { Serial.println("[SKY-SPY] No registry yet"); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.printf("[SKY-SPY] Registry parse error: %s\n", err.c_str()); return; }
  for (JsonObject e : doc["entries"].as<JsonArray>()) {
    const char* id = e["id"] | "";
    if (!id[0]) continue;
    SsRegEntry r = {};
    r.sessions       = e["sessions"] | 1u;
    r.pilot_lat      = e["plat"]  | 0.0;
    r.pilot_lon      = e["plon"]  | 0.0;
    r.pilot_lat_last = e["pllat"] | 0.0;
    r.pilot_lon_last = e["pllon"] | 0.0;
    r.samples        = e["samp"]  | 0u;
    strncpy(r.op_id, e["op"]  | "", sizeof(r.op_id) - 1);
    strncpy(r.mac,   e["mac"] | "", sizeof(r.mac)   - 1);
    for (JsonArray pt : e["pts"].as<JsonArray>()) {
      if (pt.size() >= 2)
        r.pilot_pts.push_back({pt[0].as<double>(), pt[1].as<double>()});
    }
    ssRegistry[id] = r;
  }
  Serial.printf("[SKY-SPY] Registry: %u known drones\n", (unsigned)ssRegistry.size());
}

static void ssRegistrySave() {
  File f = SD_MMC.open(SS_REG_TMP_PATH, FILE_WRITE);
  if (!f) { Serial.println("[SKY-SPY] Registry save: open failed"); return; }
  JsonDocument doc;
  doc["v"] = 1;
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (auto& kv : ssRegistry) {
    JsonObject e  = arr.add<JsonObject>();
    e["id"]       = kv.first.c_str();
    e["op"]       = kv.second.op_id;
    e["mac"]      = kv.second.mac;
    e["sessions"] = kv.second.sessions;
    e["plat"]     = kv.second.pilot_lat;
    e["plon"]     = kv.second.pilot_lon;
    e["pllat"]    = kv.second.pilot_lat_last;
    e["pllon"]    = kv.second.pilot_lon_last;
    e["samp"]     = kv.second.samples;
    if (!kv.second.pilot_pts.empty()) {
      JsonArray pts = e["pts"].to<JsonArray>();
      for (auto& pt : kv.second.pilot_pts) {
        JsonArray p = pts.add<JsonArray>();
        p.add(pt.first);
        p.add(pt.second);
      }
    }
  }
  serializeJson(doc, f);
  f.close();
  SD_MMC.remove(SS_REG_PATH);
  SD_MMC.rename(SS_REG_TMP_PATH, SS_REG_PATH);
  Serial.printf("[SKY-SPY] Registry saved (%u entries)\n", (unsigned)ssRegistry.size());
}

// Returns sessions_seen (1=brand new, >1=repeat operator). 0 if no uas_id.
static uint32_t ssRegistryUpdate(const char* uas_id, const char* op_id,
                                  const char* mac,
                                  double pilot_lat, double pilot_lon) {
  if (!uas_id || !uas_id[0]) return 0;
  bool isNew     = !ssRegistry.count(uas_id);
  bool firstThis = !ssSessionIds.count(uas_id);

  // Evict lowest-sessions entry if at capacity
  if (isNew && ssRegistry.size() >= SS_REG_MAX) {
    auto evict = ssRegistry.begin();
    for (auto it = ssRegistry.begin(); it != ssRegistry.end(); ++it) {
      if (it->second.sessions < evict->second.sessions) evict = it;
    }
    ssRegistry.erase(evict);
  }

  SsRegEntry& e = ssRegistry[uas_id];
  bool needsSave = false;

  if (isNew) {
    e = {};
    e.sessions = 1;
    ssSessionIds.insert(uas_id);
    needsSave = true;
  } else if (firstThis) {
    e.sessions++;
    ssSessionIds.insert(uas_id);
    needsSave = true;
  }

  if (op_id && op_id[0]) strncpy(e.op_id, op_id, sizeof(e.op_id) - 1);
  if (mac   && mac[0])   strncpy(e.mac,   mac,   sizeof(e.mac)   - 1);

  if (pilot_lat != 0.0 || pilot_lon != 0.0) {
    e.pilot_lat_last = pilot_lat;
    e.pilot_lon_last = pilot_lon;
    if (e.samples == 0) {
      e.pilot_lat = pilot_lat;
      e.pilot_lon = pilot_lon;
    } else {
      e.pilot_lat = (e.pilot_lat * e.samples + pilot_lat) / (e.samples + 1);
      e.pilot_lon = (e.pilot_lon * e.samples + pilot_lon) / (e.samples + 1);
    }
    e.samples++;

    // Add to individual sample list if far enough from all existing points (~22m dedup)
    bool tooClose = false;
    for (auto& pt : e.pilot_pts) {
      double dlat = pt.first  - pilot_lat;
      double dlon = pt.second - pilot_lon;
      if (dlat*dlat + dlon*dlon < SS_SAMPLE_MIN_DIST * SS_SAMPLE_MIN_DIST) {
        tooClose = true;
        break;
      }
    }
    if (!tooClose) {
      if (e.pilot_pts.size() >= SS_REG_SAMPLE_MAX)
        e.pilot_pts.erase(e.pilot_pts.begin());  // drop oldest when full
      e.pilot_pts.push_back({pilot_lat, pilot_lon});
      needsSave = true;
    }
  }

  if (needsSave) ssRegistrySave();
  return e.sessions;
}

static void ssWriteSessionSummary() {
  const char* sessionPath = sdlog_session_path();
  if (!sessionPath) return;  // no detections this session — nothing to summarise

  // Derive summary path: replace ".jsonl" suffix with "_summary.json"
  char summaryPath[80];
  strncpy(summaryPath, sessionPath, sizeof(summaryPath) - 1);
  summaryPath[sizeof(summaryPath) - 1] = '\0';
  char* dot = strrchr(summaryPath, '.');
  if (dot) strcpy(dot, "_summary.json");
  else     strncat(summaryPath, "_summary.json", sizeof(summaryPath) - strlen(summaryPath) - 1);

  // Count unique drones seen this session
  int totalDrones = 0, newDrones = 0;
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].mac[0] == 0) continue;
    totalDrones++;
    if (ssRegistry.count(uavs[i].uav_id) && ssRegistry[uavs[i].uav_id].sessions == 1)
      newDrones++;
  }

  unsigned long durationS = (millis() - ssSessionStartMs) / 1000UL;

  JsonDocument doc;
  doc["v"]          = 1;
  doc["mode"]       = 5;
  doc["session"]    = sessionPath;
  doc["duration_s"] = durationS;
  doc["total"]      = totalDrones;
  doc["new"]        = newDrones;

  JsonArray arr = doc["drones"].to<JsonArray>();
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].mac[0] == 0) continue;
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             uavs[i].mac[0], uavs[i].mac[1], uavs[i].mac[2],
             uavs[i].mac[3], uavs[i].mac[4], uavs[i].mac[5]);
    JsonObject d = arr.add<JsonObject>();
    d["id"]   = uavs[i].uav_id;
    d["mac"]  = mac_str;
    d["lat"]  = uavs[i].lat_d;
    d["lon"]  = uavs[i].long_d;
    d["alt"]  = uavs[i].altitude_msl;
    d["rssi"] = uavs[i].rssi;
    if (ssRegistry.count(uavs[i].uav_id))
      d["sessions"] = ssRegistry[uavs[i].uav_id].sessions;
  }

  File f = SD_MMC.open(summaryPath, FILE_WRITE);
  if (f) {
    serializeJson(doc, f);
    f.close();
    Serial.printf("[SKY-SPY] Session summary: %s\n", summaryPath);
  }
  ssRegistrySave();  // flush any unsaved registry changes
}
#endif

static void ssSessionEnd() {
#if HAS_SD_CARD
  sdlog_flush();
  ssWriteSessionSummary();
#endif
}

// Thread-safe flags for buzzer (volatile for ISR access)
volatile bool device_in_range = false;
volatile bool trigger_detection_beep = false;
volatile bool trigger_heartbeat_beep = false;
static portMUX_TYPE buzzerMux = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t printQueue;

id_data* next_uav(uint8_t* mac) {
  for (int i = 0; i < MAX_UAVS; i++) {
    if (memcmp(uavs[i].mac, mac, 6) == 0)
      return &uavs[i];
  }
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].mac[0] == 0)
      return &uavs[i];
  }
  return &uavs[0];
}

class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
  void onResult(NimBLEAdvertisedDevice* device) override {
    int len = device->getPayloadLength();
    if (len <= 0) return;
      
    uint8_t* payload = device->getPayload();
    if (len > 5 && payload[1] == 0x16 && payload[2] == 0xFA && 
        payload[3] == 0xFF && payload[4] == 0x0D) {
      uint8_t* mac = (uint8_t*) device->getAddress().getNative();
      id_data* UAV = next_uav(mac);
      UAV->last_seen = millis();
      UAV->rssi = device->getRSSI();
      memcpy(UAV->mac, mac, 6);
      
      uint8_t* odid = &payload[6];
      switch (odid[0] & 0xF0) {
        case 0x00: {
          ODID_BasicID_data basic;
          decodeBasicIDMessage(&basic, (ODID_BasicID_encoded*) odid);
          strncpy(UAV->uav_id, (char*) basic.UASID, ODID_ID_SIZE);
          break;
        }
        case 0x10: {
          ODID_Location_data loc;
          decodeLocationMessage(&loc, (ODID_Location_encoded*) odid);
          UAV->lat_d = loc.Latitude;
          UAV->long_d = loc.Longitude;
          UAV->altitude_msl = (int) loc.AltitudeGeo;
          UAV->height_agl = (int) loc.Height;
          UAV->speed = (int) loc.SpeedHorizontal;
          UAV->heading = (int) loc.Direction;
          break;
        }
        case 0x40: {
          ODID_System_data sys;
          decodeSystemMessage(&sys, (ODID_System_encoded*) odid);
          UAV->base_lat_d = sys.OperatorLatitude;
          UAV->base_long_d = sys.OperatorLongitude;
          break;
        }
        case 0x50: {
          ODID_OperatorID_data op;
          decodeOperatorIDMessage(&op, (ODID_OperatorID_encoded*) odid);
          strncpy(UAV->op_id, (char*) op.OperatorId, ODID_ID_SIZE);
          break;
        }
      }
      UAV->flag = 1;
      
      // Trigger buzzer alert (thread-safe, non-blocking)
      portENTER_CRITICAL_ISR(&buzzerMux);
      if (!device_in_range) {
        trigger_detection_beep = true;
        device_in_range = true;
        last_heartbeat = millis();
      }
      portEXIT_CRITICAL_ISR(&buzzerMux);
      
      {
        id_data tmp = *UAV;
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(printQueue, &tmp, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
      }
    }
  }
};

// Dedicated non-blocking buzzer task - never delays detection
void buzzerTask(void *parameter) {
  for (;;) {
    // Check for detection beep trigger
    portENTER_CRITICAL(&buzzerMux);
    bool do_detection = trigger_detection_beep;
    if (do_detection) trigger_detection_beep = false;
    portEXIT_CRITICAL(&buzzerMux);
    
    if (do_detection) {
      Serial.println("DRONE DETECTED! Playing alert sequence");
      for (int i = 0; i < 3; i++) {
#if HAS_BUZZER
        if (ssVolume > 0) tone(BUZZER_PIN, DETECT_FREQ, DETECT_BEEP_DURATION);
#endif
#if LED_PIN >= 0
        if (ssLedBrightness > 0) digitalWrite(LED_PIN, LOW);
#endif
#if HAS_NEOPIXEL
        if (ssLedBrightness > 0) { ledStrip_setColor(255, 0, 0); ledStrip_show(); }
#endif
        vTaskDelay(pdMS_TO_TICKS(150));
#if LED_PIN >= 0
        if (ssLedBrightness > 0) digitalWrite(LED_PIN, HIGH);
#endif
#if HAS_NEOPIXEL
        if (ssLedBrightness > 0) { ledStrip_clear(); ledStrip_show(); }
#endif
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      Serial.println("Detection complete - drone identified!");
    }

    // Check for heartbeat beep trigger
    portENTER_CRITICAL(&buzzerMux);
    bool do_heartbeat = trigger_heartbeat_beep;
    if (do_heartbeat) trigger_heartbeat_beep = false;
    portEXIT_CRITICAL(&buzzerMux);

    if (do_heartbeat) {
      Serial.println("Heartbeat: Drone still in range");
      for (int hb = 0; hb < 2; hb++) {
#if HAS_BUZZER
        if (ssVolume > 0) tone(BUZZER_PIN, HEARTBEAT_FREQ, HEARTBEAT_DURATION);
#endif
#if LED_PIN >= 0
        if (ssLedBrightness > 0) digitalWrite(LED_PIN, LOW);
#endif
#if HAS_NEOPIXEL
        if (ssLedBrightness > 0) { ledStrip_setColor(0, 0, 255); ledStrip_show(); }
#endif
        vTaskDelay(pdMS_TO_TICKS(100));
#if LED_PIN >= 0
        if (ssLedBrightness > 0) digitalWrite(LED_PIN, HIGH);
#endif
#if HAS_NEOPIXEL
        if (ssLedBrightness > 0) { ledStrip_clear(); ledStrip_show(); }
#endif
        vTaskDelay(pdMS_TO_TICKS(50));
      }
    }
    
    // Dim green heartbeat blink every 10s when idle (no drone in range)
#if HAS_NEOPIXEL
    {
      static unsigned long lastHeartbeatLed = 0;
      unsigned long now = millis();
      if (ssLedBrightness > 0 && !device_in_range && (now - lastHeartbeatLed >= 10000)) {
        lastHeartbeatLed = now;
        ledStrip_setBrightness(15);
        ledStrip_setColor(0, 255, 0);
        ledStrip_show();
        vTaskDelay(pdMS_TO_TICKS(80));
        ledStrip_clear();
        ledStrip_show();
        ledStrip_setBrightness(ssLedBrightness);  // restore to user setting
      }
    }
#endif

    // Check for new beep triggers every 50ms
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void send_json_fast(const id_data *UAV) {
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);
  unsigned long t_ms = millis() - ssSessionStartMs;
  char json_msg[320];
  snprintf(json_msg, sizeof(json_msg),
    "{\"t\":%lu,\"mac\":\"%s\",\"rssi\":%d"
    ",\"drone_lat\":%.6f,\"drone_long\":%.6f"
    ",\"drone_altitude\":%d,\"alt_agl\":%d"
    ",\"spd\":%d,\"hdg\":%d"
    ",\"pilot_lat\":%.6f,\"pilot_long\":%.6f"
    ",\"basic_id\":\"%s\"}",
    t_ms, mac_str, UAV->rssi,
    UAV->lat_d, UAV->long_d,
    UAV->altitude_msl, UAV->height_agl,
    UAV->speed, UAV->heading,
    UAV->base_lat_d, UAV->base_long_d,
    UAV->uav_id);
  Serial.println(json_msg);

#if HAS_SD_CARD
  sdlog_write(5, json_msg);
#endif
}

// Mesh functionality removed - this is now a pure USB serial drone scanner

void bleScanTask(void *parameter) {
  for (;;) {
    if (ssBleOn) {
      NimBLEScanResults foundDevices = pBLEScan->start(1, false);
      pBLEScan->clearResults();
    }
    delay(100);
  }
}

// wifiProcessTask removed — WiFi promiscuous callback handles everything directly

void callback(void *buffer, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  
  wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buffer;
  uint8_t *payload = packet->payload;
  int length = packet->rx_ctrl.sig_len;
  
  static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
  if (memcmp(nan_dest, &payload[4], 6) == 0) {
    char nan_mac[6] = {0};  // receive buffer for source MAC (library writes to this)
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data, nan_mac, payload, length) == 0) {
      id_data UAV;
      memset(&UAV, 0, sizeof(UAV));
      memcpy(UAV.mac, &payload[10], 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.last_seen = millis();
      
      if (UAS_data.BasicIDValid[0]) {
        strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
      }
      if (UAS_data.LocationValid) {
        UAV.lat_d = UAS_data.Location.Latitude;
        UAV.long_d = UAS_data.Location.Longitude;
        UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
        UAV.height_agl = (int)UAS_data.Location.Height;
        UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
        UAV.heading = (int)UAS_data.Location.Direction;
      }
      if (UAS_data.SystemValid) {
        UAV.base_lat_d = UAS_data.System.OperatorLatitude;
        UAV.base_long_d = UAS_data.System.OperatorLongitude;
      }
      if (UAS_data.OperatorIDValid) {
        strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
      }
      
      id_data* storedUAV = next_uav(UAV.mac);
      *storedUAV = UAV;
      storedUAV->flag = 1;
      
      // Trigger buzzer alert (thread-safe, non-blocking)
      portENTER_CRITICAL_ISR(&buzzerMux);
      if (!device_in_range) {
        trigger_detection_beep = true;
        device_in_range = true;
        last_heartbeat = millis();
      }
      portEXIT_CRITICAL_ISR(&buzzerMux);
      
      {
        id_data tmp = *storedUAV;
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(printQueue, &tmp, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
      }
    }
  }
  else if (payload[0] == 0x80) {
    int offset = 36;
    while (offset < length) {
      int typ = payload[offset];
      int len = payload[offset + 1];
      if ((typ == 0xdd) &&
          (((payload[offset + 2] == 0x90 && payload[offset + 3] == 0x3a && payload[offset + 4] == 0xe6)) ||
           ((payload[offset + 2] == 0xfa && payload[offset + 3] == 0x0b && payload[offset + 4] == 0xbc)))) {
        int j = offset + 7;
        if (j < length) {
          memset(&UAS_data, 0, sizeof(UAS_data));
          odid_message_process_pack(&UAS_data, &payload[j], length - j);
          
          id_data UAV;
          memset(&UAV, 0, sizeof(UAV));
          memcpy(UAV.mac, &payload[10], 6);
          UAV.rssi = packet->rx_ctrl.rssi;
          UAV.last_seen = millis();
          
          if (UAS_data.BasicIDValid[0]) {
            strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
          }
          if (UAS_data.LocationValid) {
            UAV.lat_d = UAS_data.Location.Latitude;
            UAV.long_d = UAS_data.Location.Longitude;
            UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
            UAV.height_agl = (int)UAS_data.Location.Height;
            UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
            UAV.heading = (int)UAS_data.Location.Direction;
          }
          if (UAS_data.SystemValid) {
            UAV.base_lat_d = UAS_data.System.OperatorLatitude;
            UAV.base_long_d = UAS_data.System.OperatorLongitude;
          }
          if (UAS_data.OperatorIDValid) {
            strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
          }
          
          id_data* storedUAV = next_uav(UAV.mac);
          *storedUAV = UAV;
          storedUAV->flag = 1;
          
          // Trigger buzzer alert (thread-safe, non-blocking)
          portENTER_CRITICAL_ISR(&buzzerMux);
          if (!device_in_range) {
            trigger_detection_beep = true;
            device_in_range = true;
            last_heartbeat = millis();
          }
          portEXIT_CRITICAL_ISR(&buzzerMux);
          
          {
            id_data tmp = *storedUAV;
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xQueueSendFromISR(printQueue, &tmp, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
          }
        }
      }
      offset += len + 2;
    }
  }
}

void printerTask(void *param) {
  id_data UAV;
  for (;;) {
    if (xQueueReceive(printQueue, &UAV, portMAX_DELAY)) {
      send_json_fast(&UAV);
      // Mesh functionality removed - only JSON output over USB serial
    }
  }
}

void initializeSerial() {
  Serial.begin(115200);
  // Serial1 removed - no mesh functionality
}

void initializeBuzzer() {
#ifndef BOARD_CYD_S3
#if HAS_BUZZER
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#endif
#ifdef SPEAKER_EN_PIN
  pinMode(SPEAKER_EN_PIN, OUTPUT);
  digitalWrite(SPEAKER_EN_PIN, LOW);  // Enable speaker amplifier (active-LOW)
#endif
#endif

  // Read display/LED/volume settings from shared NVS
  Preferences brP;
  brP.begin("ouispy-br", true);
  ssDispBrightness = brP.getUChar("disp", 200);
  ssLedBrightness  = brP.getUChar("led",  50);
  ssVolume         = brP.getUChar("vol",  128);
  brP.end();

  Preferences bzP;
  bzP.begin("ouispy-bz", true);
  ssBleOn = bzP.getBool("blen", true);
  bzP.end();

  Serial.printf("Sky Spy init: disp=%d led=%d vol=%d ble=%s\n",
                ssDispBrightness, ssLedBrightness, ssVolume,
                ssBleOn ? "ON" : "OFF");
}

// Close Encounters of the Third Kind - iconic 5-note motif
// D5, E5, C5, C4, G4 — played fast and punchy
void playCloseEncounters() {
  if (ssVolume == 0) return;

#if HAS_BUZZER
  // The five notes with duration in ms
  struct { int freq; int dur; int gap; } notes[] = {
    { 587, 120,  30 },  // D5
    { 659, 120,  30 },  // E5
    { 523, 120,  30 },  // C5
    { 262, 120,  30 },  // C4 (octave down)
    { 392, 200,   0 },  // G4 (held slightly longer)
  };

  for (int i = 0; i < 5; i++) {
    tone(BUZZER_PIN, notes[i].freq, notes[i].dur);
#if LED_PIN >= 0
    if (ssLedBrightness > 0) digitalWrite(LED_PIN, LOW);
#endif
#if HAS_NEOPIXEL
    if (ssLedBrightness > 0) { ledStrip_setColor(0, 255, 0); ledStrip_show(); }
#endif
    delay(notes[i].dur);
#if LED_PIN >= 0
    if (ssLedBrightness > 0) digitalWrite(LED_PIN, HIGH);
#endif
#if HAS_NEOPIXEL
    if (ssLedBrightness > 0) { ledStrip_clear(); ledStrip_show(); }
#endif
    noTone(BUZZER_PIN);
    if (notes[i].gap > 0) delay(notes[i].gap);
  }
#endif

  Serial.println("[SKY-SPY] *close encounters theme*");
}

void initializeLED() {
#if LED_PIN >= 0
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn off LED initially (inverted logic)
#endif
#if HAS_NEOPIXEL
  ledStrip_init();
  ledStrip_clear();
  ledStrip_show();
#endif
}

// ============================================================================
// Live web dashboard (CYD only)
// ============================================================================
#ifdef BOARD_CYD_S3

static const char SS_HTML[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Sky Spy</title>
<style>*{box-sizing:border-box;margin:0;padding:0}body{background:#111;color:#0f0;font-family:monospace;font-size:14px}
header{background:#000;border-bottom:1px solid #0a0;padding:8px 12px;display:flex;justify-content:space-between;align-items:center}
h1{font-size:16px}#st{font-size:11px;color:#060}
.d{background:#1a1a1a;border:1px solid #0a0;border-radius:4px;margin:8px;padding:8px}
.id{font-size:15px;font-weight:bold}.ag{background:#0a0;color:#000;font-size:10px;padding:1px 4px;border-radius:2px;float:right}
.r{display:flex;justify-content:space-between;margin-top:3px;font-size:12px;color:#0a0}
.p{color:#6af}.em{text-align:center;color:#060;padding:40px 8px}
a.mb{display:block;width:100%;margin-top:6px;padding:6px;background:#0a0;color:#000;text-align:center;text-decoration:none;border-radius:3px;font-weight:bold}
.bt{background:#0a0a0a;border-top:1px solid #040;padding:10px 12px;margin-top:4px}
.bth{color:#0a0;font-size:12px;margin-bottom:5px}
.btb{font-size:11px;color:#060;line-height:1.6}
a.btl{display:none;margin-top:8px;padding:7px 12px;background:#0a0;color:#000;text-decoration:none;border-radius:3px;font-size:12px;font-weight:bold}
</style></head><body>
<header><h1>&#9992; SKY SPY LIVE</h1><span id="st">connecting...</span></header>
<div id="c"></div>
<div class="bt">
<div class="bth">&#128241; BLE LIVE TRACKING &mdash; keeps your internet</div>
<div class="btb" id="btb">detecting browser...</div>
<a href="/ble" class="btl" id="btl">&#8595; DOWNLOAD BLE PAGE</a>
</div>
<script>
(function(){
var u=navigator.userAgent;
var ios=/iPhone|iPad|iPod/.test(u);
var andr=/Android/.test(u);
var chr=/Chrome\//.test(u)&&!/CriOS/.test(u)&&!/EdgA/.test(u);
var t=document.getElementById('btb');
var l=document.getElementById('btl');
if(ios){
  t.innerHTML='iOS: use <b>nRF Connect</b> or <b>LightBlue</b> app.<br>Connect to <b>skyspy-live</b> &rarr; service 6E400001 &rarr; notify 6E400003.';
}else if(andr&&chr){
  t.innerHTML='Download once &rarr; open from Files for live tracking with <b>full internet preserved</b>.';
  l.style.display='inline-block';
}else if(andr){
  t.innerHTML='Use <b>nRF Connect</b>, or try <b>Chrome</b> to download the BLE page.';
  l.style.display='inline-block';
}else{
  t.innerHTML='Download the BLE page and open in Chrome for tracking without losing internet.';
  l.style.display='inline-block';
}
})();
function ag(ms){return ms<2000?'now':ms<60000?(ms/1000|0)+'s':'old'}
function poll(){
  fetch('/api/live').then(r=>r.json()).then(function(d){
    document.getElementById('st').textContent='CH:'+d.ch+' GPS:'+d.sats+'sat '+new Date().toLocaleTimeString();
    var h='';
    if(!d.drones||!d.drones.length){h='<div class="em">No drones detected<br><small>WiFi CH:'+d.ch+'</small></div>';}
    else d.drones.forEach(function(u){
      var m='https://maps.google.com/maps?q='+u.lat+','+u.lon;
      if(u.plat&&u.plon)m='https://maps.google.com/maps?saddr='+u.plat+','+u.plon+'&daddr='+u.lat+','+u.lon;
      h+='<div class="d"><span class="ag">'+ag(u.age)+'</span>';
      h+='<div class="id">'+(u.id||u.mac||'?')+'</div>';
      h+='<div class="r">DRONE: '+u.lat.toFixed(5)+', '+u.lon.toFixed(5)+'</div>';
      h+='<div class="r"><span>ALT:'+u.alt+'m</span><span>RSSI:'+u.rssi+'</span><span>'+u.spd+'m/s '+u.hdg+'&deg;</span></div>';
      if(u.plat&&u.plon)h+='<div class="r p">PILOT: '+u.plat.toFixed(5)+', '+u.plon.toFixed(5)+'</div>';
      h+='<a class="mb" href="'+m+'" target="_blank">OPEN IN MAPS</a></div>';
    });
    document.getElementById('c').innerHTML=h;
  }).catch(function(){document.getElementById('st').textContent='reconnecting...';});
}
poll();setInterval(poll,2000);
</script></body></html>
)=====";

// Web Bluetooth client page — served at /ble; download once, open from file:// thereafter
static const char SS_BLE_HTML[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Sky Spy BLE</title>
<style>*{box-sizing:border-box;margin:0;padding:0}body{background:#111;color:#0f0;font-family:monospace;font-size:14px}
header{background:#000;border-bottom:1px solid #0a0;padding:8px 12px;display:flex;justify-content:space-between;align-items:center}
h1{font-size:16px}#st{font-size:11px;color:#060}
#btn{background:#0a0;color:#000;border:none;padding:10px 20px;font-family:monospace;font-size:14px;font-weight:bold;border-radius:3px;cursor:pointer;margin:12px;display:block}
#btn:disabled{opacity:.4;cursor:default}
.d{background:#1a1a1a;border:1px solid #0a0;border-radius:4px;margin:8px;padding:8px}
.id{font-size:15px;font-weight:bold}.r{display:flex;justify-content:space-between;margin-top:3px;font-size:12px;color:#0a0}
.p{color:#6af}.em{text-align:center;color:#060;padding:24px 12px;line-height:1.7}
a.mb{display:block;width:100%;margin-top:6px;padding:6px;background:#0a0;color:#000;text-align:center;text-decoration:none;border-radius:3px;font-weight:bold}
</style></head><body>
<header><h1>&#9992; SKY SPY BLE</h1><span id="st">disconnected</span></header>
<button id="btn" onclick="conn()">CONNECT BLE</button>
<div id="c"></div>
<script>
var SVC='6e400001-b5a3-f393-e0a9-e50e24dcca9e';
var TX='6e400003-b5a3-f393-e0a9-e50e24dcca9e';
var btn=document.getElementById('btn');
var st=document.getElementById('st');
var c=document.getElementById('c');
if(!navigator.bluetooth){
  btn.style.display='none';
  var u=navigator.userAgent;
  var ios=/iPhone|iPad|iPod/.test(u);
  var chr=/Chrome\//.test(u)&&!/CriOS/.test(u)&&!/EdgA/.test(u);
  var msg;
  if(ios){
    msg='<b>iOS does not support Web Bluetooth.</b><br><br>'+
      'Use the free <b>nRF Connect</b> app (Nordic Semiconductor)<br>'+
      'or <b>LightBlue</b> app (PunchThrough):<br><br>'+
      '1. Scan &rarr; connect to <b>skyspy-live</b><br>'+
      '2. Open service <small>6E400001-B5A3-F393...</small><br>'+
      '3. Tap &#9660; on characteristic <small>6E400003...</small><br>'+
      '4. Drone JSON pushes every 2 seconds.';
  }else if(chr){
    msg='<b>Save this page, then reopen from Files.</b><br><br>'+
      'Web Bluetooth requires a secure context.<br>'+
      'Chrome allows it from locally saved files.<br><br>'+
      '1. Tap &nbsp;&#8942;&nbsp; &rarr; <b>Download page</b><br>'+
      '2. Open <b>Files</b> app &rarr; tap <b>skyspy-ble.html</b><br>'+
      '3. Page reopens in Chrome &rarr; tap <b>CONNECT BLE</b>.';
  }else{
    msg='<b>Open this page in Chrome on Android.</b><br><br>'+
      'Then save it and reopen from the Files app.<br><br>'+
      '<b>iOS:</b> Use <b>nRF Connect</b> or <b>LightBlue</b> app<br>'+
      'and connect to <b>skyspy-live</b>.';
  }
  c.innerHTML='<div class="em">'+msg+'</div>';
}
function conn(){
  st.textContent='scanning...';
  navigator.bluetooth.requestDevice({
    filters:[{name:'skyspy-live'}],
    optionalServices:[SVC]
  }).then(function(d){
    st.textContent='connecting...';
    d.addEventListener('gattserverdisconnected',disc);
    return d.gatt.connect();
  }).then(function(s){return s.getPrimaryService(SVC);})
  .then(function(s){return s.getCharacteristic(TX);})
  .then(function(ch){
    return ch.startNotifications().then(function(){
      ch.addEventListener('characteristicvaluechanged',data);
      st.textContent='connected \u2713';
      btn.disabled=true;
    });
  }).catch(function(e){st.textContent='error: '+e.message;});
}
function disc(){st.textContent='disconnected';btn.disabled=false;}
function data(e){
  try{
    var d=JSON.parse(new TextDecoder().decode(e.target.value));
    st.textContent='CH:'+d.ch+(d.sats?' GPS:'+d.sats+'sat':'');
    if(!d.id){c.innerHTML='<div class="em">No drones detected<br><small>WiFi CH:'+d.ch+'</small></div>';return;}
    var m='https://maps.google.com/maps?q='+d.lat+','+d.lon;
    if(d.plat&&d.plon)m='https://maps.google.com/maps?saddr='+d.plat+','+d.plon+'&daddr='+d.lat+','+d.lon;
    var h='<div class="d"><div class="id">'+(d.id||d.mac||'?')+'</div>';
    h+='<div class="r">DRONE: '+d.lat.toFixed(5)+', '+d.lon.toFixed(5)+'</div>';
    h+='<div class="r"><span>ALT:'+d.alt+'m</span><span>RSSI:'+d.rssi+'</span><span>'+d.spd+'m/s '+d.hdg+'&deg;</span></div>';
    if(d.plat&&d.plon)h+='<div class="r p">PILOT: '+d.plat.toFixed(5)+', '+d.plon.toFixed(5)+'</div>';
    h+='<a class="mb" href="'+m+'" target="_blank">OPEN IN MAPS</a></div>';
    c.innerHTML=h;
  }catch(e){}
}
</script></body></html>
)=====";

static void ssSetupGatt() {
  NimBLEDevice::setMTU(247);
  ssGattSrv = NimBLEDevice::createServer();
  ssGattSrv->setCallbacks(new SsGattCallbacks());
  NimBLEService* pSvc = ssGattSrv->createService(SS_BLE_SVC);
  ssDroneChar = pSvc->createCharacteristic(SS_BLE_TX, NIMBLE_PROPERTY::NOTIFY);
  pSvc->start();
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(SS_BLE_SVC);
  pAdv->setScanResponse(false);
  NimBLEDevice::startAdvertising();
  Serial.println("[SKY-SPY] BLE GATT ready — connect 'skyspy-live' in nRF Connect or Web BT");
}

static void ssSetupServer() {
  ssServer.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", SS_HTML);
  });

  // BLE client page — download once, open from file:// for Web Bluetooth without WiFi
  ssServer.on("/ble", HTTP_GET, [](AsyncWebServerRequest *r) {
    AsyncWebServerResponse *resp = r->beginResponse(200, "text/html", SS_BLE_HTML);
    resp->addHeader("Content-Disposition", "inline; filename=\"skyspy-ble.html\"");
    r->send(resp);
  });

  ssServer.on("/api/live", HTTP_GET, [](AsyncWebServerRequest *r) {
    AsyncResponseStream *resp = r->beginResponseStream("application/json");
    unsigned long now = millis();
    resp->print("{\"ch\":");
    resp->print(ssCurrentChannel);
    resp->print(",\"sats\":");
#if HAS_GPS
    resp->print(ssDevSats);
#else
    resp->print(0);
#endif
    resp->print(",\"drones\":[");
    bool first = true;
    for (int i = 0; i < MAX_UAVS; i++) {
      if (uavs[i].mac[0] == 0) continue;
      if (now - uavs[i].last_seen > 30000) continue;
      if (!first) resp->print(',');
      first = false;
      char mac_str[18];
      snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
               uavs[i].mac[0], uavs[i].mac[1], uavs[i].mac[2],
               uavs[i].mac[3], uavs[i].mac[4], uavs[i].mac[5]);
      resp->printf("{\"mac\":\"%s\",\"id\":\"%s\","
                   "\"lat\":%.6f,\"lon\":%.6f,"
                   "\"alt\":%d,\"rssi\":%d,"
                   "\"spd\":%d,\"hdg\":%d,"
                   "\"age\":%lu",
                   mac_str, uavs[i].uav_id,
                   uavs[i].lat_d, uavs[i].long_d,
                   uavs[i].altitude_msl, uavs[i].rssi,
                   uavs[i].speed, uavs[i].heading,
                   now - uavs[i].last_seen);
      if (uavs[i].base_lat_d != 0.0 || uavs[i].base_long_d != 0.0) {
        resp->printf(",\"plat\":%.6f,\"plon\":%.6f",
                     uavs[i].base_lat_d, uavs[i].base_long_d);
      }
      resp->print('}');
    }
    resp->print("]}");
    r->send(resp);
  });

  ssServer.begin();
  Serial.println("[SKY-SPY] Live dashboard: http://192.168.4.1 (skyspy-live / skyspy123)");
}
#endif // BOARD_CYD_S3

void setup() {
  // Skip setCpuFrequencyMhz(160) — causes UART/SPI issues on CYD (TFT, touch)
  initializeSerial();
  initializeBuzzer();
  initializeLED();
  playCloseEncounters();

  nvs_flash_init();

#ifdef BOARD_CYD_S3
  // AP+STA: softAP on CH6 for live dashboard, promiscuous STA for drone scanning
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("skyspy-live", "skyspy123", 6);
#else
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
#endif

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
  ssChIdx = 0;
  ssCurrentChannel = 6;
  ssChSwitchTime = millis();
  ssSessionStartMs = millis();

#ifdef BOARD_CYD_S3
  NimBLEDevice::init("skyspy-live");
  ssSetupGatt();   // MUST be before getScan() / scan task start
#else
  NimBLEDevice::init("DroneID");
#endif
  pBLEScan = NimBLEDevice::getScan();

#ifdef ENABLE_TFT_DISPLAY
  display_create_sprite();
#endif
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
#ifdef BOARD_CYD_S3
  // Low duty-cycle scan so GATT advertising has radio airtime.
  // Interval=320 (200ms), Window=32 (20ms) → 10% scan, 90% free for advertising.
  pBLEScan->setInterval(320);
  pBLEScan->setWindow(32);
#endif

  printQueue = xQueueCreate(MAX_UAVS, sizeof(id_data));

  xTaskCreatePinnedToCore(bleScanTask, "BLEScanTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(printerTask, "PrinterTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(buzzerTask, "BuzzerTask", 4096, NULL, 1, NULL, 1);
  Serial.printf("[SKY-SPY] Ready, heap: %u bytes\n", ESP.getFreeHeap());
  
  memset(uavs, 0, sizeof(uavs));

#if HAS_GPS
  // Reinitialize UART0 for GPS after WiFi/BLE init
  Serial0.end();
  delay(50);
  Serial0.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
#endif

#if HAS_SD_CARD
  ssRegistryLoad();
#endif

#ifdef BOARD_CYD_S3
  ssSetupServer();
#endif

#ifdef ENABLE_TFT_DISPLAY
  display_skyspy_scanning(0, 0, 0, 0, ssCurrentChannel);
#endif
}

#ifdef ENABLE_TFT_DISPLAY
static void refreshDisplay(unsigned long now) {
  int total = 0;
  int bestIdx = -1;
  uint32_t bestSeen = 0;
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].mac[0] != 0) {
      total++;
      if (uavs[i].last_seen > bestSeen) {
        bestSeen = uavs[i].last_seen;
        bestIdx = i;
      }
    }
  }

  if (bestIdx >= 0 && (now - bestSeen) < 7000) {
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             uavs[bestIdx].mac[0], uavs[bestIdx].mac[1],
             uavs[bestIdx].mac[2], uavs[bestIdx].mac[3],
             uavs[bestIdx].mac[4], uavs[bestIdx].mac[5]);
    // Registry: look up drone and update cross-session tracking
    int sessionsSeen = 0;
#if HAS_SD_CARD
    if (uavs[bestIdx].uav_id[0]) {
      sessionsSeen = (int)ssRegistryUpdate(
        uavs[bestIdx].uav_id,
        uavs[bestIdx].op_id,
        mac_str,
        uavs[bestIdx].base_lat_d,
        uavs[bestIdx].base_long_d
      );
    }
#endif
    display_skyspy(mac_str, uavs[bestIdx].uav_id,
                   uavs[bestIdx].lat_d, uavs[bestIdx].long_d,
                   uavs[bestIdx].altitude_msl, uavs[bestIdx].rssi, total,
                   uavs[bestIdx].base_lat_d, uavs[bestIdx].base_long_d,
#if HAS_GPS
                   ssDevSats,
#else
                   0,
#endif
                   ssCurrentChannel, sessionsSeen);
#ifdef BOARD_CYD_S3
    if (ssPhoneConn && ssDroneChar) {
      char notif[220];
      snprintf(notif, sizeof(notif),
               "{\"id\":\"%s\",\"mac\":\"%s\","
               "\"lat\":%.6f,\"lon\":%.6f,"
               "\"alt\":%d,\"rssi\":%d,\"spd\":%d,\"hdg\":%d,"
               "\"plat\":%.6f,\"plon\":%.6f,"
               "\"ch\":%d,\"sats\":%d}",
               uavs[bestIdx].uav_id, mac_str,
               uavs[bestIdx].lat_d, uavs[bestIdx].long_d,
               uavs[bestIdx].altitude_msl, uavs[bestIdx].rssi,
               uavs[bestIdx].speed, uavs[bestIdx].heading,
               uavs[bestIdx].base_lat_d, uavs[bestIdx].base_long_d,
               ssCurrentChannel,
#if HAS_GPS
               ssDevSats
#else
               0
#endif
      );
      ssDroneChar->setValue((uint8_t*)notif, strlen(notif));
      ssDroneChar->notify();
    }
#endif
  } else {
    display_skyspy_scanning(total,
#if HAS_GPS
                            ssDevLat, ssDevLon, ssDevSats,
#else
                            0, 0, 0,
#endif
                            ssCurrentChannel);
#ifdef BOARD_CYD_S3
    if (ssPhoneConn && ssDroneChar) {
      char notif[48];
      snprintf(notif, sizeof(notif), "{\"id\":\"\",\"ch\":%d,\"sats\":%d}",
               ssCurrentChannel,
#if HAS_GPS
               ssDevSats
#else
               0
#endif
      );
      ssDroneChar->setValue((uint8_t*)notif, strlen(notif));
      ssDroneChar->notify();
    }
#endif
  }
}
#endif

void loop() {
  unsigned long current_millis = millis();

#if HAS_GPS
  // Feed onboard GPS from Serial0 (UART0)
  while (Serial0.available()) {
    if (ssGPS.encode(Serial0.read())) {
      if (ssGPS.location.isValid() && ssGPS.location.isUpdated()) {
        ssDevLat = ssGPS.location.lat();
        ssDevLon = ssGPS.location.lng();
      }
      if (ssGPS.satellites.isValid()) {
        ssDevSats = (int)ssGPS.satellites.value();
      }
    }
  }

#endif

  // Channel hopping — dwell 500ms on CH6, 100ms on others
  {
    unsigned long dwell = (ssCurrentChannel == 6) ? CH_DWELL_NAN : CH_DWELL_OTHER;
    if (current_millis - ssChSwitchTime >= dwell) {
      ssChIdx = (ssChIdx + 1) % CH_HOP_COUNT;
      ssCurrentChannel = CH_HOP_SEQUENCE[ssChIdx];
      esp_wifi_set_channel(ssCurrentChannel, WIFI_SECOND_CHAN_NONE);
      ssChSwitchTime = current_millis;
    }
  }

  // Status message every 60 seconds
  if ((current_millis - last_status) > 60000UL) {
    Serial.println("{\"status\":\"scanning\"}");
    last_status = current_millis;
  }

#ifdef ENABLE_TFT_DISPLAY
  // Update display every 2 seconds (only when not in settings)
  {
    static unsigned long lastDisplayUpdate = 0;
    if (!ssInSettings && current_millis - lastDisplayUpdate >= 2000) {
      lastDisplayUpdate = current_millis;
      refreshDisplay(current_millis);
    }
  }

  // Touch handling (CYD only) — poll every loop for responsiveness
  {
    if (!ssInSettings) {
      int touchAction = display_skyspy_touch();
      if (touchAction == 1) {
        // Wait for finger release before entering settings (prevent bleed)
        int16_t _tx, _ty;
        while (display_touch_read(_tx, _ty)) { delay(20); }
        delay(100);
        ssInSettings = true;
        display_skyspy_settings(ssDispBrightness, ssLedBrightness, ssVolume, ssBleOn, ssIncognito);
      }
    } else {
      int touchAction = display_settings_touch();
      if (touchAction == 1) {         // DSP +  (10% steps → 26/255)
        ssDispBrightness = (ssDispBrightness + 26 <= 255) ? ssDispBrightness + 26 : 255;
        display_set_brightness(ssDispBrightness);
        Preferences p; p.begin("ouispy-br", false); p.putUChar("disp", ssDispBrightness); p.end();
        display_skyspy_settings(ssDispBrightness, ssLedBrightness, ssVolume, ssBleOn, ssIncognito);
      } else if (touchAction == 2) {  // DSP -
        ssDispBrightness = (ssDispBrightness >= 26) ? ssDispBrightness - 26 : 0;
        display_set_brightness(ssDispBrightness);
        Preferences p; p.begin("ouispy-br", false); p.putUChar("disp", ssDispBrightness); p.end();
        display_skyspy_settings(ssDispBrightness, ssLedBrightness, ssVolume, ssBleOn, ssIncognito);
      } else if (touchAction == 3) {  // LED +
        ssLedBrightness = (ssLedBrightness + 26 <= 255) ? ssLedBrightness + 26 : 255;
        ledStrip_setBrightness(ssLedBrightness);
        ledStrip_show();
        Preferences p; p.begin("ouispy-br", false); p.putUChar("led", ssLedBrightness); p.end();
        display_skyspy_settings(ssDispBrightness, ssLedBrightness, ssVolume, ssBleOn, ssIncognito);
      } else if (touchAction == 4) {  // LED -
        ssLedBrightness = (ssLedBrightness >= 26) ? ssLedBrightness - 26 : 0;
        if (ssLedBrightness == 0) { ledStrip_clear(); ledStrip_show(); }
        else { ledStrip_setBrightness(ssLedBrightness); ledStrip_show(); }
        Preferences p; p.begin("ouispy-br", false); p.putUChar("led", ssLedBrightness); p.end();
        display_skyspy_settings(ssDispBrightness, ssLedBrightness, ssVolume, ssBleOn, ssIncognito);
      } else if (touchAction == 5) {  // VOL +
        ssVolume = (ssVolume + 26 <= 255) ? ssVolume + 26 : 255;
#ifdef BOARD_CYD_S3
        cyd_set_volume(ssVolume);
#endif
        Preferences p; p.begin("ouispy-br", false); p.putUChar("vol", ssVolume); p.end();
        display_skyspy_settings(ssDispBrightness, ssLedBrightness, ssVolume, ssBleOn, ssIncognito);
      } else if (touchAction == 6) {  // VOL -
        ssVolume = (ssVolume >= 26) ? ssVolume - 26 : 0;
#ifdef BOARD_CYD_S3
        cyd_set_volume(ssVolume);
#endif
        Preferences p; p.begin("ouispy-br", false); p.putUChar("vol", ssVolume); p.end();
        display_skyspy_settings(ssDispBrightness, ssLedBrightness, ssVolume, ssBleOn, ssIncognito);
      } else if (touchAction == 7) {  // BLE toggle
        ssBleOn = !ssBleOn;
        Preferences p; p.begin("ouispy-bz", false); p.putBool("blen", ssBleOn); p.end();
        display_skyspy_settings(ssDispBrightness, ssLedBrightness, ssVolume, ssBleOn, ssIncognito);
      } else if (touchAction == 8) {  // INCOGNITO toggle
        ssIncognito = !ssIncognito;
        if (ssIncognito) {
          ssSavedDisp = ssDispBrightness;
          ssSavedLed  = ssLedBrightness;
          display_set_brightness(0);
          ledStrip_clear();
          ledStrip_show();
        } else {
          ssDispBrightness = ssSavedDisp;
          ssLedBrightness  = ssSavedLed;
          display_set_brightness(ssDispBrightness);
          ledStrip_setBrightness(ssLedBrightness);
          ledStrip_show();
        }
        display_skyspy_settings(ssDispBrightness, ssLedBrightness, ssVolume, ssBleOn, ssIncognito);
      } else if (touchAction == 9) {  // BACK
        // Clear incognito on exit
        if (ssIncognito) {
          ssIncognito = false;
          ssDispBrightness = ssSavedDisp;
          ssLedBrightness  = ssSavedLed;
          display_set_brightness(ssDispBrightness);
          ledStrip_setBrightness(ssLedBrightness);
          ledStrip_show();
        }
        ssInSettings = false;
        // Wait for release to prevent touch bleeding back to main screen
        int16_t _tx, _ty;
        while (display_touch_read(_tx, _ty)) { delay(20); }
        delay(100);
        refreshDisplay(current_millis);
      }
    }
  }
#endif

  // Handle heartbeat pulse if drone is in range (thread-safe)
  portENTER_CRITICAL(&buzzerMux);
  bool in_range = device_in_range;
  portEXIT_CRITICAL(&buzzerMux);
  
  if (in_range) {
    // Check if 5 seconds have passed since last heartbeat
    if (current_millis - last_heartbeat >= 5000) {
      portENTER_CRITICAL(&buzzerMux);
      trigger_heartbeat_beep = true;
      portEXIT_CRITICAL(&buzzerMux);
      last_heartbeat = current_millis;
    }
    
    // Check if drone has gone out of range (no detection for 7 seconds)
    bool drone_still_detected = false;
    for (int i = 0; i < MAX_UAVS; i++) {
      if (uavs[i].mac[0] != 0 && (current_millis - uavs[i].last_seen) < 7000) {
        drone_still_detected = true;
        break;
      }
    }
    
    if (!drone_still_detected) {
      Serial.println("Drone out of range - stopping heartbeat");
      portENTER_CRITICAL(&buzzerMux);
      device_in_range = false;
      portEXIT_CRITICAL(&buzzerMux);
    }
  }
}
