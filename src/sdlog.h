/*
 * SD Card Logging Abstraction
 *
 * When HAS_SD_CARD is 1, uses SD_MMC in 4-bit mode on T-Dongle-S3.
 * Otherwise all functions are no-op inline stubs.
 */

#ifndef SDLOG_H
#define SDLOG_H

#include <Arduino.h>
#include "board_config.h"

struct SdLogStats {
    int  totalFiles;
    uint64_t totalBytes;
    uint64_t freeBytes;
};

#if HAS_SD_CARD

bool sdlog_init();
bool sdlog_available();
void sdlog_write(int mode, const char* jsonLine);
void sdlog_start_session(int mode);
void sdlog_flush();
SdLogStats sdlog_get_stats();
int  sdlog_mode_file_count(int mode);
bool        sdlog_is_low();          // true when free < SD_WARN_MB  (show warning)
bool        sdlog_is_full();         // true when free < SD_CRIT_MB  (writes suspended)
const char* sdlog_session_path();    // current session file path, nullptr if none

#else // No SD card — inline no-ops

inline bool sdlog_init() { return false; }
inline bool sdlog_available() { return false; }
inline void sdlog_write(int, const char*) {}
inline void sdlog_start_session(int) {}
inline void sdlog_flush() {}
inline SdLogStats sdlog_get_stats() { return {0, 0, 0}; }
inline int  sdlog_mode_file_count(int) { return 0; }
inline bool        sdlog_is_low()         { return false; }
inline bool        sdlog_is_full()        { return false; }
inline const char* sdlog_session_path()   { return nullptr; }

#endif // HAS_SD_CARD

#endif // SDLOG_H
