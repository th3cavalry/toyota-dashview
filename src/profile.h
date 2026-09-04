#pragma once

#include <stdint.h>
#include <stddef.h>
#include <ArduinoJson.h>

#define PROFILE_MAX_SIGNALS 48
#define PROFILE_KEY_MAX_LEN 24
#define PROFILE_TEXT_MAX_LEN 16

enum ProfileSignalKind {
    SIGNAL_KIND_UNKNOWN = 0,
    SIGNAL_KIND_CAN_BROADCAST,
    SIGNAL_KIND_CAN_ENUM,
    SIGNAL_KIND_OBD_POLL
};

enum ProfileByteOrder {
    BYTE_ORDER_MOTOROLA = 0, // MSB-first / big-endian
    BYTE_ORDER_INTEL = 1     // LSB-first / little-endian
};

struct SignalValue {
    char key[PROFILE_KEY_MAX_LEN];
    float value;
    char text[PROFILE_TEXT_MAX_LEN];
    bool hasText;
    unsigned long lastUpdateMs;
    bool valid;
};

// Profile lifecycle & signal accessors
bool loadProfile(const char* json);
bool loadDefaultProfile();
bool hasSignal(const char* key);
float getSignal(const char* key, float defaultVal = 0.0f);
const char* getSignalText(const char* key, const char* defaultVal = "");
unsigned long signalAge(const char* key, unsigned long nowMs = 0);
int getSignalCount();
const SignalValue* getSignalByIndex(int index);

// CAN broadcast & OBD response decode hooks
bool onBroadcastFrame(uint32_t canId, const uint8_t* data, uint8_t len, unsigned long nowMs = 0);
bool onObdPollResponse(uint8_t mode, uint8_t pid, const uint8_t* data, uint8_t len, unsigned long nowMs = 0);

// Bus & vehicle metadata accessors
uint32_t getReqId();
uint32_t getRespId();
uint32_t getFuncId();
bool isListenOnly();
uint32_t getArbBitrate();
bool isCanFd();
const char* getProfileId();
const char* getProfileName();
const char* getProfileLogo();
const char* getProfileBrandColor();
