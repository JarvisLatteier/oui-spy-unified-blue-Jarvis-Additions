/*
 * TFT Display Implementation — LILYGO T-Dongle-S3
 *
 * 0.96" ST7735 (80x160) driven by TFT_eSPI with TFT_eSprite for
 * flicker-free rendering. Green-on-black terminal aesthetic.
 *
 * Layout (landscape 160x80):
 *   HEADER (10px)  — mode name
 *   CONTENT (60px) — mode-specific data
 *   FOOTER (10px)  — status / IP / SD
 */

#if defined(ENABLE_TFT_DISPLAY) && !defined(BOARD_CYD_S3)

#include "display.h"
#include <TFT_eSPI.h>

// ---------- globals --------------------------------------------------------
static TFT_eSPI    tft = TFT_eSPI();
static TFT_eSprite spr = TFT_eSprite(&tft);

// Colours
#define C_BG     TFT_BLACK
#define C_FG     TFT_GREEN
#define C_DIM    tft.color565(0, 100, 0)
#define C_HEADER tft.color565(0, 180, 0)
#define C_WARN   TFT_YELLOW
#define C_ALERT  TFT_RED
#define C_WHITE  TFT_WHITE

// Layout constants (landscape: 160 wide x 80 tall)
#define SCR_W 160
#define SCR_H 80
#define HDR_H 12
#define FTR_H 10
#define CNT_Y HDR_H
#define CNT_H (SCR_H - HDR_H - FTR_H)
#define FTR_Y (SCR_H - FTR_H)

// SD status cache
static bool sd_ok = false;
static int  sd_files = 0;

// ---------- helpers --------------------------------------------------------

static void drawHeader(const char* title) {
    spr.setTextColor(C_HEADER, C_BG);
    spr.setTextDatum(TC_DATUM);
    spr.drawString(title, SCR_W / 2, 1, 1);
    spr.drawFastHLine(0, HDR_H - 1, SCR_W, C_DIM);
}

static void drawFooter(const char* text) {
    spr.drawFastHLine(0, FTR_Y, SCR_W, C_DIM);
    spr.setTextColor(C_DIM, C_BG);
    spr.setTextDatum(TL_DATUM);
    spr.drawString(text, 2, FTR_Y + 1, 1);

    // SD indicator on right side of footer
    char sdBuf[20];
    if (sd_ok) {
        snprintf(sdBuf, sizeof(sdBuf), "SD:OK / Files:%d", sd_files);
    } else {
        snprintf(sdBuf, sizeof(sdBuf), "SD:---");
    }
    spr.setTextDatum(TR_DATUM);
    spr.drawString(sdBuf, SCR_W - 2, FTR_Y + 1, 1);
}

static void pushSprite() {
    spr.pushSprite(0, 0);
}

// ---------- public API -----------------------------------------------------

void display_init() {
    tft.init();
    tft.setRotation(3);           // landscape: 160x80, USB on left

    // PWM backlight
#ifdef TFT_BL
    ledcSetup(0, 5000, 8);        // Channel 0, 5kHz, 8-bit
    ledcAttachPin(TFT_BL, 0);
    ledcWrite(0, 55);              // ~78% brightness (active-low: 255-200)
#endif

    tft.fillScreen(C_BG);

    // Create sprite (full-screen buffer)
    spr.createSprite(SCR_W, SCR_H);
    spr.setTextColor(C_FG, C_BG);
}

void display_boot_splash(const char* modeName) {
    spr.fillSprite(C_BG);

    spr.setTextColor(C_HEADER, C_BG);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("OUI-SPY", SCR_W / 2, 8, 2);

    spr.setTextColor(C_FG, C_BG);
    spr.drawString(modeName, SCR_W / 2, 32, 2);

    spr.setTextColor(C_DIM, C_BG);
    spr.drawString("Initializing...", SCR_W / 2, 56, 1);

    pushSprite();
}

void display_selector(int index, const char* ssid, const char* ip) {
    spr.fillSprite(C_BG);
    drawHeader("SELECT MODE");

    static const char* modes[] = {"DETECTOR", "FOXHUNTER", "FLOCK-YOU", "SKY SPY"};
    static const int   modeNums[] = {1, 2, 4, 5};

    int y = CNT_Y + 2;
    for (int i = 0; i < 4; i++) {
        if (i == index) {
            // Highlighted item — inverse colors
            spr.fillRect(0, y, SCR_W, 11, C_FG);
            spr.setTextColor(C_BG, C_FG);
        } else {
            spr.setTextColor(C_FG, C_BG);
        }
        char line[24];
        snprintf(line, sizeof(line), "%d %s", modeNums[i], modes[i]);
        spr.setTextDatum(TL_DATUM);
        spr.drawString(line, 4, y + 1, 1);
        y += 12;
    }

    // Button hints in footer
    spr.drawFastHLine(0, FTR_Y, SCR_W, C_DIM);
    spr.setTextColor(C_DIM, C_BG);
    spr.setTextDatum(TL_DATUM);
    spr.drawString("BTN:next HOLD:go", 2, FTR_Y + 1, 1);

    pushSprite();
}

void display_detector_event(const char* mac, int rssi, const char* alias,
                            const char* type, int count) {
    spr.fillSprite(C_BG);
    drawHeader("DETECTOR");

    spr.setTextColor(C_ALERT, C_BG);
    spr.setTextDatum(TL_DATUM);
    spr.drawString(type, 4, CNT_Y + 2, 1);

    spr.setTextColor(C_FG, C_BG);
    // MAC (truncate to fit)
    spr.drawString(mac, 4, CNT_Y + 14, 1);

    // RSSI
    char rssiBuf[16];
    snprintf(rssiBuf, sizeof(rssiBuf), "RSSI:%d", rssi);
    spr.setTextDatum(TR_DATUM);
    spr.drawString(rssiBuf, SCR_W - 4, CNT_Y + 14, 1);

    // Alias or filter
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(C_WARN, C_BG);
    if (alias && alias[0]) {
        spr.drawString(alias, 4, CNT_Y + 28, 1);
    }

    // Total count
    char cntBuf[16];
    snprintf(cntBuf, sizeof(cntBuf), "Total: %d", count);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(cntBuf, 4, CNT_Y + 42, 1);

    drawFooter("Scanning...");
    pushSprite();
}

void display_detector_scanning(int filters, int devices) {
    spr.fillSprite(C_BG);
    drawHeader("DETECTOR");

    spr.setTextColor(C_FG, C_BG);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("Scanning...", SCR_W / 2, CNT_Y + 10, 2);

    char buf[32];
    snprintf(buf, sizeof(buf), "Filters:%d  Found:%d", filters, devices);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(buf, SCR_W / 2, CNT_Y + 35, 1);

    drawFooter("BLE Active");
    pushSprite();
}

void display_foxhunter(int rssi, const char* mac, bool detected,
                       const char* status) {
    spr.fillSprite(C_BG);
    drawHeader("FOXHUNTER");

    if (detected) {
        // Large RSSI number
        char rssiBuf[8];
        snprintf(rssiBuf, sizeof(rssiBuf), "%d", rssi);
        spr.setTextColor(C_FG, C_BG);
        spr.setTextDatum(TC_DATUM);
        spr.drawString(rssiBuf, SCR_W / 2, CNT_Y + 2, 4);

        // Proximity bar
        int barW = map(constrain(rssi, -95, -25), -95, -25, 2, SCR_W - 8);
        uint16_t barColor;
        if (rssi >= -45)      barColor = TFT_GREEN;
        else if (rssi >= -65) barColor = TFT_YELLOW;
        else                  barColor = TFT_RED;
        spr.fillRect(4, CNT_Y + 38, barW, 8, barColor);
        spr.drawRect(4, CNT_Y + 38, SCR_W - 8, 8, C_DIM);

        // MAC
        spr.setTextColor(C_DIM, C_BG);
        spr.setTextDatum(TC_DATUM);
        spr.drawString(mac, SCR_W / 2, CNT_Y + 50, 1);
    } else {
        spr.setTextColor(C_WARN, C_BG);
        spr.setTextDatum(TC_DATUM);
        spr.drawString("Searching...", SCR_W / 2, CNT_Y + 18, 2);
    }

    drawFooter(status);
    pushSprite();
}

void display_flockyou(int count, int inRange, bool buzzer, const char* mac,
                      const char* method, int rssi, int /*gpsSats*/,
                      const char* /*name*/, int /*sightings*/,
                      bool /*isRaven*/, const char* /*ravenFW*/,
                      bool /*gpsTagged*/, const char* /*alias*/) {
    spr.fillSprite(C_BG);
    drawHeader("FLOCK-YOU");

    spr.setTextColor(C_FG, C_BG);
    spr.setTextDatum(TL_DATUM);

    char line1[32];
    snprintf(line1, sizeof(line1), "Det:%d  InRange:%d", count, inRange);
    spr.drawString(line1, 4, CNT_Y + 2, 1);

    if (mac && mac[0]) {
        spr.setTextColor(C_WARN, C_BG);
        spr.drawString(mac, 4, CNT_Y + 16, 1);

        char line3[32];
        snprintf(line3, sizeof(line3), "[%s] %ddBm", method ? method : "?", rssi);
        spr.setTextColor(C_DIM, C_BG);
        spr.drawString(line3, 4, CNT_Y + 28, 1);
    }

    char foot[24];
    snprintf(foot, sizeof(foot), "BZR:%s", buzzer ? "ON" : "OFF");
    drawFooter(foot);
    pushSprite();
}

void display_skyspy(const char* mac, const char* id, double lat, double lon,
                    int alt, int rssi, int total,
                    double /*pilotLat*/, double /*pilotLon*/,
                    int /*gpsSats*/, int wifiCh, int /*sessionsSeen*/) {
    spr.fillSprite(C_BG);
    drawHeader("SKY SPY");

    spr.setTextColor(C_ALERT, C_BG);
    spr.setTextDatum(TL_DATUM);
    spr.drawString("DRONE", 4, CNT_Y + 2, 1);

    spr.setTextColor(C_FG, C_BG);
    // ID
    if (id && id[0]) {
        spr.drawString(id, 4, CNT_Y + 14, 1);
    }
    // MAC
    if (mac && mac[0]) {
        spr.drawString(mac, 4, CNT_Y + 26, 1);
    }
    // Position
    char posBuf[32];
    snprintf(posBuf, sizeof(posBuf), "Alt:%dm RSSI:%d", alt, rssi);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(posBuf, 4, CNT_Y + 38, 1);

    // GPS
    char gpsBuf[32];
    snprintf(gpsBuf, sizeof(gpsBuf), "%.4f,%.4f", lat, lon);
    spr.drawString(gpsBuf, 4, CNT_Y + 50, 1);

    char foot[32];
    snprintf(foot, sizeof(foot), "Tot:%d CH:%d", total, wifiCh);
    drawFooter(foot);
    pushSprite();
}

void display_skyspy_scanning(int count, double /*devLat*/, double /*devLon*/,
                             int /*gpsSats*/, int wifiCh) {
    spr.fillSprite(C_BG);
    drawHeader("SKY SPY");

    spr.setTextColor(C_FG, C_BG);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("Scanning...", SCR_W / 2, CNT_Y + 10, 2);

    char buf[24];
    snprintf(buf, sizeof(buf), "Drones: %d", count);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(buf, SCR_W / 2, CNT_Y + 35, 1);

    char foot[32];
    snprintf(foot, sizeof(foot), "BLE:ON WiFi CH:%d", wifiCh);
    drawFooter(foot);
    pushSprite();
}

int display_skyspy_touch() { return 0; }  // no touch on T-Dongle

void display_sd_status(bool inserted, int fileCount) {
    sd_ok = inserted;
    sd_files = fileCount;
}

void display_clear() {
    tft.fillScreen(C_BG);
}

void display_backlight(bool on) {
#ifdef TFT_BL
    ledcWrite(0, on ? 55 : 255);
#endif
}

void display_set_brightness(uint8_t level) {
#ifdef TFT_BL
    if (level == 0) {
        ledcDetachPin(TFT_BL);
        digitalWrite(TFT_BL, HIGH);   // active-low: HIGH = off
    } else {
        ledcAttachPin(TFT_BL, 0);
        ledcWrite(0, 255 - level);
    }
#endif
}

// Touch stubs (not available on T-Dongle)
void display_touch_init() {}
bool display_touch_read(int16_t &x, int16_t &y) { return false; }
int  display_selector_touch() { return -1; }

// Settings stubs (not available on T-Dongle)
void display_settings(uint8_t, uint8_t, uint8_t, bool, bool) {}
int  display_settings_touch() { return 0; }
void display_skyspy_settings(uint8_t, uint8_t, uint8_t, bool, bool) {}

// Sprite lifecycle stubs (not needed on T-Dongle, sprite is small)
void display_free_sprite() {}
void display_create_sprite() {}

// Touch calibration stubs
bool display_touch_calibration_load() { return false; }
void display_touch_calibrate() {}

#endif // ENABLE_TFT_DISPLAY && !BOARD_CYD_S3
