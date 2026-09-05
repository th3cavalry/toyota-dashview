// Native unit tests for the vehicle profile engine (src/profile.cpp).
// Build & run: bash tests/native/run.sh   (CI-able; exits non-zero on failure)
#include <cstdio>
#include <cstring>
#include <cmath>
#include "profile.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
static bool nearf(float a, float b) { return std::fabs(a - b) < 0.01f; }

static void test_load() {
    printf("== load\n");
    CHECK(loadDefaultProfile(), "loadDefaultProfile failed");
    CHECK(!strcmp(getProfileId(), "toyota_tacoma_2016_2023"), "id=%s", getProfileId());
    CHECK(getReqId() == 0x7E0 && getRespId() == 0x7E8 && getFuncId() == 0x7DF, "bus ids");
    CHECK(!isListenOnly(), "listen_only must be false for a polling profile");
    CHECK(getArbBitrate() == 500000, "arb=%u", getArbBitrate());
    CHECK(getSignalCount() == 15, "signals=%d (8 tacoma + 10 j1979 - 3 overrides)", getSignalCount());
    CHECK(hasSignal("rpm") && hasSignal("kclv") && hasSignal("coolant"), "inheritance merge");
}

static void test_broadcast_decode() {
    printf("== broadcast decode\n");
    // rpm: 0x2C4 bits0-15 motorola /4.  4000 raw -> 1000 rpm
    uint8_t a4c4[8] = { 0x0F, 0xA0, 0x64, 0x40, 0x80, 0, 0, 0 };
    CHECK(onBroadcastFrame(0x2C4, a4c4, 8, 2000), "frame not consumed");
    CHECK(nearf(getSignal("rpm"), 1000), "rpm=%f", getSignal("rpm"));
    // load: d2=100 * 0.39216 = 39.216%
    CHECK(nearf(getSignal("load"), 39.216f), "load=%f", getSignal("load"));
    // speed: 0x0B4 bits40-55 (bytes 5-6, big endian) *0.00621371; raw 16000 -> 99.4 mph
    // (scale flagged for bench calibration; test pins current JSON behavior)
    uint8_t b4[8] = { 0, 0, 0, 0, 0, 0x3E, 0x80, 0 };
    CHECK(onBroadcastFrame(0x0B4, b4, 8, 2010), "0x0B4 not consumed");
    CHECK(nearf(getSignal("speed"), 99.42f), "speed=%f", getSignal("speed"));
    // gear enum 0x3BC: d0=0 -> P, d0=1 -> R, d0=3 -> '*' default D
    uint8_t bc[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    onBroadcastFrame(0x3BC, bc, 8, 2020);
    CHECK(!strcmp(getSignalText("gear"), "P"), "gear=%s", getSignalText("gear"));
    bc[0] = 1; onBroadcastFrame(0x3BC, bc, 8, 2030);
    CHECK(!strcmp(getSignalText("gear"), "R"), "gear=%s", getSignalText("gear"));
    bc[0] = 3; onBroadcastFrame(0x3BC, bc, 8, 2040);
    CHECK(!strcmp(getSignalText("gear"), "D"), "gear=%s", getSignalText("gear"));
    // tcc_locked: 0x3BC bit31 = byte 3 MSB
    bc[3] = 0x80; onBroadcastFrame(0x3BC, bc, 8, 2050);
    CHECK(nearf(getSignal("tcc_locked"), 1), "tcc=%f", getSignal("tcc_locked"));
    // other ids ignored
    CHECK(!onBroadcastFrame(0x123, bc, 8, 2060), "unknown id should not consume");
}

static void test_obd_decode() {
    printf("== obd poll decode\n");
    // kclv 0x21 A2 idx2 *0.1 clamp 10..30 : p2=140 -> 14.0
    uint8_t d[8] = { 0, 0, 140, 0, 0, 0, 0, 0 };
    CHECK(onObdPollResponse(0x61, 0xA2, d, 8, 3000), "61 A2 not consumed");
    CHECK(nearf(getSignal("kclv"), 14.0f), "kclv=%f", getSignal("kclv"));
    d[2] = 50;  onObdPollResponse(0x61, 0xA2, d, 8, 3010);   // 5.0 -> clamped to 10
    CHECK(nearf(getSignal("kclv"), 10.0f), "clamp_min kclv=%f", getSignal("kclv"));
    // knockfb idx3 *0.1 signed: p3=200 -> int8 -56 -> -5.6 deg
    d[3] = 200; onObdPollResponse(0x61, 0xA2, d, 8, 3020);
    CHECK(nearf(getSignal("knockfb"), -5.6f), "knock=%f", getSignal("knockfb"));
    // j1979 coolant (req 01 05 -> resp 41 05): A=90 -40 = 50C
    uint8_t c[8] = { 90, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(onObdPollResponse(0x41, 0x05, c, 8, 3030), "coolant not consumed");
    CHECK(nearf(getSignal("coolant"), 50.0f), "coolant=%f", getSignal("coolant"));
    // afr_actual (req 01 24 -> resp 41 24) lambda*14.7: A,B=0x4000 -> 0.5 -> 7.35
    uint8_t l[8] = { 0x40, 0x00, 0, 0, 0, 0, 0, 0 };
    CHECK(onObdPollResponse(0x41, 0x24, l, 8, 3040), "afr not consumed");
    CHECK(nearf(getSignal("afr_actual"), 7.35f), "afr=%f", getSignal("afr_actual"));
    // timing (req 01 0E -> resp 41 0E): A=128*0.5-64 = 0
    uint8_t t[8] = { 128, 0, 0, 0, 0, 0, 0, 0 };
    onObdPollResponse(0x41, 0x0E, t, 8, 3050);
    CHECK(nearf(getSignal("timing"), 0.0f), "timing=%f", getSignal("timing"));
    CHECK(!onObdPollResponse(0x41, 0x99, c, 8, 3060), "unknown pid should not consume");
}

static void test_freshness() {
    printf("== freshness\n");
    CHECK(signalAge("rpm", 2050) == 50, "age=%lu", signalAge("rpm", 2050));
    unsigned long huge = signalAge("iat", 3000);   // never updated
    CHECK(huge > 1000000UL, "stale sentinel=%lu", huge);
}

static void test_meta() {
    printf("== meta / poll-id (Custom Dash gauge binding)\n");
    SignalMeta m{};
    CHECK(getSignalMeta("knockfb", &m), "meta knockfb");
    CHECK(!strcmp(m.unit, "deg"), "unit=%s", m.unit);
    CHECK(m.decimals == 1, "dec=%u", m.decimals);
    CHECK(getSignalMeta("kclv", &m) && m.hasClampMin && nearf(m.clampMin, 10), "kclv clamp");
    uint8_t mode, pid;
    CHECK(profileSignalPollId("kclv", &mode, &pid) && mode == 0x21 && pid == 0xA2, "poll id kclv");
    CHECK(!profileSignalPollId("rpm", &mode, &pid), "rpm is broadcast, not poll");
    CHECK(profileSignalPollId("coolant", &mode, &pid) && mode == 0x01 && pid == 0x05, "poll id coolant");
}

static void test_bad_json() {
    printf("== robustness\n");
    CHECK(!loadProfile("{ not json"), "garbage rejected");
    CHECK(!loadProfile("{}"), "empty object rejected");
    CHECK(!loadProfile(nullptr), "null rejected");
    // metadata must NOT leak from the previous profile (Copilot review #4)
    loadDefaultProfile();  // tacoma: req 0x7E0, listen_only false
    CHECK(loadProfile(R"({"id":"min","signals":[{"key":"x","kind":"obd_poll","mode":"0x01","pid":"0x0D"}]})"),
          "minimal profile loads");
    CHECK(!strcmp(getProfileId(), "min"), "id replaced=%s", getProfileId());
    CHECK(getReqId() == 0x7E0 && getRespId() == 0x7E8, "bus ids reset to defaults");
    CHECK(isListenOnly(), "listen_only reset to safe default when bus section missing");
    CHECK(getProfileLogo()[0] == 0, "logo from previous profile cleared");
    loadDefaultProfile();  // restore for test order
    // 60 signals > PROFILE_MAX 48 must not crash; extra entries dropped
    char buf[8192]; int n = snprintf(buf, sizeof(buf), "{\"id\":\"huge\",\"signals\":[");
    for (int i = 0; i < 60; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"key\":\"s%d\",\"kind\":\"obd_poll\",\"mode\":\"0x01\",\"pid\":\"0x%02X\",\"a\":1}",
                      i ? "," : "", i, 0x30 + (i % 10));
    snprintf(buf + n, sizeof(buf) - n, "]}");
    CHECK(loadProfile(buf), "huge profile should load (truncated)");
    CHECK(getSignalCount() <= 48, "signal cap respected: %d", getSignalCount());
    // reload good profile for anything after
    loadDefaultProfile();
}

int main() {
    test_load();
    test_broadcast_decode();
    test_obd_decode();
    test_freshness();
    test_meta();
    test_bad_json();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
