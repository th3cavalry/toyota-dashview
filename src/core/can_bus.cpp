#include "can_bus.h"
#include <Arduino.h>
#include "driver/twai.h"
#include "profile.h"

// =========================================================================
// CAN driver init + TX failsafe (extracted from main.cpp, milestone #17
// Phase 1). A moving vehicle's bus outranks our gauges: any error activity
// silences OBD polling until TXERROR_COOLDOWN_MS of clean bus have passed.
// =========================================================================
void canBusInit() {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        Serial.printf("[CAN] TWAI driver installed on TX: IO%d, RX: IO%d (Normal 500k Mode).\n", CAN_TX_PIN, CAN_RX_PIN);
    } else {
        Serial.println("[CAN] Failed to install TWAI driver.");
        return;
    }

    // Alert on RX FIFO overrun and bus-off so processCAN() can react instead of
    // silently dropping frames (shorted tap -> bus-off was previously permanent).
    uint32_t alerts = TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_BUS_OFF | TWAI_ALERT_ERR_PASS;
    twai_reconfigure_alerts(alerts, nullptr);

    if (twai_start() == ESP_OK) {
        Serial.println("[CAN] TWAI started successfully.");
    } else {
        Serial.println("[CAN] Failed to start TWAI.");
    }
}

// =========================================================================
// TX failsafe: a moving vehicle's bus outranks our gauges. Any error
// activity (error-passive alert or non-zero TX error counter) silences OBD
// polling; it auto-resumes only after TXERROR_COOLDOWN_MS of clean bus.
// =========================================================================
static const unsigned long TXERROR_COOLDOWN_MS = 5000;
static unsigned long txInhibitUntilMs = 0;
static bool txInhibitLogged = false;

void canBusNoteTxError(const char* reason) {
    txInhibitUntilMs = millis() + TXERROR_COOLDOWN_MS;
    if (!txInhibitLogged) {
        Serial.printf("[CAN-TX] SAFETY: OBD polling paused 5s (%s).\n", reason);
        txInhibitLogged = true;
    }
}

#ifdef UNIT_TEST
void canBusResetFailsafeForTest() {
    txInhibitUntilMs = 0;
    txInhibitLogged = false;
}
#endif

// Polls may only go out when: profile allows TX (not listen-only), the bus
// has been quiet of TX errors for the cooldown window, and the controller is
// error-active with near-zero TX error count.
bool canBusTxCleared() {
    if (isListenOnly()) return false;
    if ((long)(millis() - txInhibitUntilMs) < 0) return false;
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
        if (st.state != TWAI_STATE_RUNNING || st.tx_error_counter > 8 || st.rx_error_counter > 8) {
            canBusNoteTxError("TEC/REC elevated");
            return false;
        }
    }
    if (txInhibitLogged) {
        Serial.println("[CAN-TX] Bus healthy -> OBD polling resumed.");
        txInhibitLogged = false;
    }
    return true;
}

// Restart the TWAI peripheral after a bus-off (recovery requires stop/start).
void canBusTryRecovery() {
    canBusNoteTxError("bus-off recovery");
    Serial.println("[CAN] Bus-off detected -> attempting TWAI recovery...");
    twai_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    if (twai_start() == ESP_OK) {
        Serial.println("[CAN] TWAI restarted after bus-off.");
    } else {
        Serial.println("[CAN] TWAI restart failed (wiring/termination?).");
    }
}
