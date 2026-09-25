// Native unit tests for the CAN bus module (src/core/can_bus.cpp).
// Drives the TX failsafe state machine through the fake TWAI bus.
// Build & run: bash tests/native/run.sh   (exits non-zero on failure)
#include <cstdio>
#include <cstring>
#include "profile.h"
#include "core/can_bus.h"
#include "driver/twai.h"   // native shim (pulls in the Arduino.h shim)

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define PROFILE_TXABLE R"({"id":"t","bus":{"req_id":"0x7E0","func_id":"0x7DF","listen_only":false},"signals":[{"key":"x","kind":"obd_poll","mode":"0x01","pid":"0x0D"}]})"
#define PROFILE_LISTEN R"({"id":"t","bus":{"req_id":"0x7E0","func_id":"0x7DF","listen_only":true},"signals":[{"key":"x","kind":"obd_poll","mode":"0x01","pid":"0x0D"}]})"

static void resetBus() {
    g_fakeBus = FakeBus{};
    setMillisForTest(1000);
}

static void test_clean_bus() {
    printf("== clean bus\n");
    resetBus();
    loadProfile(PROFILE_TXABLE);
    CHECK(canBusTxCleared() == true, "clean bus should allow TX");
}

static void test_listen_only() {
    printf("== listen-only profile\n");
    resetBus();
    loadProfile(PROFILE_LISTEN);
    CHECK(canBusTxCleared() == false, "listen-only profile must never allow TX");
    // Absolute kill-switch: time passing must not re-arm TX.
    setMillisForTest(60000);
    CHECK(canBusTxCleared() == false, "listen-only must not recover with time");
}

static void test_tec_elevated_and_cooldown() {
    printf("== TEC elevated + cooldown window\n");
    resetBus();
    loadProfile(PROFILE_TXABLE);
    g_fakeBus.tec = 30;
    CHECK(canBusTxCleared() == false, "TEC=30 must block TX");
    // Cooldown = millis()+5000 at detection (1000+5000=6000): still blocked
    // at 5999 even though the bus counters healed.
    setMillisForTest(5999);
    g_fakeBus.tec = 0;
    CHECK(canBusTxCleared() == false, "must stay blocked inside the 5s cooldown");
    setMillisForTest(6000);
    CHECK(canBusTxCleared() == true, "cooldown expiry (millis==inhibit) must allow TX");
}

static void test_bus_off_recovery_cycle() {
    printf("== bus-off recovery cycles the controller\n");
    resetBus();
    loadProfile(PROFILE_TXABLE);
    g_fakeBus.state = TWAI_STATE_BUS_OFF;
    CHECK(canBusTxCleared() == false, "bus-off must block TX");
    int stopsBefore = g_fakeBus.stopCalls, startsBefore = g_fakeBus.startCalls;
    canBusTryRecovery();
    CHECK(g_fakeBus.stopCalls == stopsBefore + 1, "recovery must twai_stop()");
    CHECK(g_fakeBus.startCalls == startsBefore + 1, "recovery must twai_start()");
    CHECK(g_fakeBus.state == TWAI_STATE_RUNNING, "fake bus back to RUNNING after restart");
    // Recovery armed a fresh 5s cooldown at millis 1000 -> blocked until 6000.
    CHECK(canBusTxCleared() == false, "post-recovery cooldown must block TX");
    setMillisForTest(6000);
    CHECK(canBusTxCleared() == true, "after recovery + cooldown TX resumes");
}

static void test_err_pass_via_note() {
    printf("== error-passive note blocks then heals\n");
    resetBus();
    loadProfile(PROFILE_TXABLE);
    canBusNoteTxError("error-passive");
    CHECK(canBusTxCleared() == false, "ERR_PASS note must block TX");
    setMillisForTest(6000);
    CHECK(canBusTxCleared() == true, "after cooldown TX resumes");
}

static void test_can_bus_init() {
    printf("== canBusInit\n");
    resetBus();
    canBusInit();   // shim: must not crash
    CHECK(true, "canBusInit completes");
}

int main() {
    test_clean_bus();
    test_listen_only();
    test_tec_elevated_and_cooldown();
    test_bus_off_recovery_cycle();
    test_err_pass_via_note();
    test_can_bus_init();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
