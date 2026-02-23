/*
 * SD Card Logging Implementation — LILYGO T-Dongle-S3
 *
 * Uses ESP32 SD_MMC in 4-bit mode. Log format is JSONL (one JSON object per
 * line) for easy parsing and append-only operation.
 *
 * Directory structure:
 *   /oui-spy/detector/session_NNNNNN.jsonl
 *   /oui-spy/foxhunter/session_NNNNNN.jsonl
 *   /oui-spy/flockyou/session_NNNNNN.jsonl
 *   /oui-spy/skyspy/session_NNNNNN.jsonl
 */

#include "board_config.h"

#if HAS_SD_CARD

#include "sdlog.h"
#include <SD_MMC.h>

// Mode name mapping
static const char* modeDirs[] = {
    "unknown",    // 0
    "detector",   // 1
    "foxhunter",  // 2
    "unknown3",   // 3
    "flockyou",   // 4
    "skyspy"      // 5
};

// Free-space thresholds
static const uint64_t SD_WARN_MB = 50ULL  * 1024 * 1024;  // show warning below 50 MB
static const uint64_t SD_CRIT_MB =  5ULL  * 1024 * 1024;  // suspend writes below 5 MB

static bool     _mounted  = false;
static bool     _sdLow    = false;   // free < SD_WARN_MB
static bool     _sdFull   = false;   // free < SD_CRIT_MB (writes suspended)
static File     _logFile;
static int      _currentMode = -1;
static char     _logPath[64] = "";
static char     _writeBuf[512];
static int      _bufPos = 0;

// ---------- internal helpers -----------------------------------------------

static const char* modeDir(int mode) {
    if (mode >= 0 && mode <= 5) return modeDirs[mode];
    return "unknown";
}

static void ensureDir(const char* path) {
    if (!SD_MMC.exists(path)) {
        SD_MMC.mkdir(path);
    }
}

static int nextSessionNumber(const char* dirPath) {
    int maxNum = 0;
    File dir = SD_MMC.open(dirPath);
    if (!dir || !dir.isDirectory()) return 1;

    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();
        // Parse session_NNNNNN.jsonl
        if (strncmp(name, "session_", 8) == 0) {
            int num = atoi(name + 8);
            if (num > maxNum) maxNum = num;
        }
        f = dir.openNextFile();
    }
    return maxNum + 1;
}

static void checkSpace() {
    uint64_t free = SD_MMC.totalBytes() - SD_MMC.usedBytes();
    _sdFull = (free < SD_CRIT_MB);
    _sdLow  = (free < SD_WARN_MB);
    if (_sdFull) Serial.printf("[SD] CRITICAL: only %llu KB free — logging suspended\n", free / 1024);
    else if (_sdLow) Serial.printf("[SD] WARNING: only %llu MB free\n", free / (1024*1024));
}

static void flushBuffer() {
    if (_bufPos > 0 && _logFile) {
        _logFile.write((uint8_t*)_writeBuf, _bufPos);
        _logFile.flush();
        _bufPos = 0;
    }
}

// ---------- public API -----------------------------------------------------

bool sdlog_init() {
#if defined(BOARD_CYD_S3)
    // 4-bit SDMMC mode for Freenove CYD
    SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN,
                   SD_MMC_D1_PIN, SD_MMC_D2_PIN, SD_MMC_D3_PIN);
    _mounted = SD_MMC.begin("/sdcard", false);  // false = 4-bit mode
#else
    // 1-bit SDMMC mode (CLK=12, CMD=16, D0=14) — proven config for T-Dongle-S3
    SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);
    _mounted = SD_MMC.begin("/sdcard", true);   // true = 1-bit mode
#endif

    if (_mounted) {
        ensureDir("/oui-spy");
        ensureDir("/oui-spy/detector");
        ensureDir("/oui-spy/foxhunter");
        ensureDir("/oui-spy/flockyou");
        ensureDir("/oui-spy/skyspy");
        Serial.printf("[SD] Mounted — %.1f MB total\n",
                      SD_MMC.totalBytes() / 1048576.0f);
    } else {
        Serial.println("[SD] No card detected (non-fatal)");
    }
    return _mounted;
}

bool sdlog_available() {
    return _mounted;
}

void sdlog_start_session(int mode) {
    if (!_mounted) return;

    // Close previous file if open
    if (_logFile) {
        flushBuffer();
        _logFile.close();
    }

    _currentMode = mode;

    // Prepare path but don't create file yet — deferred until first write
    char dirPath[32];
    snprintf(dirPath, sizeof(dirPath), "/oui-spy/%s", modeDir(mode));
    ensureDir(dirPath);

    checkSpace();
    int num = nextSessionNumber(dirPath);
    snprintf(_logPath, sizeof(_logPath), "%s/session_%06d.jsonl", dirPath, num);
    Serial.printf("[SD] Session ready: %s (deferred)\n", _logPath);
}

static void openLogFileIfNeeded() {
    if (_logFile || !_mounted || _logPath[0] == '\0') return;
    _logFile = SD_MMC.open(_logPath, FILE_APPEND);
    if (_logFile) {
        Serial.printf("[SD] Session created: %s\n", _logPath);
    } else {
        Serial.printf("[SD] Failed to open: %s\n", _logPath);
    }
}

void sdlog_write(int mode, const char* jsonLine) {
    if (!_mounted || _sdFull) return;
    openLogFileIfNeeded();
    if (!_logFile) return;

    int len = strlen(jsonLine);
    // If line + newline won't fit in buffer, flush first
    if (_bufPos + len + 1 >= (int)sizeof(_writeBuf)) {
        flushBuffer();
    }
    // If single line exceeds buffer, write directly
    if (len + 1 >= (int)sizeof(_writeBuf)) {
        _logFile.print(jsonLine);
        _logFile.print('\n');
        _logFile.flush();
        return;
    }
    memcpy(_writeBuf + _bufPos, jsonLine, len);
    _bufPos += len;
    _writeBuf[_bufPos++] = '\n';

    // Auto-flush when buffer is mostly full
    if (_bufPos >= 400) {
        flushBuffer();
    }
}

void sdlog_flush() {
    flushBuffer();
}

SdLogStats sdlog_get_stats() {
    SdLogStats s = {0, 0, 0};
    if (!_mounted) return s;

    s.totalBytes = SD_MMC.totalBytes();
    s.freeBytes  = SD_MMC.totalBytes() - SD_MMC.usedBytes();

    // Count session files
    const char* dirs[] = {
        "/oui-spy/detector", "/oui-spy/foxhunter",
        "/oui-spy/flockyou", "/oui-spy/skyspy"
    };
    for (int d = 0; d < 4; d++) {
        File dir = SD_MMC.open(dirs[d]);
        if (!dir || !dir.isDirectory()) continue;
        File f = dir.openNextFile();
        while (f) {
            s.totalFiles++;
            f = dir.openNextFile();
        }
    }
    return s;
}

int sdlog_mode_file_count(int mode) {
    if (!_mounted) return 0;
    char dirPath[32];
    snprintf(dirPath, sizeof(dirPath), "/oui-spy/%s", modeDir(mode));
    File dir = SD_MMC.open(dirPath);
    if (!dir || !dir.isDirectory()) return 0;
    int count = 0;
    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();
        int len = strlen(name);
        if (len > 6 && strcmp(name + len - 6, ".jsonl") == 0) count++;
        f = dir.openNextFile();
    }
    return count;
}

const char* sdlog_session_path() {
    return _logPath[0] ? _logPath : nullptr;
}

bool sdlog_is_low()  { return _sdLow;  }
bool sdlog_is_full() { return _sdFull; }

#endif // HAS_SD_CARD
