#include "profile.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

struct EnumMapEntry {
    int32_t rawVal;
    char text[PROFILE_TEXT_MAX_LEN];
};

struct ProfileSignalDef {
    char key[PROFILE_KEY_MAX_LEN];
    ProfileSignalKind kind;

    // CAN broadcast / enum
    uint32_t canId;
    uint16_t startBit;
    uint8_t bitLen;
    ProfileByteOrder order;
    float scale;
    float offset;
    bool isSigned;
    uint8_t signedWidth;

    // Clamping
    bool hasClampMin;
    float clampMin;
    bool hasClampMax;
    float clampMax;

    // Enum mapping
    uint8_t enumCount;
    EnumMapEntry enumMap[8];
    char defaultEnumText[PROFILE_TEXT_MAX_LEN];

    // OBD Poll
    uint8_t obdMode;
    uint8_t obdPid;
    int8_t dataIndex;
    float a;
    float b;
    float c;
    float lambdaToAfr;

    // Current State
    SignalValue state;
};

// Global Profile State
static ProfileSignalDef s_signals[PROFILE_MAX_SIGNALS];
static int s_signalCount = 0;

static char s_profileId[32] = {0};
static char s_profileName[64] = {0};
static char s_profileLogo[32] = {0};
static char s_profileBrandColor[16] = {0};

static bool s_isCanFd = false;
static uint32_t s_arbBitrate = 500000;
static uint32_t s_reqId = 0x7E0;
static uint32_t s_respId = 0x7E8;
static uint32_t s_funcId = 0x7DF;
static bool s_listenOnly = true;

// Bitfield extractor supporting Motorola (MSB-first) and Intel (LSB-first)
static uint64_t extractBitfield(const uint8_t* data, uint8_t len, uint16_t startBit, uint8_t bitLen, ProfileByteOrder order) {
    if (!data || len == 0 || bitLen == 0 || bitLen > 64) return 0;

    // Fast paths for common byte-aligned Motorola fields
    if (order == BYTE_ORDER_MOTOROLA) {
        uint16_t byteIdx = startBit / 8;
        uint8_t bitInByte = startBit % 8;

        // Byte-aligned 8-bit
        if (bitInByte == 0 && bitLen == 8 && byteIdx < len) {
            return data[byteIdx];
        }
        // Byte-aligned 16-bit big endian
        if (bitInByte == 0 && bitLen == 16 && (byteIdx + 1) < len) {
            return ((uint64_t)data[byteIdx] << 8) | data[byteIdx + 1];
        }
        // Single-bit flag at MSB of byte (e.g. start_bit=31 -> byte 3 bit 7, or start_bit=24 -> byte 3 bit 7)
        if (bitLen == 1 && byteIdx < len) {
            if (bitInByte == 7) {
                return (data[byteIdx] >> 7) & 0x01;
            } else if (bitInByte == 0) {
                return (data[byteIdx] >> 7) & 0x01;
            } else {
                return (data[byteIdx] >> (7 - bitInByte)) & 0x01;
            }
        }
        // Generic Motorola bit extraction
        uint64_t val = 0;
        for (uint8_t i = 0; i < bitLen; i++) {
            uint16_t bitPos = startBit + i;
            uint16_t bIdx = bitPos / 8;
            if (bIdx >= len) break;
            uint8_t bOffset = 7 - (bitPos % 8);
            uint8_t bit = (data[bIdx] >> bOffset) & 0x01;
            val = (val << 1) | bit;
        }
        return val;
    } else {
        // Intel (little-endian)
        uint64_t val = 0;
        for (uint8_t i = 0; i < bitLen; i++) {
            uint16_t bitPos = startBit + i;
            uint16_t bIdx = bitPos / 8;
            if (bIdx >= len) break;
            uint8_t bOffset = bitPos % 8;
            uint8_t bit = (data[bIdx] >> bOffset) & 0x01;
            val |= ((uint64_t)bit << i);
        }
        return val;
    }
}

// Find existing signal index or allocate new
static int findOrAllocateSignal(const char* key) {
    if (!key || key[0] == 0) return -1;
    for (int i = 0; i < s_signalCount; i++) {
        if (strcmp(s_signals[i].key, key) == 0) {
            return i;
        }
    }
    if (s_signalCount < PROFILE_MAX_SIGNALS) {
        int idx = s_signalCount++;
        memset(&s_signals[idx], 0, sizeof(ProfileSignalDef));
        strncpy(s_signals[idx].key, key, PROFILE_KEY_MAX_LEN - 1);
        strncpy(s_signals[idx].state.key, key, PROFILE_KEY_MAX_LEN - 1);
        return idx;
    }
    return -1;
}

static uint32_t parseHexOrDec(JsonVariant v, uint32_t defaultVal = 0) {
    if (v.isNull()) return defaultVal;
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        return (uint32_t)strtoul(s, NULL, 0);
    }
    return v.as<uint32_t>();
}

// Populate standard SAE J1979 universal baseline
static void populateJ1979Base() {
    struct BaseSig {
        const char* key;
        uint8_t pid;
        float a;
        float c;
        float lambdaToAfr;
    };

    static const BaseSig kBaseSigs[] = {
        { "rpm",           0x0C, 0.25f,    0.0f,  0.0f },
        { "speed_kmh",     0x0D, 1.0f,     0.0f,  0.0f },
        { "coolant",       0x05, 1.0f,   -40.0f,  0.0f },
        { "iat",           0x0F, 1.0f,   -40.0f,  0.0f },
        { "throttle",      0x11, 0.39216f, 0.0f,  0.0f },
        { "load",          0x04, 0.39216f, 0.0f,  0.0f },
        { "timing",        0x0E, 0.5f,   -64.0f,  0.0f },
        { "maf",           0x10, 0.01f,    0.0f,  0.0f },
        { "afr_actual",    0x24, 0.0305f,  0.0f, 14.7f },
        { "afr_commanded", 0x44, 0.0305f,  0.0f, 14.7f }
    };

    for (size_t i = 0; i < sizeof(kBaseSigs)/sizeof(kBaseSigs[0]); i++) {
        int idx = findOrAllocateSignal(kBaseSigs[i].key);
        if (idx < 0) continue;
        ProfileSignalDef& s = s_signals[idx];
        s.kind = SIGNAL_KIND_OBD_POLL;
        s.obdMode = 0x01;
        s.obdPid = kBaseSigs[i].pid;
        s.dataIndex = -1;
        s.a = kBaseSigs[i].a;
        s.b = 0;
        s.c = kBaseSigs[i].c;
        s.lambdaToAfr = kBaseSigs[i].lambdaToAfr;
    }
}


static const char kDefaultToyotaProfile[] PROGMEM = R"json(
{
  "id": "toyota_tacoma_2016_2023",
  "name": "Toyota Tacoma (3rd Gen)",
  "match": { "make": "Toyota", "model": "Tacoma", "year_min": 2016, "year_max": 2023, "gen": "3rd" },
  "inherits": "j1979_base",
  "assets": { "logo": "toyota_trd", "brand_color": "#EB0A1E" },
  "bus": {
    "physical": "classic",
    "arb_bitrate": 500000,
    "fd_data_bitrate": null,
    "protocol": "iso_tp",
    "req_id": "0x7E0",
    "resp_id": "0x7E8",
    "func_id": "0x7DF",
    "listen_only": false
  },
  "signals": [
    { "key": "rpm", "kind": "can_broadcast", "can_id": "0x2C4", "start_bit": 0, "bit_len": 16, "order": "motorola", "scale": 0.25, "offset": 0, "signed": false },
    { "key": "load", "kind": "can_broadcast", "can_id": "0x2C4", "start_bit": 16, "bit_len": 8, "order": "motorola", "scale": 0.39216, "offset": 0, "signed": false, "unit": "%" },
    { "key": "throttle", "kind": "can_broadcast", "can_id": "0x2C4", "start_bit": 32, "bit_len": 8, "order": "motorola", "scale": 0.39216, "offset": 0, "signed": false, "unit": "%" },
    { "key": "speed", "kind": "can_broadcast", "can_id": "0x0B4", "start_bit": 40, "bit_len": 16, "order": "motorola", "scale": 0.00621371, "offset": 0, "signed": false, "unit": "mph" },
    { "key": "gear", "kind": "can_enum", "can_id": "0x3BC", "start_bit": 0, "bit_len": 8, "order": "motorola", "map": { "0": "P", "1": "R", "2": "N", "*": "D" } },
    { "key": "tcc_locked", "kind": "can_broadcast", "can_id": "0x3BC", "start_bit": 31, "bit_len": 1, "order": "motorola", "scale": 1, "offset": 0, "signed": false },
    { "key": "kclv", "kind": "obd_poll", "mode": "0x21", "pid": "0xA2", "data_index": 2, "a": 0.1, "clamp_min": 10, "clamp_max": 30 },
    { "key": "knockfb", "kind": "obd_poll", "mode": "0x21", "pid": "0xA2", "data_index": 3, "a": 0.1, "signed": true, "signed_width": 8, "unit": "deg" }
  ]
}
)json";

bool loadDefaultProfile() {
    return loadProfile(kDefaultToyotaProfile);
}

bool loadProfile(const char* json) {
    if (!json || json[0] == 0) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        return false;
    }

    // Reset signal table
    s_signalCount = 0;
    memset(s_signals, 0, sizeof(s_signals));

    // Profile metadata
    const char* idStr = doc["id"];
    if (idStr) strncpy(s_profileId, idStr, sizeof(s_profileId) - 1);

    const char* nameStr = doc["name"];
    if (nameStr) strncpy(s_profileName, nameStr, sizeof(s_profileName) - 1);

    JsonObject assets = doc["assets"];
    if (!assets.isNull()) {
        const char* logo = assets["logo"];
        if (logo) strncpy(s_profileLogo, logo, sizeof(s_profileLogo) - 1);
        const char* col = assets["brand_color"];
        if (col) strncpy(s_profileBrandColor, col, sizeof(s_profileBrandColor) - 1);
    }

    // Layer 1: Bus settings
    JsonObject bus = doc["bus"];
    if (!bus.isNull()) {
        const char* phys = bus["physical"];
        s_isCanFd = (phys && strcmp(phys, "canfd") == 0);
        s_arbBitrate = bus["arb_bitrate"] | 500000;
        s_reqId = parseHexOrDec(bus["req_id"], 0x7E0);
        s_respId = parseHexOrDec(bus["resp_id"], 0x7E8);
        s_funcId = parseHexOrDec(bus["func_id"], 0x7DF);
        s_listenOnly = bus["listen_only"] | true;
    }

    // Layer 2: Inherit baseline
    const char* inherits = doc["inherits"];
    if (inherits && strcmp(inherits, "j1979_base") == 0) {
        populateJ1979Base();
    }

    // Layer 3: Signals
    JsonArray signals = doc["signals"];
    if (!signals.isNull()) {
        for (JsonObject sigObj : signals) {
            const char* key = sigObj["key"];
            if (!key) continue;

            int idx = findOrAllocateSignal(key);
            if (idx < 0) continue;

            ProfileSignalDef& def = s_signals[idx];
            const char* kindStr = sigObj["kind"];

            if (kindStr && strcmp(kindStr, "can_broadcast") == 0) {
                def.kind = SIGNAL_KIND_CAN_BROADCAST;
                def.canId = parseHexOrDec(sigObj["can_id"]);
                def.startBit = sigObj["start_bit"] | 0;
                def.bitLen = sigObj["bit_len"] | 8;
                const char* orderStr = sigObj["order"];
                def.order = (orderStr && strcmp(orderStr, "intel") == 0) ? BYTE_ORDER_INTEL : BYTE_ORDER_MOTOROLA;
                def.scale = sigObj["scale"] | 1.0f;
                def.offset = sigObj["offset"] | 0.0f;
                def.isSigned = sigObj["signed"] | false;
                def.signedWidth = sigObj["signed_width"] | def.bitLen;
                if (sigObj["clamp_min"].is<float>()) {
                    def.hasClampMin = true;
                    def.clampMin = sigObj["clamp_min"].as<float>();
                }
                if (sigObj["clamp_max"].is<float>()) {
                    def.hasClampMax = true;
                    def.clampMax = sigObj["clamp_max"].as<float>();
                }
            } else if (kindStr && strcmp(kindStr, "can_enum") == 0) {
                def.kind = SIGNAL_KIND_CAN_ENUM;
                def.canId = parseHexOrDec(sigObj["can_id"]);
                def.startBit = sigObj["start_bit"] | 0;
                def.bitLen = sigObj["bit_len"] | 8;
                const char* orderStr = sigObj["order"];
                def.order = (orderStr && strcmp(orderStr, "intel") == 0) ? BYTE_ORDER_INTEL : BYTE_ORDER_MOTOROLA;
                
                JsonObject mapObj = sigObj["map"];
                if (!mapObj.isNull()) {
                    def.enumCount = 0;
                    for (JsonPair kv : mapObj) {
                        const char* k = kv.key().c_str();
                        const char* v = kv.value().as<const char*>();
                        if (!v) continue;
                        if (strcmp(k, "*") == 0) {
                            strncpy(def.defaultEnumText, v, sizeof(def.defaultEnumText) - 1);
                        } else if (def.enumCount < 8) {
                            def.enumMap[def.enumCount].rawVal = atoi(k);
                            strncpy(def.enumMap[def.enumCount].text, v, sizeof(def.enumMap[def.enumCount].text) - 1);
                            def.enumCount++;
                        }
                    }
                }
            } else if (kindStr && strcmp(kindStr, "obd_poll") == 0) {
                def.kind = SIGNAL_KIND_OBD_POLL;
                def.obdMode = (uint8_t)parseHexOrDec(sigObj["mode"], 0x01);
                def.obdPid = (uint8_t)parseHexOrDec(sigObj["pid"], 0x00);
                def.dataIndex = sigObj["data_index"] | -1;
                def.a = sigObj["a"] | 1.0f;
                def.b = sigObj["b"] | 0.0f;
                def.c = sigObj["c"] | 0.0f;
                def.lambdaToAfr = sigObj["lambda_to_afr"] | 0.0f;
                def.isSigned = sigObj["signed"] | false;
                def.signedWidth = sigObj["signed_width"] | 8;
                if (sigObj["clamp_min"].is<float>()) {
                    def.hasClampMin = true;
                    def.clampMin = sigObj["clamp_min"].as<float>();
                }
                if (sigObj["clamp_max"].is<float>()) {
                    def.hasClampMax = true;
                    def.clampMax = sigObj["clamp_max"].as<float>();
                }
            }
        }
    }

    return true;
}

bool hasSignal(const char* key) {
    if (!key) return false;
    for (int i = 0; i < s_signalCount; i++) {
        if (strcmp(s_signals[i].key, key) == 0) return true;
    }
    return false;
}

float getSignal(const char* key, float defaultVal) {
    if (!key) return defaultVal;
    for (int i = 0; i < s_signalCount; i++) {
        if (strcmp(s_signals[i].key, key) == 0) {
            if (s_signals[i].state.valid) {
                return s_signals[i].state.value;
            }
            return defaultVal;
        }
    }
    return defaultVal;
}

const char* getSignalText(const char* key, const char* defaultVal) {
    if (!key) return defaultVal;
    for (int i = 0; i < s_signalCount; i++) {
        if (strcmp(s_signals[i].key, key) == 0) {
            if (s_signals[i].state.valid && s_signals[i].state.hasText) {
                return s_signals[i].state.text;
            }
            return defaultVal;
        }
    }
    return defaultVal;
}

unsigned long signalAge(const char* key, unsigned long nowMs) {
    if (!key) return 0xFFFFFFFFUL;
    if (nowMs == 0) nowMs = millis();
    for (int i = 0; i < s_signalCount; i++) {
        if (strcmp(s_signals[i].key, key) == 0) {
            if (!s_signals[i].state.valid) return 0xFFFFFFFFUL;
            return nowMs - s_signals[i].state.lastUpdateMs;
        }
    }
    return 0xFFFFFFFFUL;
}

int getSignalCount() {
    return s_signalCount;
}

const SignalValue* getSignalByIndex(int index) {
    if (index >= 0 && index < s_signalCount) {
        return &s_signals[index].state;
    }
    return nullptr;
}

bool onBroadcastFrame(uint32_t canId, const uint8_t* data, uint8_t len, unsigned long nowMs) {
    if (!data || len == 0) return false;
    if (nowMs == 0) nowMs = millis();
    bool anyUpdated = false;

    for (int i = 0; i < s_signalCount; i++) {
        ProfileSignalDef& sig = s_signals[i];
        if (sig.kind != SIGNAL_KIND_CAN_BROADCAST && sig.kind != SIGNAL_KIND_CAN_ENUM) {
            continue;
        }
        if (sig.canId != canId) {
            continue;
        }

        uint64_t rawVal = extractBitfield(data, len, sig.startBit, sig.bitLen, sig.order);

        if (sig.kind == SIGNAL_KIND_CAN_BROADCAST) {
            float val = 0.0f;
            if (sig.isSigned && sig.bitLen > 0 && sig.bitLen <= 32) {
                int32_t sval = (int32_t)rawVal;
                uint8_t shift = (uint8_t)(32 - (sig.signedWidth > 0 ? sig.signedWidth : sig.bitLen));
                sval = (sval << shift) >> shift;
                val = ((float)sval * sig.scale) + sig.offset;
            } else {
                val = ((float)rawVal * sig.scale) + sig.offset;
            }

            if (sig.hasClampMin && val < sig.clampMin) val = sig.clampMin;
            if (sig.hasClampMax && val > sig.clampMax) val = sig.clampMax;

            sig.state.value = val;
            sig.state.hasText = false;
            sig.state.lastUpdateMs = nowMs;
            sig.state.valid = true;
            anyUpdated = true;
        } else if (sig.kind == SIGNAL_KIND_CAN_ENUM) {
            int32_t ival = (int32_t)rawVal;
            const char* match = nullptr;
            for (uint8_t e = 0; e < sig.enumCount; e++) {
                if (sig.enumMap[e].rawVal == ival) {
                    match = sig.enumMap[e].text;
                    break;
                }
            }
            if (!match && sig.defaultEnumText[0] != '\0') {
                match = sig.defaultEnumText;
            }
            if (match) {
                strncpy(sig.state.text, match, sizeof(sig.state.text) - 1);
                sig.state.text[sizeof(sig.state.text) - 1] = '\0';
                sig.state.hasText = true;
            }
            sig.state.value = (float)ival;
            sig.state.lastUpdateMs = nowMs;
            sig.state.valid = true;
            anyUpdated = true;
        }
    }
    return anyUpdated;
}

bool onObdPollResponse(uint8_t mode, uint8_t pid, const uint8_t* data, uint8_t len, unsigned long nowMs) {
    if (!data || len == 0) return false;
    if (nowMs == 0) nowMs = millis();

    uint8_t reqMode = (mode >= 0x40) ? (mode - 0x40) : mode;
    bool anyUpdated = false;

    for (int i = 0; i < s_signalCount; i++) {
        ProfileSignalDef& sig = s_signals[i];
        if (sig.kind != SIGNAL_KIND_OBD_POLL) continue;

        if (sig.obdMode != reqMode || sig.obdPid != pid) continue;

        float val = 0.0f;
        if (sig.dataIndex >= 0) {
            if (sig.dataIndex < len) {
                if (sig.isSigned) {
                    int8_t sbyte = (int8_t)data[sig.dataIndex];
                    val = ((float)sbyte * sig.a) + sig.c;
                } else {
                    val = ((float)data[sig.dataIndex] * sig.a) + sig.c;
                }
            } else {
                continue;
            }
        } else {
            uint8_t A = (len > 0) ? data[0] : 0;
            uint8_t B = (len > 1) ? data[1] : 0;

            if (sig.lambdaToAfr > 0.0f && len >= 2) {
                float lambda = (float)((A << 8) | B) / 32768.0f;
                val = lambda * sig.lambdaToAfr;
            } else if (len >= 2 && (sig.obdPid == 0x0C || sig.obdPid == 0x10 || sig.obdPid == 0x24 || sig.obdPid == 0x44)) {
                uint16_t raw16 = (A << 8) | B;
                val = ((float)raw16 * sig.a) + sig.c;
            } else {
                val = ((float)A * sig.a) + sig.c;
            }
        }

        if (sig.hasClampMin && val < sig.clampMin) val = sig.clampMin;
        if (sig.hasClampMax && val > sig.clampMax) val = sig.clampMax;

        sig.state.value = val;
        sig.state.hasText = false;
        sig.state.lastUpdateMs = nowMs;
        sig.state.valid = true;
        anyUpdated = true;
    }
    return anyUpdated;
}

uint32_t getReqId() { return s_reqId; }
uint32_t getRespId() { return s_respId; }
uint32_t getFuncId() { return s_funcId; }
bool isListenOnly() { return s_listenOnly; }
uint32_t getArbBitrate() { return s_arbBitrate; }
bool isCanFd() { return s_isCanFd; }
const char* getProfileId() { return s_profileId; }
const char* getProfileName() { return s_profileName; }
const char* getProfileLogo() { return s_profileLogo; }
const char* getProfileBrandColor() { return s_profileBrandColor; }
