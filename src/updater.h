#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD.h>

// Initialize Web OTA server (runs on port 80 if Wi-Fi AP is active)
void initWebUpdater();

// Poll Web OTA server clients in loop()
void handleWebUpdater();

// Check if a firmware update binary exists on the SD card (/dashview.bin or /update.bin)
// Returns true if an update file was detected
bool checkSdUpdateAvailable(char* outFileName, size_t maxLen, size_t* outFileSize);

// Flash firmware from SD card with progress callback
// Returns true if flash succeeded (caller should reboot)
bool performSdUpdate(const char* filename, void (*progressCb)(size_t written, size_t total));

// Global state indicating an OTA or SD update is currently burning to flash
extern bool isUpdateInProgress;
extern int updateProgressPercent;
extern char updateStatusMessage[64];
