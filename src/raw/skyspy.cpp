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

// Channel hopping state
// Dwell 500ms on CH6 (NAN discovery), 100ms on each other channel (beacons)
static const int CH_HOP_SEQUENCE[] = {6, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13};
static const int CH_HOP_COUNT = sizeof(CH_HOP_SEQUENCE) / sizeof(CH_HOP_SEQUENCE[0]);
static const unsigned long CH_DWELL_NAN = 500;   // ms on CH6
static const unsigned long CH_DWELL_OTHER = 100;  // ms on other channels
static int ssChIdx = 0;
static int ssCurrentChannel = 6;
static unsigned long ssChSwitchTime = 0;

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
  char json_msg[256];
  snprintf(json_msg, sizeof(json_msg),
    "{\"mac\":\"%s\",\"rssi\":%d,\"drone_lat\":%.6f,\"drone_long\":%.6f,\"drone_altitude\":%d,\"pilot_lat\":%.6f,\"pilot_long\":%.6f,\"basic_id\":\"%s\"}",
    mac_str, UAV->rssi, UAV->lat_d, UAV->long_d, UAV->altitude_msl,
    UAV->base_lat_d, UAV->base_long_d, UAV->uav_id);
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

void setup() {
  // Skip setCpuFrequencyMhz(160) — causes UART/SPI issues on CYD (TFT, touch)
  initializeSerial();
  initializeBuzzer();
  initializeLED();
  playCloseEncounters();

  nvs_flash_init();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
  ssChIdx = 0;
  ssCurrentChannel = 6;
  ssChSwitchTime = millis();

  NimBLEDevice::init("DroneID");
  pBLEScan = NimBLEDevice::getScan();

#ifdef ENABLE_TFT_DISPLAY
  display_create_sprite();
#endif
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);

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
    display_skyspy(mac_str, uavs[bestIdx].uav_id,
                   uavs[bestIdx].lat_d, uavs[bestIdx].long_d,
                   uavs[bestIdx].altitude_msl, uavs[bestIdx].rssi, total,
                   uavs[bestIdx].base_lat_d, uavs[bestIdx].base_long_d,
#if HAS_GPS
                   ssDevSats,
#else
                   0,
#endif
                   ssCurrentChannel);
  } else {
    display_skyspy_scanning(total,
#if HAS_GPS
                            ssDevLat, ssDevLon, ssDevSats,
#else
                            0, 0, 0,
#endif
                            ssCurrentChannel);
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
