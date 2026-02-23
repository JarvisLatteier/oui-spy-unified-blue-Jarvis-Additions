/*
 * OUI SPY - Unified Firmware Boot Selector
 * colonelpanichacks
 *
 * On boot: creates AP "ouispy" with web UI to select firmware mode 1-5.
 * After selection, stores mode in NVS and reboots into that firmware.
 * To return to selector: double-tap the reset button quickly.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include "modes.h"
#include "board_config.h"
#include "display.h"
#include "sdlog.h"
#include "led_strip.h"

#ifdef BOARD_CYD_S3
#include "cyd_audio.h"
#endif

// Hardware pins — sourced from board_config.h (already defined there)
// BUZZER_PIN, LED_PIN, NEOPIXEL_PIN come from board_config.h

// Boot button (GPIO0) - held during boot to return to selector menu
#define BOOT_BUTTON_PIN 0
#define BOOT_HOLD_TIME 1500  // ms - hold boot button this long to force selector
#define SHORT_PRESS_MS 500   // ms - release within this for short press

static Preferences prefs;
static AsyncWebServer selectorServer(80);
static int currentMode = 0;

// AP configuration (loaded from NVS, user-configurable via web UI)
static String apSSID = "oui-spy";
static String apPassword = "ouispy123";

// Buzzer configuration (shared across all modes via NVS)
static bool buzzerEnabled = true;

// Brightness settings (0-255, persisted in NVS)
static uint8_t displayBrightness = 200;  // TFT backlight PWM duty
static uint8_t ledBrightness = 128;      // LED strip brightness

// Volume setting (0-255, persisted in NVS, exposed to mode wrappers)
uint8_t buzzerVolume = 200;

// Selector TFT menu index (for short-press navigation)
static int selectorIndex = 0;
#ifdef BOARD_CYD_S3
static const int SELECTOR_MODES[] = {4, 5};
static const char* SELECTOR_NAMES[] = {"FLOCK-YOU", "SKY SPY"};
static const int NUM_SELECTOR_MODES = 2;
#else
static const int SELECTOR_MODES[] = {1, 2, 4, 5};
static const char* SELECTOR_NAMES[] = {"DETECTOR", "FOXHUNTER", "FLOCK-YOU", "SKY SPY"};
static const int NUM_SELECTOR_MODES = 4;
#endif

// ============================================================================
// AP Config Storage (NVS)
// ============================================================================
static void loadAPConfig() {
    Preferences apPrefs;
    apPrefs.begin("ouispy-ap", true);  // read-only
    apSSID = apPrefs.getString("ssid", "oui-spy");
    apPassword = apPrefs.getString("pass", "ouispy123");
    apPrefs.end();
    Serial.printf("[OUI-SPY] Loaded AP config: SSID='%s' PASS='%s'\n", apSSID.c_str(), apPassword.c_str());
}

static void saveAPConfig(const String& ssid, const String& pass) {
    Preferences apPrefs;
    apPrefs.begin("ouispy-ap", false);
    apPrefs.putString("ssid", ssid);
    apPrefs.putString("pass", pass);
    apPrefs.end();
    Serial.printf("[OUI-SPY] Saved AP config: SSID='%s' PASS='%s'\n", ssid.c_str(), pass.c_str());
}

// ============================================================================
// Buzzer Config Storage (NVS) — shared across all modes
// ============================================================================
static void loadBuzzerConfig() {
    Preferences bzPrefs;
    bzPrefs.begin("ouispy-bz", true);
    buzzerEnabled = bzPrefs.getBool("on", true);
    bzPrefs.end();
    Serial.printf("[OUI-SPY] Buzzer: %s\n", buzzerEnabled ? "ON" : "OFF");
}

static void saveBuzzerConfig(bool enabled) {
    Preferences bzPrefs;
    bzPrefs.begin("ouispy-bz", false);
    bzPrefs.putBool("on", enabled);
    bzPrefs.end();
    buzzerEnabled = enabled;
    Serial.printf("[OUI-SPY] Buzzer saved: %s\n", enabled ? "ON" : "OFF");
}

// ============================================================================
// Brightness Config Storage (NVS) — display + LED
// ============================================================================
static void loadBrightnessConfig() {
    Preferences brPrefs;
    brPrefs.begin("ouispy-br", true);
    displayBrightness = brPrefs.getUChar("disp", 200);
    ledBrightness = brPrefs.getUChar("led", 128);
    buzzerVolume = brPrefs.getUChar("vol", 200);
    brPrefs.end();
    Serial.printf("[OUI-SPY] Brightness: display=%d led=%d vol=%d\n",
                  displayBrightness, ledBrightness, buzzerVolume);
}

static void saveBrightnessConfig() {
    Preferences brPrefs;
    brPrefs.begin("ouispy-br", false);
    brPrefs.putUChar("disp", displayBrightness);
    brPrefs.putUChar("led", ledBrightness);
    brPrefs.putUChar("vol", buzzerVolume);
    brPrefs.end();
    Serial.printf("[OUI-SPY] Brightness saved: display=%d led=%d vol=%d\n",
                  displayBrightness, ledBrightness, buzzerVolume);
}

// ============================================================================
// MAC Address Randomization
// ============================================================================
static void randomizeMAC() {
    uint8_t mac[6];
    // Generate random bytes using ESP32 hardware RNG
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    mac[0] = (r1 >> 0) & 0xFF;
    mac[1] = (r1 >> 8) & 0xFF;
    mac[2] = (r1 >> 16) & 0xFF;
    mac[3] = (r1 >> 24) & 0xFF;
    mac[4] = (r2 >> 0) & 0xFF;
    mac[5] = (r2 >> 8) & 0xFF;
    // Set locally administered bit (bit 1 of first byte) and clear multicast bit (bit 0)
    mac[0] = (mac[0] | 0x02) & 0xFE;

    esp_wifi_set_mac(WIFI_IF_AP, mac);
    Serial.printf("[OUI-SPY] Randomized MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ============================================================================
// Selector Web UI HTML
// ============================================================================
static const char SELECTOR_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>OUI SPY</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html{height:100%;height:-webkit-fill-available;overflow:hidden}
body{margin:0;height:100vh;height:-webkit-fill-available;font-family:monospace;background:#000;color:#0f0;display:flex;flex-direction:column;padding:4px;overflow:hidden}
.t{flex:1;display:flex;flex-direction:column;border:2px solid #0f0;padding:6px;overflow:hidden;min-height:0}
.h{text-align:center;padding-bottom:3px;margin-bottom:3px;border-bottom:1px solid #0f0;flex-shrink:0}
.ti{font-size:24px;font-weight:bold;letter-spacing:2px}
.s{font-size:8px;margin-top:1px;opacity:.7}
#x{flex:1;display:flex;flex-direction:column;min-height:0;overflow:hidden}
.m{flex:1;display:flex;flex-direction:column;min-height:0;overflow:hidden}
.i{flex:1;display:flex;flex-direction:column;justify-content:center;align-items:center;border:2px solid #0f0;border-bottom:0;cursor:pointer;background:#000;text-align:center;min-height:0;overflow:hidden}
.i:last-child{border-bottom:2px solid #0f0}
.i:active{background:#0f0;color:#000}
.n{font-size:18px;font-weight:bold;letter-spacing:1px}
.d{font-size:9px;opacity:.7;margin-top:1px}
.ap{display:flex;gap:3px;align-items:center;margin-top:4px;border-top:1px solid #0f0;padding-top:4px;flex-shrink:0}
.ap input{flex:1;padding:4px;background:#000;color:#0f0;border:1px solid #0f0;font-family:monospace;font-size:11px;min-width:0}
.ap input:focus{outline:none;border-color:#fff;color:#fff}
.ap .sb{padding:4px 7px;background:#0f0;color:#000;border:none;font-family:monospace;font-size:10px;font-weight:bold;cursor:pointer;white-space:nowrap}
.ap .sb:active{background:#fff}
.bz{display:flex;align-items:center;white-space:nowrap;cursor:pointer;font-size:9px;gap:2px;opacity:.7}
.bz:hover{opacity:1}
.bz input{margin:0;cursor:pointer}
.sl{display:flex;gap:4px;align-items:center;margin-top:3px;border-top:1px solid #0f0;padding-top:12px;padding-bottom:12px;flex-shrink:0;font-size:9px;opacity:.8}
.sl label{white-space:nowrap;min-width:28px}
.sl input[type=range]{flex:1;-webkit-appearance:none;height:4px;background:#0f0;outline:none;border-radius:2px;min-width:0}
.sl input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:14px;background:#0f0;border-radius:50%;cursor:pointer}
.sl span{min-width:24px;text-align:right;font-size:8px}
.f{padding-top:2px;margin-top:3px;font-size:7px;text-align:center;opacity:.5;flex-shrink:0}
.boot{flex:1;display:flex;flex-direction:column;justify-content:center;align-items:center;text-align:center;padding:20px}
.bt{font-size:28px;font-weight:bold;margin-bottom:16px;letter-spacing:2px}
.bs{font-size:12px;line-height:1.5;margin-bottom:16px;opacity:.9;max-width:500px}
.br{font-size:13px}
@keyframes b{0%,50%{opacity:1}51%,100%{opacity:0}}
.blink{animation:b 1s infinite}
</style></head><body>
<div class="t">
<div class="h"><div class="ti">OUI SPY</div><div class="s">FIRMWARE SELECTOR</div></div>
<div id="x">
<div class="m">
<div class="i" onclick="go(1)"><div class="n">DETECTOR</div><div class="d">BLE Alert Tool for Specific Devices</div></div>
<div class="i" onclick="go(2)"><div class="n">FOXHUNTER</div><div class="d">RSSI Proximity Tracker</div></div>
<div class="i" onclick="go(4)"><div class="n">FLOCK-YOU</div><div class="d">Surveillance Detector &bull; AP: flockyou</div></div>
<div class="i" onclick="go(5)"><div class="n">SKY SPY</div><div class="d">Drone Remote ID Monitor</div></div>
</div>
<div class="ap">
<input type="text" id="ap_ssid" placeholder="SSID" maxlength="32" value="%SSID%">
<input type="text" id="ap_pass" placeholder="PASSWORD" maxlength="63" value="%PASS%">
<button class="sb" onclick="saveAP()">SET</button>
<label class="bz"><input type="checkbox" id="bz" onchange="saveBZ(this.checked)" %BUZZER%>BZR</label>
<label class="bz"><input type="checkbox" id="inc" onchange="setIncognito(this.checked)">INCOGNITO</label>
</div>
<div class="sl">
<label>DSP</label><input type="range" id="br_d" min="10" max="255" value="%DISP_BR%" oninput="dv.textContent=this.value" onchange="setBr()"><span id="dv">%DISP_BR%</span>
<label>LED</label><input type="range" id="br_l" min="0" max="255" value="%LED_BR%" oninput="lv.textContent=this.value" onchange="setBr()"><span id="lv">%LED_BR%</span>
</div>
<div class="f" id="ft">Hold BOOT button on dongle 2s for menu &bull; MAC randomized</div>
</div>
<div id="y" class="boot" style="display:none">
<div class="bt" id="yt"></div>
<div class="bs" id="ys"></div>
<div class="br">REBOOTING<span class="blink">_</span></div>
</div>
</div>
<script>
var info={1:{t:'DETECTOR',s:'Scans for BLE devices and alerts when specific targets are detected. Configure OUI prefixes and MAC addresses to monitor.'},2:{t:'FOXHUNTER',s:'Track down a specific device using RSSI signal strength. Beeps get faster as you get closer to your target.'},4:{t:'FLOCK-YOU',s:'Detects Flock Safety surveillance cameras via BLE. Serves web dashboard on AP flockyou with live detections, pattern DB, and JSON/CSV export.'},5:{t:'SKY SPY',s:'Monitors for FAA Remote ID broadcasts from drones. Detects Open Drone ID signals over WiFi and BLE.'}};
function go(m){var d=info[m];document.getElementById('yt').textContent=d.t;document.getElementById('ys').textContent=d.s;document.getElementById('x').style.display='none';document.getElementById('y').style.display='flex';fetch('/select?mode='+m)}
function saveAP(){
var s=document.getElementById('ap_ssid').value.trim();
var p=document.getElementById('ap_pass').value.trim();
var ft=document.getElementById('ft');
if(s.length<1||s.length>32){ft.textContent='SSID must be 1-32 chars';return}
if(p.length>0&&p.length<8){ft.textContent='Password must be 8+ chars or empty';return}
ft.textContent='SAVING...';
fetch('/saveap?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)).then(function(r){
if(r.ok){ft.textContent='SAVED! REBOOTING...'}else{ft.textContent='ERROR'}
}).catch(function(){ft.textContent='ERROR'})}
function saveBZ(on){fetch('/buzzer?on='+(on?'1':'0'))}
function setBr(){fetch('/brightness?disp='+document.getElementById('br_d').value+'&led='+document.getElementById('br_l').value)}
var _sd=0,_sl=0;
function setIncognito(on){var d=document.getElementById('br_d'),l=document.getElementById('br_l');if(on){_sd=d.value;_sl=l.value;d.value=0;l.value=0;dv.textContent='0';lv.textContent='0';fetch('/brightness?disp=0&led=0')}else{d.value=_sd;l.value=_sl;dv.textContent=_sd;lv.textContent=_sl;setBr()}}
</script></body></html>
)rawliteral";

// ============================================================================
// Boot Jingle for Selector - Zelda "Secret Discovered" Style
// ============================================================================
static void playNote(int freq, int duration) {
#if HAS_BUZZER
#ifdef BOARD_CYD_S3
    cyd_tone(BUZZER_PIN, freq, duration);
    delay(duration);
    cyd_noTone(BUZZER_PIN);
#else
    ledcSetup(BUZZER_LEDC_CH, freq, 8);
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
    ledcWrite(BUZZER_LEDC_CH, 100);
    delay(duration);
    ledcWrite(BUZZER_LEDC_CH, 0);
#endif
#else
    delay(duration);
#endif
}

static void selectorBeep() {
#if HAS_BUZZER
    // "Secret discovered" ascending jingle
    playNote(784, 150);   // G5
    delay(20);
    playNote(988, 150);   // B5
    delay(20);
    playNote(1175, 150);  // D6
    delay(20);
    playNote(1568, 400);  // G6 (hold)
    delay(100);
#endif

    // LED flash sync
#if LED_PIN >= 0
    pinMode(LED_PIN, OUTPUT);
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, LOW);   // On
        delay(50);
        digitalWrite(LED_PIN, HIGH);  // Off
        delay(50);
    }
#elif HAS_APA102 || HAS_NEOPIXEL
    for (int i = 0; i < 3; i++) {
        ledStrip_setColor(0, 255, 0);
        ledStrip_show();
        delay(50);
        ledStrip_clear();
        ledStrip_show();
        delay(50);
    }
#endif
}

// ============================================================================
// Boot Button Detection (GPIO0)
// ============================================================================
// Hold the BOOT button during startup to return to selector menu.
// Beeps while waiting so you know it's detecting the hold.
static bool checkBootButton() {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    // Quick check - is button even pressed?
    if (digitalRead(BOOT_BUTTON_PIN) == HIGH) {
        Serial.println("[OUI-SPY] Boot button not pressed");
        return false;  // Not pressed, skip
    }

    Serial.println("[OUI-SPY] Boot button PRESSED - hold to return to menu...");
    Serial.flush();

    // Button is pressed - wait for hold duration with beep feedback
    unsigned long start = millis();
    while (millis() - start < BOOT_HOLD_TIME) {
        if (digitalRead(BOOT_BUTTON_PIN) == HIGH) {
            Serial.println("[OUI-SPY] Boot button released too early");
            return false;  // Released before threshold
        }
        // Quick beep feedback every 300ms so user knows it's working
#if HAS_BUZZER && !defined(BOARD_CYD_S3)
        if ((millis() - start) % 300 < 50) {
            ledcSetup(BUZZER_LEDC_CH, 2000, 8);
            ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
            ledcWrite(BUZZER_LEDC_CH, 80);
        } else {
            ledcWrite(BUZZER_LEDC_CH, 0);
        }
#endif
        delay(10);
    }
#if HAS_BUZZER && !defined(BOARD_CYD_S3)
    ledcWrite(BUZZER_LEDC_CH, 0);  // Stop beeping
#endif

    Serial.println("[OUI-SPY] *** BOOT BUTTON HELD *** -> FORCING SELECTOR");
    Serial.flush();

    // Clear the stored mode
    prefs.begin("unified-mode", false);
    prefs.putInt("mode", 0);
    prefs.end();
    return true;
}

// ============================================================================
// SD Card Web Endpoints (serve log files over WiFi)
// ============================================================================
#if HAS_SD_CARD
#include <SD_MMC.h>

static void setupSDWebEndpoints() {
    // List log files
    selectorServer.on("/sd/list", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!sdlog_available()) {
            request->send(503, "application/json", "{\"error\":\"No SD card\"}");
            return;
        }
        String json = "[";
        const char* dirs[] = {"detector", "foxhunter", "flockyou", "skyspy"};
        bool first = true;
        for (int d = 0; d < 4; d++) {
            char path[32];
            snprintf(path, sizeof(path), "/oui-spy/%s", dirs[d]);
            File dir = SD_MMC.open(path);
            if (!dir || !dir.isDirectory()) continue;
            File f = dir.openNextFile();
            while (f) {
                if (!first) json += ",";
                first = false;
                json += "{\"file\":\"";
                json += dirs[d];
                json += "/";
                json += f.name();
                json += "\",\"size\":";
                json += String((unsigned long)f.size());
                json += "}";
                f = dir.openNextFile();
            }
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    // Download a specific log file
    selectorServer.on("/sd/download", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!sdlog_available()) {
            request->send(503, "text/plain", "No SD card");
            return;
        }
        if (!request->hasParam("file")) {
            request->send(400, "text/plain", "Missing file param");
            return;
        }
        String fname = "/oui-spy/" + request->getParam("file")->value();
        if (!SD_MMC.exists(fname)) {
            request->send(404, "text/plain", "Not found");
            return;
        }
        request->send(SD_MMC, fname, "application/octet-stream");
    });

    // Clear all logs (requires ?confirm=yes)
    selectorServer.on("/sd/clear", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("confirm") ||
            request->getParam("confirm")->value() != "yes") {
            request->send(400, "text/plain", "Add ?confirm=yes");
            return;
        }
        const char* dirs[] = {
            "/oui-spy/detector", "/oui-spy/foxhunter",
            "/oui-spy/flockyou", "/oui-spy/skyspy"
        };
        int removed = 0;
        for (int d = 0; d < 4; d++) {
            File dir = SD_MMC.open(dirs[d]);
            if (!dir || !dir.isDirectory()) continue;
            File f = dir.openNextFile();
            while (f) {
                char fullPath[64];
                snprintf(fullPath, sizeof(fullPath), "%s/%s", dirs[d], f.name());
                f = dir.openNextFile();
                SD_MMC.remove(fullPath);
                removed++;
            }
        }
        char resp[32];
        snprintf(resp, sizeof(resp), "Removed %d files", removed);
        request->send(200, "text/plain", resp);
    });
}
#endif // HAS_SD_CARD

// ============================================================================
// Selector Mode - AP + Web UI
// ============================================================================
static void startSelector() {
    // Load user-configured AP credentials and buzzer setting from NVS
    loadAPConfig();
    loadBuzzerConfig();

    Serial.println("\n========================================");
    Serial.println("  OUI SPY - Firmware Selector");
    Serial.printf("  Connect to WiFi: %s\n", apSSID.c_str());
    Serial.printf("  Password: %s\n", apPassword.c_str());
    Serial.println("  Open: http://192.168.4.1");
    Serial.println("========================================\n");
    Serial.flush();

    // Clean WiFi init from OFF state (setup() already nuked everything)
    Serial.println("[SELECTOR] Initializing WiFi AP...");
    Serial.flush();
    WiFi.persistent(false);       // Don't save this config back to NVS
    WiFi.mode(WIFI_AP);
    delay(200);

    // Randomize MAC address every boot for privacy
    randomizeMAC();

    Serial.printf("[SELECTOR] Starting AP: %s...\n", apSSID.c_str());
    Serial.flush();
    bool apStarted;
    if (apPassword.length() >= 8) {
        apStarted = WiFi.softAP(apSSID.c_str(), apPassword.c_str());
    } else {
        // Open network (no password or too short for WPA2)
        apStarted = WiFi.softAP(apSSID.c_str());
    }
    Serial.printf("[SELECTOR] AP started: %s\n", apStarted ? "SUCCESS" : "FAILED");
    Serial.print("[SELECTOR] AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.flush();

    // Selector page - inject current AP config into template
    selectorServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        prefs.begin("unified-mode", false);
        prefs.putInt("mode", 0);
        prefs.end();
        // Build HTML with current AP values injected
        String html = FPSTR(SELECTOR_HTML);
        html.replace("%SSID%", apSSID);
        html.replace("%PASS%", apPassword);
        html.replace("%BUZZER%", buzzerEnabled ? "checked" : "");
        html.replace("%DISP_BR%", String(displayBrightness));
        html.replace("%LED_BR%", String(ledBrightness));
        request->send(200, "text/html", html);
    });

    // Mode selection endpoint - ONLY place that should trigger reboot
    selectorServer.on("/select", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("mode")) {
            int mode = request->getParam("mode")->value().toInt();
            if (mode >= 1 && mode <= 5) {
                Serial.printf("[OUI-SPY] USER SELECTED MODE %d - Storing and rebooting\n", mode);

                // Write mode to NVS
                prefs.begin("unified-mode", false);
                prefs.putInt("mode", mode);
                prefs.end();

                // Verify the write by reading it back
                prefs.begin("unified-mode", true);
                int verify = prefs.getInt("mode", -1);
                prefs.end();
                Serial.printf("[OUI-SPY] NVS VERIFY: wrote %d, read back %d - %s\n",
                    mode, verify, (verify == mode) ? "OK" : "MISMATCH!");
                Serial.flush();

                request->send(200, "text/plain", "OK");
                delay(1500);  // Extra time for NVS to settle
                Serial.printf("[OUI-SPY] REBOOTING INTO MODE %d NOW\n", mode);
                Serial.flush();
                ESP.restart();
                return;
            }
        }
        Serial.println("[OUI-SPY] Invalid mode selection rejected");
        request->send(400, "text/plain", "Invalid mode (1-5)");
    });

    // Save AP settings endpoint
    selectorServer.on("/saveap", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("ssid")) {
            String newSSID = request->getParam("ssid")->value();
            String newPass = request->hasParam("pass") ? request->getParam("pass")->value() : "";

            // Validate
            if (newSSID.length() < 1 || newSSID.length() > 32) {
                request->send(400, "text/plain", "SSID must be 1-32 chars");
                return;
            }
            if (newPass.length() > 0 && newPass.length() < 8) {
                request->send(400, "text/plain", "Password must be 8+ chars or empty");
                return;
            }

            Serial.printf("[OUI-SPY] Saving new AP config: SSID='%s'\n", newSSID.c_str());
            saveAPConfig(newSSID, newPass);

            request->send(200, "text/plain", "OK");
            delay(1000);
            ESP.restart();
            return;
        }
        request->send(400, "text/plain", "Missing SSID parameter");
    });

    // Buzzer toggle endpoint
    selectorServer.on("/buzzer", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("on")) {
            bool enabled = request->getParam("on")->value() == "1";
            saveBuzzerConfig(enabled);
            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing 'on' parameter");
        }
    });

    // Brightness endpoint — GET returns current values, with params sets them
    selectorServer.on("/brightness", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("disp")) {
            displayBrightness = (uint8_t)request->getParam("disp")->value().toInt();
            display_set_brightness(displayBrightness);
        }
        if (request->hasParam("led")) {
            ledBrightness = (uint8_t)request->getParam("led")->value().toInt();
            ledStrip_setBrightness(ledBrightness);
            ledStrip_show();
        }
        if (request->hasParam("disp") || request->hasParam("led")) {
            saveBrightnessConfig();
        }
        char json[64];
        snprintf(json, sizeof(json), "{\"disp\":%d,\"led\":%d}", displayBrightness, ledBrightness);
        request->send(200, "application/json", json);
    });

    // Reset to selector (callable from any mode's web interface)
    selectorServer.on("/menu", HTTP_GET, [](AsyncWebServerRequest *request) {
        prefs.begin("unified-mode", false);
        prefs.putInt("mode", 0);
        prefs.end();
        request->send(200, "text/plain", "Returning to menu...");
        delay(500);
        ESP.restart();
    });

#if HAS_SD_CARD
    setupSDWebEndpoints();
#endif

    Serial.println("[SELECTOR] Starting web server...");
    Serial.flush();
    selectorServer.begin();
    Serial.println("[SELECTOR] Web server started!");
    Serial.flush();

    // Visual indicator - breathe LED
    Serial.println("[SELECTOR] Setting up LED...");
    Serial.flush();
#if LED_PIN >= 0
    pinMode(LED_PIN, OUTPUT);
#endif

    Serial.println("[SELECTOR] Playing startup jingle...");
    Serial.flush();
    selectorBeep();

    // Update TFT display with selector menu
    display_selector(selectorIndex, apSSID.c_str(),
                     WiFi.softAPIP().toString().c_str());

    Serial.println("[SELECTOR] *** SELECTOR FULLY INITIALIZED ***");
    Serial.printf("[SELECTOR] WiFi AP: '%s'\n", apSSID.c_str());
    Serial.flush();
}

// ============================================================================
// Boot Button Watchdog Task (runs during setup, catches hangs)
// ============================================================================
static TaskHandle_t bootWatchdogHandle = NULL;

static void bootWatchdogTask(void* param) {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    unsigned long pressStart = 0;
    bool pressing = false;

    while (true) {
        if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
            if (!pressing) {
                pressing = true;
                pressStart = millis();
            } else if (millis() - pressStart >= BOOT_HOLD_TIME) {
                Serial.println("\n[OUI-SPY] BOOT held during setup -> RETURNING TO SELECTOR");
                Serial.flush();
                // Flash NeoPixel red to confirm
#if HAS_NEOPIXEL || HAS_APA102
                ledStrip_setColor(255, 0, 0);
                ledStrip_show();
#endif
                Preferences p;
                p.begin("unified-mode", false);
                p.putInt("mode", 0);
                p.end();
                delay(200);
                ESP.restart();
            }
        } else {
            pressing = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ============================================================================
// Arduino Entry Points
// ============================================================================
static unsigned long bootTime = 0;

void setup() {
    Serial.begin(115200);
    delay(200);  // Give serial time to initialize

    Serial.println("\n\n========================================");
    Serial.println("OUI SPY UNIFIED FIRMWARE v2.0");
    Serial.println("========================================");
    Serial.flush();

    // Initialize display early for boot splash
    display_init();

    // FIRST THING: Check if BOOT button (GPIO0) is being held
    // Hold BOOT for 1.5 seconds during startup to force selector menu.
    bool forceSelector = checkBootButton();

    // Initialize shared hardware
#if HAS_BUZZER && !defined(BOARD_CYD_S3)
    // CYD uses GPIO 8 for I2S DOUT, not digital buzzer — skip pinMode
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
#endif
#ifdef SPEAKER_EN_PIN
    pinMode(SPEAKER_EN_PIN, OUTPUT);
    digitalWrite(SPEAKER_EN_PIN, LOW);  // Enable speaker amplifier (active-LOW)
#endif

#ifdef BOARD_CYD_S3
    cyd_audio_init();
#endif

#if LED_PIN >= 0
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED off (inverted logic on XIAO)
#endif
#if HAS_APA102 || HAS_NEOPIXEL
    ledStrip_init();
#endif

    // Load and apply brightness settings from NVS
    loadBrightnessConfig();
    display_set_brightness(displayBrightness);
    ledStrip_setBrightness(ledBrightness);
#ifdef BOARD_CYD_S3
    cyd_set_volume(buzzerVolume);
#endif

    // Initialize SD card logging
    sdlog_init();

    // Initialize GPS UART (CYD only) — flockyou.cpp reads Serial0 directly
#if HAS_GPS
    Serial0.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.println("[OUI-SPY] GPS initialized on UART0");
#endif

    // SD status updated after mode selection (needs mode for per-mode file count)

    // Touch (CYD only): uses Freenove documented mapping, no calibration needed
#ifdef BOARD_CYD_S3
    Serial.println("[CYD] Touch: Freenove rotation-1 mapping (rawY -> scrX, 240-rawX -> scrY)");
#endif

    bootTime = millis();

    if (forceSelector) {
        Serial.println("[OUI-SPY] Boot button override -> SELECTOR MODE");
        Serial.flush();
        currentMode = 0;
    } else {
        // Read stored mode from NVS
        prefs.begin("unified-mode", true);
        currentMode = prefs.getInt("mode", 0);
        prefs.end();
        Serial.printf("[OUI-SPY] Stored mode from NVS: %d\n", currentMode);
        Serial.flush();

        // Validate mode range
        if (currentMode < 0 || currentMode > 5) {
            Serial.printf("[OUI-SPY] Invalid stored mode %d, defaulting to selector\n", currentMode);
            currentMode = 0;
        }

        if (currentMode != 0) {
            Serial.println("========================================");
            Serial.printf("[OUI-SPY] *** BOOTING INTO FIRMWARE MODE %d ***\n", currentMode);
            Serial.println("========================================");
            Serial.flush();
        }
    }

    // WiFi factory reset: skip for BLE modes (4,5) — they configure WiFi themselves
    // and the init/deinit cycle fragments heap, preventing BLE from allocating
    if (currentMode != 4 && currentMode != 5) {
        WiFi.mode(WIFI_AP_STA);
        delay(100);
        esp_wifi_restore();
        WiFi.softAPdisconnect(true);
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        delay(100);
        Serial.println("[OUI-SPY] WiFi factory-reset complete");
    } else {
        Serial.println("[OUI-SPY] Skipping WiFi factory-reset (BLE mode handles WiFi)");
    }
    Serial.flush();

    Serial.printf("[OUI-SPY] FINAL BOOT MODE: %d\n", currentMode);
    Serial.println("========================================");
    Serial.flush();

    // Start SD logging session for the selected mode
    if (currentMode != 0) {
        sdlog_start_session(currentMode);
    }

    // Update display with SD card status (after mode is known for file count)
#if HAS_SD_CARD
    if (sdlog_available()) {
        display_sd_status(true, sdlog_mode_file_count(currentMode),
                          sdlog_is_low(), sdlog_is_full());
    } else {
        display_sd_status(false, 0);
    }
#endif

    // Start boot button watchdog — monitors BOOT during mode setup.
    // If setup() hangs, user can hold BOOT 2s to force back to selector.
    if (currentMode != 0) {
        xTaskCreatePinnedToCore(bootWatchdogTask, "bootWd", 2048, NULL, 1,
                                &bootWatchdogHandle, 0);
    }

    // Route to selected mode
    Serial.println("\n[OUI-SPY] ========== ROUTING TO MODE ==========");
    Serial.printf("[OUI-SPY] About to switch on currentMode = %d\n", currentMode);
    Serial.flush();
    delay(100);

    if (currentMode == 0) {
        Serial.println("[OUI-SPY] >>> STARTING SELECTOR (mode 0) <<<");
        Serial.println("[OUI-SPY] AP will be configured from NVS");
        Serial.println("[OUI-SPY] Calling startSelector()...");
        Serial.flush();
        delay(100);
        display_boot_splash("SELECTOR");
        startSelector();
        Serial.println("[OUI-SPY] startSelector() returned");
        Serial.flush();
#ifndef BOARD_CYD_S3
    } else if (currentMode == 1) {
        Serial.println("[OUI-SPY] >>> STARTING DETECTOR (mode 1) <<<");
        Serial.println("[OUI-SPY] AP will be: snoopuntothem");
        Serial.flush();
        display_boot_splash("DETECTOR");
        detector_setup();
    } else if (currentMode == 2) {
        Serial.println("[OUI-SPY] >>> STARTING FOXHUNTER (mode 2) <<<");
        Serial.println("[OUI-SPY] AP will be: foxhunter");
        Serial.flush();
        display_boot_splash("FOXHUNTER");
        foxhunter_setup();
#endif
    } else if (currentMode == 4) {
        Serial.println("[OUI-SPY] >>> STARTING FLOCK-YOU (mode 4) <<<");
        Serial.println("[OUI-SPY] BLE + WiFi AP");
        Serial.flush();
        display_boot_splash("FLOCK-YOU");
        display_free_sprite();  // Free 150KB for BLE+WiFi AP init
        flockyou_setup();
    } else if (currentMode == 5) {
        Serial.println("[OUI-SPY] >>> STARTING SKY SPY (mode 5) <<<");
        Serial.println("[OUI-SPY] No WiFi AP (BLE only)");
        Serial.flush();
        display_boot_splash("SKY SPY");
        display_free_sprite();  // Free 150KB for BLE+WiFi init
        skyspy_setup();
    } else {
        Serial.printf("[OUI-SPY] ERROR: Unknown mode %d, defaulting to selector\n", currentMode);
        Serial.flush();
        display_boot_splash("SELECTOR");
        startSelector();
    }

    Serial.println("[OUI-SPY] ========== MODE STARTED ==========\n");
    Serial.flush();
}

// ============================================================================
// Boot Button -> Menu (runs every loop, works from ANY mode)
// ============================================================================
static unsigned long bootBtnStart = 0;
static bool bootBtnActive = false;

static void checkBootButtonLoop() {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        if (!bootBtnActive) {
            // Button just pressed - start timing
            bootBtnActive = true;
            bootBtnStart = millis();
        } else if (millis() - bootBtnStart >= BOOT_HOLD_TIME) {
            // Flush SD before reboot
            sdlog_flush();

            if (currentMode == 0) {
                // In selector mode: long press CONFIRMS the highlighted mode
                int selectedMode = SELECTOR_MODES[selectorIndex];
                Serial.printf("\n[OUI-SPY] *** LONG PRESS -> SELECTING %s (mode %d) ***\n",
                              SELECTOR_NAMES[selectorIndex], selectedMode);
                Serial.flush();

                // Show confirmation on TFT
                display_boot_splash(SELECTOR_NAMES[selectorIndex]);

#if HAS_BUZZER
                // Ascending confirmation beep
                playNote(1000, 80);
                delay(30);
                playNote(1500, 80);
                delay(30);
                playNote(2000, 120);
#elif HAS_APA102 || HAS_NEOPIXEL
                // Flash green to confirm
                for (int i = 0; i < 3; i++) {
                    ledStrip_setColor(0, 255, 0);
                    ledStrip_show();
                    delay(60);
                    ledStrip_clear();
                    ledStrip_show();
                    delay(40);
                }
#endif

                // Store selected mode and reboot
                prefs.begin("unified-mode", false);
                prefs.putInt("mode", selectedMode);
                prefs.end();
                delay(300);
                ESP.restart();
            } else {
                // In any other mode: long press returns to selector menu
                Serial.println("\n[OUI-SPY] *** BOOT BUTTON HELD -> RETURNING TO MENU ***");
                Serial.flush();
#if HAS_SD_CARD
                if (currentMode == 5) skyspy_session_end();
#endif
#if HAS_BUZZER
#ifdef BOARD_CYD_S3
                for (int i = 0; i < 3; i++) {
                    cyd_tone(BUZZER_PIN, 3000, 80);
                    delay(80);
                    cyd_noTone(BUZZER_PIN);
                    delay(60);
                }
#else
                for (int i = 0; i < 3; i++) {
                    ledcSetup(BUZZER_LEDC_CH, 3000, 8);
                    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
                    ledcWrite(BUZZER_LEDC_CH, 100);
                    delay(80);
                    ledcWrite(BUZZER_LEDC_CH, 0);
                    delay(60);
                }
#endif
#endif
                // Clear mode and reboot
                prefs.begin("unified-mode", false);
                prefs.putInt("mode", 0);
                prefs.end();
                delay(200);
                ESP.restart();
            }
        }
    } else {
        // Button released
        if (bootBtnActive) {
            unsigned long held = millis() - bootBtnStart;
            // Short press in selector mode: cycle menu selection
            if (held < SHORT_PRESS_MS && currentMode == 0) {
                selectorIndex = (selectorIndex + 1) % NUM_SELECTOR_MODES;
                display_selector(selectorIndex, apSSID.c_str(),
                                 WiFi.softAPIP().toString().c_str());
                Serial.printf("[SELECTOR] TFT menu -> %s\n",
                              SELECTOR_NAMES[selectorIndex]);
            }
        }
        bootBtnActive = false;
    }
}

void loop() {
    // Kill boot watchdog task once loop() is running (no longer needed)
    if (bootWatchdogHandle) {
        vTaskDelete(bootWatchdogHandle);
        bootWatchdogHandle = NULL;
    }

    // ALWAYS check boot button - hold 2s from ANY mode to return to menu
    checkBootButtonLoop();

    // Route to active mode's loop
    switch (currentMode) {
#ifndef BOARD_CYD_S3
        case 1: detector_loop(); break;
        case 2: foxhunter_loop(); break;
#endif
        case 4: flockyou_loop(); break;
        case 5: skyspy_loop(); break;
        default:
            // Selector mode - web server handles everything
#ifdef BOARD_CYD_S3
            // Touch-based mode selection + settings page
            {
                static bool inSettings = false;
                static bool settingsIncognito = false;
                static uint8_t savedDispBr = 0, savedLedBr = 0;

                if (inSettings) {
                    int action = display_settings_touch();
                    switch (action) {
                        case 1: // disp+
                            displayBrightness = (displayBrightness <= 230) ? displayBrightness + 25 : 255;
                            display_set_brightness(displayBrightness);
                            saveBrightnessConfig();
                            display_settings(displayBrightness, ledBrightness, buzzerVolume, buzzerEnabled, settingsIncognito);
                            break;
                        case 2: // disp-
                            displayBrightness = (displayBrightness >= 35) ? displayBrightness - 25 : 10;
                            display_set_brightness(displayBrightness);
                            saveBrightnessConfig();
                            display_settings(displayBrightness, ledBrightness, buzzerVolume, buzzerEnabled, settingsIncognito);
                            break;
                        case 3: // led+
                            ledBrightness = (ledBrightness <= 230) ? ledBrightness + 25 : 255;
                            ledStrip_setBrightness(ledBrightness);
                            ledStrip_show();
                            saveBrightnessConfig();
                            display_settings(displayBrightness, ledBrightness, buzzerVolume, buzzerEnabled, settingsIncognito);
                            break;
                        case 4: // led-
                            ledBrightness = (ledBrightness >= 25) ? ledBrightness - 25 : 0;
                            ledStrip_setBrightness(ledBrightness);
                            ledStrip_show();
                            saveBrightnessConfig();
                            display_settings(displayBrightness, ledBrightness, buzzerVolume, buzzerEnabled, settingsIncognito);
                            break;
                        case 5: // vol+
                            buzzerVolume = (buzzerVolume <= 230) ? buzzerVolume + 25 : 255;
                            saveBrightnessConfig();
                            display_settings(displayBrightness, ledBrightness, buzzerVolume, buzzerEnabled, settingsIncognito);
                            break;
                        case 6: // vol-
                            buzzerVolume = (buzzerVolume >= 25) ? buzzerVolume - 25 : 0;
                            saveBrightnessConfig();
                            display_settings(displayBrightness, ledBrightness, buzzerVolume, buzzerEnabled, settingsIncognito);
                            break;
                        case 7: // buzzer toggle
                            saveBuzzerConfig(!buzzerEnabled);
                            display_settings(displayBrightness, ledBrightness, buzzerVolume, buzzerEnabled, settingsIncognito);
                            break;
                        case 8: // incognito toggle
                            settingsIncognito = !settingsIncognito;
                            if (settingsIncognito) {
                                savedDispBr = displayBrightness;
                                savedLedBr = ledBrightness;
                                display_set_brightness(0);
                                ledStrip_setBrightness(0);
                                ledStrip_show();
                            } else {
                                displayBrightness = savedDispBr;
                                ledBrightness = savedLedBr;
                                display_set_brightness(displayBrightness);
                                ledStrip_setBrightness(ledBrightness);
                                ledStrip_show();
                            }
                            display_settings(displayBrightness, ledBrightness, buzzerVolume, buzzerEnabled, settingsIncognito);
                            break;
                        case 9: { // back
                            inSettings = false;
                            settingsIncognito = false;
                            // Wait for finger release before showing selector
                            // (prevents residual touch from hitting SETTINGS button)
                            int16_t _tx, _ty;
                            while (display_touch_read(_tx, _ty)) { delay(20); }
                            delay(100);
                            display_selector(selectorIndex, apSSID.c_str(),
                                             WiFi.softAPIP().toString().c_str());
                            break;
                        }
                    }
                } else {
                    int touchedMode = display_selector_touch();
                    if (touchedMode == 99) {
                        // Enter settings — wait for finger release first
                        // (prevents residual touch from hitting BACK/INCOGNITO)
                        int16_t _tx, _ty;
                        while (display_touch_read(_tx, _ty)) { delay(20); }
                        delay(100);
                        inSettings = true;
                        settingsIncognito = false;
                        display_settings(displayBrightness, ledBrightness, buzzerVolume, buzzerEnabled, false);
                    } else if (touchedMode > 0) {
                        display_boot_splash(touchedMode == 4 ? "FLOCK-YOU" : "SKY SPY");
                        // Store and reboot into selected mode
                        prefs.begin("unified-mode", false);
                        prefs.putInt("mode", touchedMode);
                        prefs.end();
                        delay(300);
                        ESP.restart();
                    }
                }
            }
#endif
            // LED breathing animation
            {
                static unsigned long lastLed = 0;
                static bool ledState = false;
                if (millis() - lastLed > 1000) {
                    ledState = !ledState;
#if LED_PIN >= 0
                    digitalWrite(LED_PIN, ledState ? LOW : HIGH);
#elif HAS_APA102 || HAS_NEOPIXEL
                    if (ledState) {
                        ledStrip_setColor(0, 40, 0);
                    } else {
                        ledStrip_clear();
                    }
                    ledStrip_show();
#endif
                    lastLed = millis();
                }
            }
            delay(10);
            break;
    }
}
