/*
 * TFT Display + Touch Implementation — Freenove ESP32-S3 2.8" IPS CYD
 *
 * ILI9341 (320x240) driven by TFT_eSPI. FT6336U capacitive touch via
 * raw I2C (no library). Green-on-black terminal aesthetic.
 *
 * Layout (landscape 320x240):
 *   HEADER  (40px)  — "OUI-SPY" + mode name
 *   CONTENT (170px) — mode-specific data
 *   FOOTER  (30px)  — status bar, SD info, GPS status
 */

#if defined(ENABLE_TFT_DISPLAY) && defined(BOARD_CYD_S3)

#include "display.h"
#include "board_config.h"
#include "sdlog.h"
#if HAS_SD_CARD
#include <FS.h>
#include <SD_MMC.h>
#endif
#include <TFT_eSPI.h>
#include <Wire.h>
#include "qrcode.h"

// ---------- globals --------------------------------------------------------
static TFT_eSPI    tft = TFT_eSPI();
static TFT_eSprite spr = TFT_eSprite(&tft);
static bool        spriteOk = false;  // false when sprite couldn't be allocated

// Colours
#define C_BG     TFT_BLACK
#define C_FG     TFT_GREEN
#define C_DIM    tft.color565(0, 100, 0)
#define C_HEADER tft.color565(0, 180, 0)
#define C_WARN   TFT_YELLOW
#define C_ALERT  TFT_RED
#define C_WHITE  TFT_WHITE

// Layout constants (landscape: 320 wide x 240 tall)
#define SCR_W 320
#define SCR_H 240
#define HDR_H 40
#define FTR_H 30
#define CNT_Y HDR_H
#define CNT_H (SCR_H - HDR_H - FTR_H)
#define FTR_Y (SCR_H - FTR_H)

// Screen state for dirty-region drawing (no-sprite mode)
enum ScreenType { SCR_NONE, SCR_SELECTOR, SCR_FLOCKYOU,
                  SCR_SKYSPY_SCAN, SCR_SKYSPY_DRONE, SCR_SETTINGS };
static ScreenType currentScreen = SCR_NONE;

// Previous QR coordinates — skip redraw if unchanged
static double prevQrLat = 0.0, prevQrLon = 0.0;
static double prevQrPilotLat = 0.0, prevQrPilotLon = 0.0;

// SD status cache
static bool sd_ok = false;
static int  sd_files = 0;

// Touch state
static bool touch_ready = false;

// Sky Spy settings button (bottom-left of content, above footer)
#define SS_BTN_Y       (FTR_Y - 38)
#define SS_BTN_H       34
#define SS_BTN_W       152
#define SS_SET_BTN_X   4
static unsigned long ssLastTouch = 0;
#define SS_TOUCH_DEBOUNCE_MS 300

// Backlight LEDC channel (channel 2 to avoid conflict with buzzer on ch1)
#define BL_LEDC_CH     2

// Selector button regions (two mode buttons + settings bar)
#define SEL_BTN_Y      44
#define SEL_BTN_H      104
#define SEL_BTN_W      150
#define SEL_BTN_GAP    12
#define SEL_BTN1_X     4
#define SEL_BTN2_X     (SEL_BTN1_X + SEL_BTN_W + SEL_BTN_GAP)
#define SEL_SET_Y      (SEL_BTN_Y + SEL_BTN_H + 10)
#define SEL_SET_H      36
#define SEL_SET_X      4
#define SEL_SET_W      312

// Settings touch debounce
static unsigned long settings_last_touch = 0;
#define SETTINGS_DEBOUNCE_MS 300

// ---------- helpers --------------------------------------------------------

// Drawing target: sprite when available, direct TFT when not (BLE modes)
static TFT_eSPI& gfx() { return spriteOk ? (TFT_eSPI&)spr : tft; }

static void clearScreen() {
    if (spriteOk) spr.fillSprite(C_BG);
    else          tft.fillScreen(C_BG);
}

static void drawHeader(const char* title) {
    auto& g = gfx();
    g.setTextColor(C_HEADER, C_BG);
    g.setTextDatum(TC_DATUM);
    g.drawString("OUI-SPY", SCR_W / 2, 2, 2);
    g.setTextColor(C_FG, C_BG);
    g.drawString(title, SCR_W / 2, 20, 2);
    g.drawFastHLine(0, HDR_H - 1, SCR_W, C_DIM);
}

// initScreen: only does a full clear when screen type changes.
// In sprite mode, always clears the sprite buffer (flicker-free push).
// If customHeader is true, the caller draws its own header (skip default).
// Returns true if the screen type changed (layout transition).
static bool initScreen(ScreenType type, const char* title, bool customHeader = false) {
    bool changed = (currentScreen != type);
    if (changed) {
        currentScreen = type;
        if (spriteOk) {
            spr.fillSprite(C_BG);
        } else {
            tft.fillScreen(C_BG);
        }
        if (!customHeader) {
            // Draw static header (only on layout change)
            auto& g = gfx();
            g.setTextColor(C_HEADER, C_BG);
            g.setTextDatum(TC_DATUM);
            g.drawString("OUI-SPY", SCR_W / 2, 2, 2);
            g.setTextColor(C_FG, C_BG);
            g.drawString(title, SCR_W / 2, 20, 2);
            g.drawFastHLine(0, HDR_H - 1, SCR_W, C_DIM);
        }
        // Reset QR tracking on screen change
        prevQrLat = prevQrLon = 0.0;
        prevQrPilotLat = prevQrPilotLon = 0.0;
    } else if (spriteOk) {
        spr.fillSprite(C_BG);  // Sprite mode: always clear (no flicker)
        if (!customHeader) {
            // Redraw header in sprite mode (buffer was just cleared)
            auto& g = gfx();
            g.setTextColor(C_HEADER, C_BG);
            g.setTextDatum(TC_DATUM);
            g.drawString("OUI-SPY", SCR_W / 2, 2, 2);
            g.setTextColor(C_FG, C_BG);
            g.drawString(title, SCR_W / 2, 20, 2);
            g.drawFastHLine(0, HDR_H - 1, SCR_W, C_DIM);
        }
    }
    return changed;
}

// clearRow: erase a horizontal strip before redrawing text (no-sprite mode only)
static void clearRow(int y, int h, int x = 0, int w = SCR_W) {
    if (!spriteOk) tft.fillRect(x, y, w, h, C_BG);
}

// Forward declarations
static void drawSsToggleButtons(bool ledOn, bool toneOn);

static void drawFooter(const char* text) {
    auto& g = gfx();
    if (!spriteOk) tft.fillRect(0, FTR_Y, SCR_W, FTR_H, C_BG);
    g.drawFastHLine(0, FTR_Y, SCR_W, C_DIM);
    g.setTextColor(C_DIM, C_BG);
    g.setTextDatum(TL_DATUM);
    g.drawString(text, 4, FTR_Y + 8, 2);

    // SD indicator on right side
    char sdBuf[24];
    if (sd_ok) {
        snprintf(sdBuf, sizeof(sdBuf), "SD:OK / Files:%d", sd_files);
    } else {
        snprintf(sdBuf, sizeof(sdBuf), "SD:---");
    }
    g.setTextDatum(TR_DATUM);
    g.drawString(sdBuf, SCR_W - 4, FTR_Y + 8, 2);
}

static void pushSprite() {
    if (spriteOk) spr.pushSprite(0, 0);
    // When !spriteOk, drawing went directly to TFT — nothing to push
}

// ---------- touch I2C ------------------------------------------------------

void display_touch_init() {
    Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
    Wire.setClock(400000);

    // Reset touch controller
    pinMode(TOUCH_RST_PIN, OUTPUT);
    digitalWrite(TOUCH_RST_PIN, LOW);
    delay(10);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(300);

    // Verify presence
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    touch_ready = (Wire.endTransmission() == 0);

    if (touch_ready) {
        Serial.println("[CYD] FT6336U touch controller found");
    } else {
        Serial.println("[CYD] FT6336U touch controller NOT found");
    }
}

bool display_touch_read(int16_t &x, int16_t &y) {
    if (!touch_ready) return false;

    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x02);  // Number of touches register
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)5);

    if (Wire.available() < 5) return false;

    uint8_t touches = Wire.read();
    if ((touches & 0x0F) == 0) return false;

    uint8_t xH = Wire.read();
    uint8_t xL = Wire.read();
    uint8_t yH = Wire.read();
    uint8_t yL = Wire.read();

    int16_t rawX = ((xH & 0x0F) << 8) | xL;
    int16_t rawY = ((yH & 0x0F) << 8) | yL;

    // Freenove CYD rotation-1 mapping (from Sketch_12.1):
    // Sensor Y → Screen X, (240 - Sensor X) → Screen Y
    x = constrain(rawY, 0, 319);
    y = constrain((int16_t)(240 - rawX), 0, 239);

    return true;
}

int display_selector_touch() {
    int16_t tx, ty;
    if (!display_touch_read(tx, ty)) return -1;

    // Mode buttons (top row)
    if (ty >= SEL_BTN_Y - 6 && ty <= SEL_BTN_Y + SEL_BTN_H + 6) {
        int mid = SEL_BTN1_X + SEL_BTN_W + SEL_BTN_GAP / 2;
        if (tx < mid) return 4;   // FLOCK-YOU
        return 5;                  // SKY SPY
    }

    // Settings bar (below mode buttons)
    if (ty >= SEL_SET_Y - 6 && ty <= SEL_SET_Y + SEL_SET_H + 6) {
        return 99;
    }

    return -1;
}

// ---------- sprite lifecycle (free before BLE init, recreate after) ----------

void display_free_sprite() {
    spr.deleteSprite();
    spriteOk = false;
    Serial.printf("[CYD] Sprite freed, heap: %u bytes\n", ESP.getFreeHeap());
}

void display_create_sprite() {
    spriteOk = (spr.createSprite(SCR_W, SCR_H) != nullptr);
    if (spriteOk) spr.setTextColor(C_FG, C_BG);
    Serial.printf("[CYD] Sprite %s, heap: %u bytes\n",
                  spriteOk ? "created" : "FAILED (using direct TFT)", ESP.getFreeHeap());
}

// ---------- touch calibration (no-op: using Freenove documented mapping) -----

bool display_touch_calibration_load() { return true; }  // mapping is hardcoded
void display_touch_calibrate() {}                        // not needed for capacitive

// ---------- public API -----------------------------------------------------

void display_init() {
    tft.init();
    tft.setRotation(1);  // landscape: 320x240

    // PWM backlight (active HIGH on CYD) — channel 2 (ch0 reserved, ch1=buzzer)
    ledcSetup(BL_LEDC_CH, 5000, 8);
    ledcAttachPin(TFT_BL_PIN, BL_LEDC_CH);
    ledcWrite(BL_LEDC_CH, 200);  // ~78% brightness (active high: direct duty)

    tft.fillScreen(C_BG);

    // Create sprite (full-screen buffer) — may fail if insufficient heap
    spriteOk = (spr.createSprite(SCR_W, SCR_H) != nullptr);
    if (spriteOk) spr.setTextColor(C_FG, C_BG);
    Serial.printf("[CYD] Sprite %s at init, heap: %u\n", spriteOk ? "OK" : "FAILED", ESP.getFreeHeap());

    // Initialize touch
    display_touch_init();
}

void display_boot_splash(const char* modeName) {
    currentScreen = SCR_NONE;  // Ensure next real screen does full clear
    clearScreen();

    gfx().setTextColor(C_HEADER, C_BG);
    gfx().setTextDatum(TC_DATUM);
    gfx().drawString("OUI-SPY", SCR_W / 2, 40, 4);

    gfx().setTextColor(C_FG, C_BG);
    gfx().drawString(modeName, SCR_W / 2, 100, 4);

    gfx().setTextColor(C_DIM, C_BG);
    gfx().drawString("Initializing...", SCR_W / 2, 160, 2);

    pushSprite();
}

void display_selector(int index, const char* ssid, const char* ip) {
    initScreen(SCR_SELECTOR, "SELECTOR", true);

    auto& g = gfx();

    // Header
    g.setTextColor(C_HEADER, C_BG);
    g.setTextDatum(TC_DATUM);
    g.drawString("OUI-SPY", SCR_W / 2, 4, 4);
    g.drawFastHLine(0, 35, SCR_W, C_DIM);

    // Button 1: FLOCK-YOU (large, left)
    g.drawRoundRect(SEL_BTN1_X, SEL_BTN_Y, SEL_BTN_W, SEL_BTN_H, 6, C_FG);
    g.setTextColor(C_FG, C_BG);
    g.setTextDatum(MC_DATUM);
    g.drawString("FLOCK", SEL_BTN1_X + SEL_BTN_W / 2, SEL_BTN_Y + SEL_BTN_H / 2 - 15, 4);
    g.drawString("YOU", SEL_BTN1_X + SEL_BTN_W / 2, SEL_BTN_Y + SEL_BTN_H / 2 + 18, 4);

    // Button 2: SKY SPY (large, right)
    g.drawRoundRect(SEL_BTN2_X, SEL_BTN_Y, SEL_BTN_W, SEL_BTN_H, 6, C_FG);
    g.setTextColor(C_FG, C_BG);
    g.setTextDatum(MC_DATUM);
    g.drawString("SKY", SEL_BTN2_X + SEL_BTN_W / 2, SEL_BTN_Y + SEL_BTN_H / 2 - 15, 4);
    g.drawString("SPY", SEL_BTN2_X + SEL_BTN_W / 2, SEL_BTN_Y + SEL_BTN_H / 2 + 18, 4);

    // Settings bar (full-width, below mode buttons)
    g.drawRoundRect(SEL_SET_X, SEL_SET_Y, SEL_SET_W, SEL_SET_H, 6, C_DIM);
    g.setTextColor(C_DIM, C_BG);
    g.setTextDatum(MC_DATUM);
    g.drawString("SETTINGS", SEL_SET_X + SEL_SET_W / 2, SEL_SET_Y + SEL_SET_H / 2, 2);

    // Footer with IP and SD
    g.drawFastHLine(0, FTR_Y, SCR_W, C_DIM);
    g.setTextColor(C_DIM, C_BG);
    g.setTextDatum(TL_DATUM);
    char foot[48];
    snprintf(foot, sizeof(foot), "IP:%s  SSID:%s", ip, ssid);
    g.drawString(foot, 4, FTR_Y + 8, 2);

    pushSprite();
}

void display_detector_event(const char* mac, int rssi, const char* alias,
                            const char* type, int count) {
    clearScreen();
    drawHeader("DETECTOR");

    gfx().setTextColor(C_ALERT, C_BG);
    gfx().setTextDatum(TL_DATUM);
    gfx().drawString(type, 8, CNT_Y + 8, 2);

    gfx().setTextColor(C_FG, C_BG);
    gfx().drawString(mac, 8, CNT_Y + 35, 2);

    char rssiBuf[16];
    snprintf(rssiBuf, sizeof(rssiBuf), "RSSI: %d", rssi);
    gfx().setTextDatum(TR_DATUM);
    gfx().drawString(rssiBuf, SCR_W - 8, CNT_Y + 35, 2);

    gfx().setTextDatum(TL_DATUM);
    gfx().setTextColor(C_WARN, C_BG);
    if (alias && alias[0]) {
        gfx().drawString(alias, 8, CNT_Y + 65, 2);
    }

    char cntBuf[24];
    snprintf(cntBuf, sizeof(cntBuf), "Total: %d", count);
    gfx().setTextColor(C_DIM, C_BG);
    gfx().drawString(cntBuf, 8, CNT_Y + 95, 2);

    drawFooter("Scanning...");
    pushSprite();
}

void display_detector_scanning(int filters, int devices) {
    clearScreen();
    drawHeader("DETECTOR");

    gfx().setTextColor(C_FG, C_BG);
    gfx().setTextDatum(TC_DATUM);
    gfx().drawString("Scanning...", SCR_W / 2, CNT_Y + 30, 4);

    char buf[32];
    snprintf(buf, sizeof(buf), "Filters: %d   Found: %d", filters, devices);
    gfx().setTextColor(C_DIM, C_BG);
    gfx().drawString(buf, SCR_W / 2, CNT_Y + 80, 2);

    drawFooter("BLE Active");
    pushSprite();
}

void display_foxhunter(int rssi, const char* mac, bool detected,
                       const char* status) {
    clearScreen();
    drawHeader("FOXHUNTER");

    if (detected) {
        char rssiBuf[8];
        snprintf(rssiBuf, sizeof(rssiBuf), "%d", rssi);
        gfx().setTextColor(C_FG, C_BG);
        gfx().setTextDatum(TC_DATUM);
        gfx().drawString(rssiBuf, SCR_W / 2, CNT_Y + 10, 4);

        // Proximity bar
        int barW = map(constrain(rssi, -95, -25), -95, -25, 4, SCR_W - 16);
        uint16_t barColor;
        if (rssi >= -45)      barColor = TFT_GREEN;
        else if (rssi >= -65) barColor = TFT_YELLOW;
        else                  barColor = TFT_RED;
        gfx().fillRect(8, CNT_Y + 70, barW, 16, barColor);
        gfx().drawRect(8, CNT_Y + 70, SCR_W - 16, 16, C_DIM);

        gfx().setTextColor(C_DIM, C_BG);
        gfx().setTextDatum(TC_DATUM);
        gfx().drawString(mac, SCR_W / 2, CNT_Y + 100, 2);
    } else {
        gfx().setTextColor(C_WARN, C_BG);
        gfx().setTextDatum(TC_DATUM);
        gfx().drawString("Searching...", SCR_W / 2, CNT_Y + 50, 4);
    }

    drawFooter(status);
    pushSprite();
}

void display_flockyou(int count, int inRange, bool buzzer, const char* mac,
                      const char* method, int rssi, int gpsSats,
                      const char* name, int sightings,
                      bool isRaven, const char* ravenFW,
                      bool gpsTagged) {
    initScreen(SCR_FLOCKYOU, "FLOCK-YOU");

    auto& g = gfx();
    g.setTextDatum(TL_DATUM);

    // Row 1: Detected / In Range
    clearRow(CNT_Y + 10, 18);
    char line1[40];
    snprintf(line1, sizeof(line1), "Detected: %d   In Range: %d", count, inRange);
    g.setTextColor(C_FG, C_BG);
    g.drawString(line1, 8, CNT_Y + 10, 2);

    // Row 2: MAC + sighting count
    clearRow(CNT_Y + 32, 18);
    if (mac && mac[0]) {
        g.setTextColor(C_WARN, C_BG);
        g.drawString(mac, 8, CNT_Y + 32, 2);
        if (sightings > 0) {
            char cntBuf[8];
            if (sightings == 1) {
                snprintf(cntBuf, sizeof(cntBuf), "NEW");
            } else {
                snprintf(cntBuf, sizeof(cntBuf), "x%d", sightings);
            }
            g.setTextColor(sightings == 1 ? C_WARN : C_DIM, C_BG);
            g.setTextDatum(TR_DATUM);
            g.drawString(cntBuf, SCR_W - 8, CNT_Y + 32, 2);
            g.setTextDatum(TL_DATUM);
        }
    }

    // Row 3: Device name (truncated)
    clearRow(CNT_Y + 52, 18);
    if (name && name[0]) {
        char nameBuf[31];
        snprintf(nameBuf, sizeof(nameBuf), "%s", name);
        g.setTextColor(C_DIM, C_BG);
        g.drawString(nameBuf, 8, CNT_Y + 52, 2);
    } else {
        g.setTextColor(C_DIM, C_BG);
        g.drawString("Name: ---", 8, CNT_Y + 52, 2);
    }

    // Row 4: Method + RSSI
    clearRow(CNT_Y + 72, 18);
    if (mac && mac[0]) {
        char line3[40];
        snprintf(line3, sizeof(line3), "[%s]  %d dBm", method ? method : "?", rssi);
        g.setTextColor(C_DIM, C_BG);
        g.drawString(line3, 8, CNT_Y + 72, 2);
    }

    // Row 5: Raven label + GPS tag indicator
    clearRow(CNT_Y + 92, 18);
    if (isRaven) {
        char rvBuf[24];
        snprintf(rvBuf, sizeof(rvBuf), "RAVEN v%s", ravenFW ? ravenFW : "?");
        g.setTextColor(C_ALERT, C_BG);
        g.drawString(rvBuf, 8, CNT_Y + 92, 2);
    } else {
        g.setTextColor(C_DIM, C_BG);
        g.drawString("Raven: ---", 8, CNT_Y + 92, 2);
    }
    if (gpsTagged) {
        g.setTextColor(C_FG, C_BG);
        g.setTextDatum(TR_DATUM);
        g.drawString("GPS", SCR_W - 18, CNT_Y + 92, 2);
        g.fillCircle(SCR_W - 10, CNT_Y + 99, 4, C_FG);
        g.setTextDatum(TL_DATUM);
    } else {
        g.setTextColor(C_DIM, C_BG);
        g.setTextDatum(TR_DATUM);
        g.drawString("GPS: ---", SCR_W - 8, CNT_Y + 92, 2);
        g.setTextDatum(TL_DATUM);
    }

    // AP connection info (just above footer separator)
    clearRow(FTR_Y - 18, 18);
    g.setTextColor(C_DIM, C_BG);
    g.drawString("flockyou/flockyou123 192.168.4.1", 8, FTR_Y - 18, 2);

    char foot[40];
    if (gpsSats > 0)
        snprintf(foot, sizeof(foot), "GPS:%d sats", gpsSats);
    else
        snprintf(foot, sizeof(foot), "GPS:---");
    drawFooter(foot);
    pushSprite();
}

static void drawSsSettingsButton();  // forward declaration

void display_skyspy(const char* mac, const char* id, double lat, double lon,
                    int alt, int rssi, int total,
                    double pilotLat, double pilotLon,
                    int gpsSats, int wifiCh, int sessionsSeen) {
    bool layoutChanged = initScreen(SCR_SKYSPY_DRONE, "SKY SPY");

    auto& g = gfx();

    // Left side: drone info (0-155)
    // Header row: "DRONE DETECTED" + NEW/SEEN badge — always dynamic so badge stays current
    clearRow(CNT_Y + 4, 18, 0, 155);
    g.setTextColor(C_ALERT, C_BG);
    g.setTextDatum(TL_DATUM);
    g.drawString("DRONE DETECTED", 8, CNT_Y + 4, 2);
    if (id && id[0] && sessionsSeen > 0) {
        char badge[8];
        if (sessionsSeen == 1) {
            snprintf(badge, sizeof(badge), "NEW");
        } else {
            snprintf(badge, sizeof(badge), "x%d", sessionsSeen);
        }
        g.setTextDatum(TR_DATUM);
        g.setTextColor(sessionsSeen == 1 ? C_WARN : C_DIM, C_BG);
        g.drawString(badge, 152, CNT_Y + 4, 2);
        g.setTextDatum(TL_DATUM);
    }

    // ID (dynamic)
    clearRow(CNT_Y + 26, 18, 0, 155);
    g.setTextColor(C_FG, C_BG);
    g.setTextDatum(TL_DATUM);
    if (id && id[0]) {
        char idBuf[24];
        snprintf(idBuf, sizeof(idBuf), "ID:%.20s", id);
        g.drawString(idBuf, 8, CNT_Y + 26, 2);
    }

    // MAC (dynamic)
    clearRow(CNT_Y + 48, 18, 0, 155);
    if (mac && mac[0]) {
        g.drawString(mac, 8, CNT_Y + 48, 2);
    }

    // Alt + RSSI (dynamic)
    clearRow(CNT_Y + 70, 18, 0, 155);
    char posBuf[32];
    snprintf(posBuf, sizeof(posBuf), "Alt:%dm RSSI:%d", alt, rssi);
    g.setTextColor(C_DIM, C_BG);
    g.drawString(posBuf, 8, CNT_Y + 70, 2);

    // Drone coords (dynamic)
    clearRow(CNT_Y + 92, 18, 0, 155);
    if (lat != 0.0 || lon != 0.0) {
        char gpsBuf[32];
        snprintf(gpsBuf, sizeof(gpsBuf), "%.5f,%.5f", lat, lon);
        g.drawString(gpsBuf, 8, CNT_Y + 92, 2);
    }

    // Pilot coords (dynamic)
    clearRow(CNT_Y + 114, 40, 0, 155);
    if (pilotLat != 0.0 || pilotLon != 0.0) {
        g.setTextColor(C_WARN, C_BG);
        g.drawString("PILOT:", 8, CNT_Y + 114, 2);
        char pilotBuf[32];
        snprintf(pilotBuf, sizeof(pilotBuf), "%.5f,%.5f", pilotLat, pilotLon);
        g.setTextColor(C_DIM, C_BG);
        g.drawString(pilotBuf, 8, CNT_Y + 134, 2);
    }

    // Right side: QR code (160-320) when we have drone coords
    if (lat != 0.0 || lon != 0.0) {
        // Only redraw QR if coords changed significantly
        bool qrChanged = layoutChanged ||
            fabs(lat - prevQrLat) > 0.0001 ||
            fabs(lon - prevQrLon) > 0.0001 ||
            fabs(pilotLat - prevQrPilotLat) > 0.0001 ||
            fabs(pilotLon - prevQrPilotLon) > 0.0001;

        if (qrChanged) {
            prevQrLat = lat;
            prevQrLon = lon;
            prevQrPilotLat = pilotLat;
            prevQrPilotLon = pilotLon;

            // Clear right-half QR region
            clearRow(CNT_Y, CNT_H, 160, 160);

            char url[128];
            if (pilotLat != 0.0 || pilotLon != 0.0) {
                snprintf(url, sizeof(url),
                    "https://maps.apple.com/?saddr=%.6f,%.6f&daddr=%.6f,%.6f",
                    pilotLat, pilotLon, lat, lon);
            } else {
                snprintf(url, sizeof(url),
                    "https://maps.apple.com/?ll=%.6f,%.6f&q=Drone",
                    lat, lon);
            }

            QRCode qr;
            uint8_t qrData[qrcode_getBufferSize(5)];
            qrcode_initText(&qr, qrData, 5, ECC_LOW, url);

            int modSize = 3;
            int qrPixels = qr.size * modSize;
            int qrX = 160 + (160 - qrPixels) / 2;
            int qrY = CNT_Y + (CNT_H - qrPixels - 20) / 2;

            // White background with quiet zone
            g.fillRect(qrX - 4, qrY - 4, qrPixels + 8, qrPixels + 8, TFT_WHITE);
            // Draw modules
            for (uint8_t y = 0; y < qr.size; y++) {
                for (uint8_t x = 0; x < qr.size; x++) {
                    if (qrcode_getModule(&qr, x, y)) {
                        g.fillRect(qrX + x * modSize, qrY + y * modSize,
                                     modSize, modSize, TFT_BLACK);
                    }
                }
            }
            // Label below QR
            g.setTextColor(C_DIM, C_BG);
            g.setTextDatum(TC_DATUM);
            g.drawString("Scan for Maps", 240, qrY + qrPixels + 6, 1);
        }
    }

    drawSsSettingsButton();

    char foot[40];
    snprintf(foot, sizeof(foot), "Total:%d BLE:ON WiFi CH:%d", total, wifiCh);
    drawFooter(foot);
    pushSprite();
}

void display_skyspy_scanning(int count, double devLat, double devLon,
                             int gpsSats, int wifiCh) {
    bool layoutChanged = initScreen(SCR_SKYSPY_SCAN, "SKY SPY");

    auto& g = gfx();

    // Left half: scanning info
    // "Scanning..." is static — only draw on layout change
    if (layoutChanged) {
        g.setTextColor(C_FG, C_BG);
        g.setTextDatum(TL_DATUM);
        g.drawString("Scanning...", 8, CNT_Y + 10, 4);
    }

    // Drone count (dynamic)
    clearRow(CNT_Y + 50, 18, 0, 160);
    char buf[24];
    snprintf(buf, sizeof(buf), "Drones: %d", count);
    g.setTextColor(C_DIM, C_BG);
    g.setTextDatum(TL_DATUM);
    g.drawString(buf, 8, CNT_Y + 50, 2);

    // GPS sats (dynamic)
    clearRow(CNT_Y + 75, 18, 0, 160);
    if (gpsSats > 0) {
        char gpsBuf[24];
        snprintf(gpsBuf, sizeof(gpsBuf), "GPS: %d sats", gpsSats);
        g.setTextColor(C_DIM, C_BG);
        g.drawString(gpsBuf, 8, CNT_Y + 75, 2);
    }

    // Coords + QR (dynamic)
    clearRow(CNT_Y + 95, 18, 0, 160);
    if (devLat != 0.0 || devLon != 0.0) {
        // Show device coords text
        char coordBuf[32];
        snprintf(coordBuf, sizeof(coordBuf), "%.5f,%.5f", devLat, devLon);
        g.setTextColor(C_DIM, C_BG);
        g.drawString(coordBuf, 8, CNT_Y + 95, 2);

        // Only redraw QR if coords changed significantly
        bool qrChanged = layoutChanged ||
            fabs(devLat - prevQrLat) > 0.0001 ||
            fabs(devLon - prevQrLon) > 0.0001;

        if (qrChanged) {
            prevQrLat = devLat;
            prevQrLon = devLon;

            // Clear right-half QR region
            clearRow(CNT_Y, CNT_H, 160, 160);

            // Right half: QR code of device location
            char url[128];
            snprintf(url, sizeof(url),
                "https://maps.apple.com/?ll=%.6f,%.6f&q=Device",
                devLat, devLon);

            QRCode qr;
            uint8_t qrData[qrcode_getBufferSize(5)];
            qrcode_initText(&qr, qrData, 5, ECC_LOW, url);

            int modSize = 3;
            int qrPixels = qr.size * modSize;
            int qrX = 170 + (150 - qrPixels) / 2;
            int qrY = CNT_Y + (CNT_H - qrPixels - 20) / 2;

            // White background with quiet zone
            g.fillRect(qrX - 4, qrY - 4, qrPixels + 8, qrPixels + 8, TFT_WHITE);
            for (uint8_t y = 0; y < qr.size; y++) {
                for (uint8_t x = 0; x < qr.size; x++) {
                    if (qrcode_getModule(&qr, x, y)) {
                        g.fillRect(qrX + x * modSize, qrY + y * modSize,
                                     modSize, modSize, TFT_BLACK);
                    }
                }
            }
            g.setTextColor(C_DIM, C_BG);
            g.setTextDatum(TC_DATUM);
            g.drawString("Device GPS", 170 + 75, qrY + qrPixels + 6, 1);
        }
    } else {
#if HAS_GPS
        g.setTextColor(C_DIM, C_BG);
        g.drawString("GPS: waiting...", 8, CNT_Y + 95, 2);
#endif
    }

    drawSsSettingsButton();

    char foot[32];
    snprintf(foot, sizeof(foot), "BLE:ON WiFi CH:%d", wifiCh);
    drawFooter(foot);
    pushSprite();
}

static void drawSsSettingsButton() {
    auto& g = gfx();
    g.drawRoundRect(SS_SET_BTN_X, SS_BTN_Y, SS_BTN_W, SS_BTN_H, 4, C_FG);
    g.fillRect(SS_SET_BTN_X + 2, SS_BTN_Y + 2, SS_BTN_W - 4, SS_BTN_H - 4, C_BG);
    g.setTextColor(C_FG, C_BG);
    g.setTextDatum(MC_DATUM);
    g.drawString("SETTINGS", SS_SET_BTN_X + SS_BTN_W / 2, SS_BTN_Y + SS_BTN_H / 2, 2);
}

int display_skyspy_touch() {
    int16_t tx, ty;
    if (!display_touch_read(tx, ty)) return 0;
    if (millis() - ssLastTouch < SS_TOUCH_DEBOUNCE_MS) return 0;
    ssLastTouch = millis();

    // Check SETTINGS button (6px padding for easier tapping)
    const int pad = 6;
    if (ty >= SS_BTN_Y - pad && ty <= SS_BTN_Y + SS_BTN_H + pad) {
        if (tx >= SS_SET_BTN_X - pad && tx <= SS_SET_BTN_X + SS_BTN_W + pad) return 1;
    }
    return 0;
}

void display_sd_status(bool inserted, int fileCount) {
    sd_ok = inserted;
    sd_files = fileCount;
}

void display_clear() {
    tft.fillScreen(C_BG);
    currentScreen = SCR_NONE;
}

void display_backlight(bool on) {
    ledcWrite(BL_LEDC_CH, on ? 200 : 0);  // Active HIGH on CYD
}

void display_set_brightness(uint8_t level) {
    if (level == 0) {
        ledcDetachPin(TFT_BL_PIN);
        digitalWrite(TFT_BL_PIN, LOW);  // Active HIGH: LOW = off
    } else {
        ledcAttachPin(TFT_BL_PIN, BL_LEDC_CH);
        ledcWrite(BL_LEDC_CH, level);  // Direct duty (active high)
    }
}

// Settings layout constants (shared by Sky Spy and selector settings pages)
#define SET_ROW_Y      50
#define SET_ROW_H      30
#define SET_BAR_X      90
#define SET_BAR_W      120
#define SET_BAR_H      12
#define SET_VAL_X      220
#define SET_BTN_W      28
#define SET_BTN_H      22
#define SET_MINUS_X    248
#define SET_PLUS_X     280

void display_skyspy_settings(uint8_t dispBr, uint8_t ledBr, uint8_t vol,
                             bool bleOn, bool incognito) {
    // Force full screen clear in non-sprite mode so bars don't ghost
    if (!spriteOk) currentScreen = SCR_NONE;
    initScreen(SCR_SETTINGS, "SETTINGS", true);

    gfx().setTextColor(C_HEADER, C_BG);
    gfx().setTextDatum(TC_DATUM);
    gfx().drawString("SETTINGS", SCR_W / 2, 8, 2);
    gfx().drawFastHLine(0, 35, SCR_W, C_DIM);

    auto drawBar = [&](int y, const char* label, uint8_t val) {
        int pct = (int)roundf(val * 100.0f / 255.0f);
        int barFill = (int)((float)val / 255.0f * SET_BAR_W);
        gfx().setTextColor(C_FG, C_BG);
        gfx().setTextDatum(TL_DATUM);
        gfx().drawString(label, 8, y + 4, 2);
        // Clear bar interior before drawing (prevents ghost in direct-TFT mode)
        gfx().fillRect(SET_BAR_X + 1, y + 3, SET_BAR_W - 2, SET_BAR_H - 2, C_BG);
        gfx().drawRect(SET_BAR_X, y + 2, SET_BAR_W, SET_BAR_H, C_DIM);
        if (barFill > 0) gfx().fillRect(SET_BAR_X, y + 2, barFill, SET_BAR_H, C_FG);
        // Clear value area then draw percentage
        gfx().fillRect(SET_VAL_X, y, 40, SET_ROW_H - 2, C_BG);
        char vBuf[8];
        snprintf(vBuf, sizeof(vBuf), "%d%%", pct);
        gfx().drawString(vBuf, SET_VAL_X, y + 4, 2);
        // +/- buttons
        gfx().drawRect(SET_MINUS_X, y, SET_BTN_W, SET_BTN_H, C_FG);
        gfx().setTextDatum(MC_DATUM);
        gfx().drawString("-", SET_MINUS_X + SET_BTN_W / 2, y + SET_BTN_H / 2, 2);
        gfx().drawRect(SET_PLUS_X, y, SET_BTN_W, SET_BTN_H, C_FG);
        gfx().drawString("+", SET_PLUS_X + SET_BTN_W / 2, y + SET_BTN_H / 2, 2);
    };

    // Row 0: DISPLAY brightness
    int y = SET_ROW_Y;
    drawBar(y, "DISPLAY", dispBr);

    // Row 1: LED brightness
    y += SET_ROW_H;
    drawBar(y, "LED", ledBr);

    // Row 2: VOLUME
    y += SET_ROW_H;
    drawBar(y, "VOLUME", vol);

    // Row 3: BLE toggle
    y += SET_ROW_H;
    gfx().setTextDatum(TL_DATUM);
    gfx().setTextColor(C_FG, C_BG);
    gfx().drawString("BLE", 8, y + 4, 2);
    gfx().fillRect(SET_BAR_X, y, 62, SET_BTN_H + 2, C_BG);
    gfx().drawRect(SET_BAR_X, y, 60, SET_BTN_H, bleOn ? C_FG : C_DIM);
    gfx().setTextDatum(MC_DATUM);
    gfx().setTextColor(bleOn ? C_FG : C_DIM, C_BG);
    gfx().drawString(bleOn ? "ON" : "OFF", SET_BAR_X + 30, y + SET_BTN_H / 2, 2);

    // Row 4: INCOGNITO + BACK
    y += SET_ROW_H + 10;
    gfx().setTextColor(incognito ? C_WARN : C_DIM, C_BG);
    gfx().drawRect(8, y, 100, SET_BTN_H + 4, incognito ? C_WARN : C_DIM);
    gfx().setTextDatum(MC_DATUM);
    gfx().drawString("INCOGNITO", 58, y + (SET_BTN_H + 4) / 2, 2);

    gfx().setTextColor(C_FG, C_BG);
    gfx().drawRect(SCR_W - 80, y, 72, SET_BTN_H + 4, C_FG);
    gfx().setTextDatum(MC_DATUM);
    gfx().drawString("BACK", SCR_W - 44, y + (SET_BTN_H + 4) / 2, 2);

    pushSprite();
}

// ---------- selector settings page -----------------------------------------

void display_settings(uint8_t dispBr, uint8_t ledBr, uint8_t vol,
                      bool buzzer, bool incognito) {
    initScreen(SCR_SETTINGS, "SETTINGS", true);

    // Header (settings uses custom header layout, redraw always in sprite mode)
    gfx().setTextColor(C_HEADER, C_BG);
    gfx().setTextDatum(TC_DATUM);
    gfx().drawString("SETTINGS", SCR_W / 2, 8, 2);
    gfx().drawFastHLine(0, 35, SCR_W, C_DIM);

    // Row 0: DISPLAY brightness
    int y = SET_ROW_Y;
    gfx().setTextColor(C_FG, C_BG);
    gfx().setTextDatum(TL_DATUM);
    gfx().drawString("DISPLAY", 8, y + 4, 2);
    // Bar
    int barFill = (int)((float)dispBr / 255.0f * SET_BAR_W);
    gfx().drawRect(SET_BAR_X, y + 2, SET_BAR_W, SET_BAR_H, C_DIM);
    if (barFill > 0) gfx().fillRect(SET_BAR_X, y + 2, barFill, SET_BAR_H, C_FG);
    // Value
    char vBuf[8];
    snprintf(vBuf, sizeof(vBuf), "%d", dispBr);
    gfx().drawString(vBuf, SET_VAL_X, y + 4, 2);
    // [-] [+]
    gfx().drawRect(SET_MINUS_X, y, SET_BTN_W, SET_BTN_H, C_FG);
    gfx().setTextDatum(MC_DATUM);
    gfx().drawString("-", SET_MINUS_X + SET_BTN_W / 2, y + SET_BTN_H / 2, 2);
    gfx().drawRect(SET_PLUS_X, y, SET_BTN_W, SET_BTN_H, C_FG);
    gfx().drawString("+", SET_PLUS_X + SET_BTN_W / 2, y + SET_BTN_H / 2, 2);

    // Row 1: LED brightness
    y += SET_ROW_H;
    gfx().setTextDatum(TL_DATUM);
    gfx().drawString("LED", 8, y + 4, 2);
    barFill = (int)((float)ledBr / 255.0f * SET_BAR_W);
    gfx().drawRect(SET_BAR_X, y + 2, SET_BAR_W, SET_BAR_H, C_DIM);
    if (barFill > 0) gfx().fillRect(SET_BAR_X, y + 2, barFill, SET_BAR_H, C_FG);
    snprintf(vBuf, sizeof(vBuf), "%d", ledBr);
    gfx().drawString(vBuf, SET_VAL_X, y + 4, 2);
    gfx().drawRect(SET_MINUS_X, y, SET_BTN_W, SET_BTN_H, C_FG);
    gfx().setTextDatum(MC_DATUM);
    gfx().drawString("-", SET_MINUS_X + SET_BTN_W / 2, y + SET_BTN_H / 2, 2);
    gfx().drawRect(SET_PLUS_X, y, SET_BTN_W, SET_BTN_H, C_FG);
    gfx().drawString("+", SET_PLUS_X + SET_BTN_W / 2, y + SET_BTN_H / 2, 2);

    // Row 2: VOLUME
    y += SET_ROW_H;
    gfx().setTextDatum(TL_DATUM);
    gfx().drawString("VOLUME", 8, y + 4, 2);
    barFill = (int)((float)vol / 255.0f * SET_BAR_W);
    gfx().drawRect(SET_BAR_X, y + 2, SET_BAR_W, SET_BAR_H, C_DIM);
    if (barFill > 0) gfx().fillRect(SET_BAR_X, y + 2, barFill, SET_BAR_H, C_FG);
    snprintf(vBuf, sizeof(vBuf), "%d", vol);
    gfx().drawString(vBuf, SET_VAL_X, y + 4, 2);
    gfx().drawRect(SET_MINUS_X, y, SET_BTN_W, SET_BTN_H, C_FG);
    gfx().setTextDatum(MC_DATUM);
    gfx().drawString("-", SET_MINUS_X + SET_BTN_W / 2, y + SET_BTN_H / 2, 2);
    gfx().drawRect(SET_PLUS_X, y, SET_BTN_W, SET_BTN_H, C_FG);
    gfx().drawString("+", SET_PLUS_X + SET_BTN_W / 2, y + SET_BTN_H / 2, 2);

    // Row 3: BUZZER toggle
    y += SET_ROW_H;
    gfx().setTextDatum(TL_DATUM);
    gfx().drawString("BUZZER", 8, y + 4, 2);
    // Toggle button
    gfx().drawRect(SET_BAR_X, y, 60, SET_BTN_H, buzzer ? C_FG : C_DIM);
    gfx().setTextDatum(MC_DATUM);
    gfx().setTextColor(buzzer ? C_FG : C_DIM, C_BG);
    gfx().drawString(buzzer ? "ON" : "OFF", SET_BAR_X + 30, y + SET_BTN_H / 2, 2);

    // Row 4: INCOGNITO toggle + BACK button
    y += SET_ROW_H + 10;
    gfx().setTextColor(incognito ? C_WARN : C_DIM, C_BG);
    gfx().drawRect(8, y, 100, SET_BTN_H + 4, incognito ? C_WARN : C_DIM);
    gfx().setTextDatum(MC_DATUM);
    gfx().drawString("INCOGNITO", 58, y + (SET_BTN_H + 4) / 2, 2);

    // BACK button (bottom right)
    gfx().setTextColor(C_FG, C_BG);
    gfx().drawRect(SCR_W - 80, y, 72, SET_BTN_H + 4, C_FG);
    gfx().setTextDatum(MC_DATUM);
    gfx().drawString("BACK", SCR_W - 44, y + (SET_BTN_H + 4) / 2, 2);

    pushSprite();
}

int display_settings_touch() {
    int16_t tx, ty;
    if (!display_touch_read(tx, ty)) return 0;

    // Debounce
    if (millis() - settings_last_touch < SETTINGS_DEBOUNCE_MS) return 0;
    settings_last_touch = millis();

    // Check rows 0-2 (DISPLAY, LED, VOLUME)
    for (int row = 0; row < 3; row++) {
        int ry = SET_ROW_Y + row * SET_ROW_H;
        if (ty >= ry - 5 && ty < ry + SET_ROW_H - 5) {
            int midX = (SET_MINUS_X + SET_PLUS_X) / 2 + SET_BTN_W / 2;
            if (tx >= SET_MINUS_X - 10 && tx < midX) return 2 + row * 2;
            if (tx >= midX) return 1 + row * 2;
        }
    }

    // Row 3: BUZZER toggle
    int buzRow = SET_ROW_Y + 3 * SET_ROW_H;
    if (ty >= buzRow - 5 && ty < buzRow + SET_ROW_H - 5 &&
        tx >= SET_BAR_X - 20 && tx < SET_BAR_X + 80) {
        return 7;
    }

    // Row 4: INCOGNITO and BACK
    int row4y = SET_ROW_Y + 4 * SET_ROW_H + 10;
    if (ty >= row4y - 10 && ty < row4y + SET_BTN_H + 14) {
        if (tx < SCR_W / 2) return 8;
        if (tx >= SCR_W / 2) return 9;
    }

    return 0;  // no action
}

#endif // ENABLE_TFT_DISPLAY && BOARD_CYD_S3
