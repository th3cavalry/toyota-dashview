// Native TWAI shim: test-controllable fake bus replacing driver/twai.h.
#ifndef TEST_TWAI_H
#define TEST_TWAI_H

#include <cstdint>
#include <Arduino.h>   // shim: TickType_t, millis

typedef struct {
    uint32_t identifier;
    uint8_t data_length_code;
    uint8_t extd;
    uint8_t rtr;
    uint8_t data[8];
} twai_message_t;

typedef struct {
    int state;
    uint32_t tx_error_counter;
    uint32_t rx_error_counter;
} twai_status_info_t;

typedef struct {
    uint32_t flags;
    uint8_t tx_io;
    uint8_t rx_io;
    uint8_t mode;
} twai_general_config_t;

typedef struct {
    uint32_t brp;
    uint32_t tseg_1;
    uint32_t tseg_2;
    uint32_t sjw;
    uint32_t samp;
} twai_timing_config_t;

typedef struct {
    uint32_t accept_filter;
    uint32_t reject_filter;
} twai_filter_config_t;

typedef enum {
    ESP_OK = 0,
    ESP_FAIL
} esp_err_t;

#define TWAI_MODE_NORMAL 0
#define TWAI_STATE_STOPPED 0
#define TWAI_STATE_RUNNING 1
#define TWAI_STATE_BUS_OFF 2

#define TWAI_ALERT_RX_QUEUE_FULL 0x01
#define TWAI_ALERT_BUS_OFF 0x02
#define TWAI_ALERT_ERR_PASS 0x04

// C++ aggregate construction (C designated initializers are not valid C++).
#define TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin, mode) \
    twai_general_config_t{0, (uint8_t)(tx_pin), (uint8_t)(rx_pin), (uint8_t)(mode)}

#define TWAI_TIMING_CONFIG_500KBITS() \
    twai_timing_config_t{1, 12, 3, 3, 1}

#define TWAI_FILTER_CONFIG_ACCEPT_ALL() \
    twai_filter_config_t{0, 0}

// Test-controllable fake bus. twai_stop() marks a pending restart so tests
// can verify canBusTryRecovery() actually cycles the controller.
struct FakeBus {
    int state = TWAI_STATE_RUNNING;
    uint32_t tec = 0;
    uint32_t rec = 0;
    int transmitCalls = 0;
    int startCalls = 0;
    int stopCalls = 0;
    bool restartPending = false;
    twai_message_t lastTx = {};
    // Install/config recording so tests can assert canBusInit wired the
    // right pins/mode/alerts, and failure flags for negative paths.
    int installCalls = 0;
    int reconfigureCalls = 0;
    uint32_t reconfigureAlerts = 0;
    uint8_t installTxPin = 0, installRxPin = 0, installMode = 0;
    bool installShouldFail = false;
    bool statusInfoShouldFail = false;
};
inline FakeBus g_fakeBus;

inline esp_err_t twai_driver_install(const twai_general_config_t* g, const twai_timing_config_t*, const twai_filter_config_t*) {
    if (g_fakeBus.installShouldFail) return ESP_FAIL;
    g_fakeBus.installCalls++;
    g_fakeBus.installTxPin = g->tx_io;
    g_fakeBus.installRxPin = g->rx_io;
    g_fakeBus.installMode = g->mode;
    return ESP_OK;
}
inline esp_err_t twai_start() {
    g_fakeBus.startCalls++;
    if (g_fakeBus.restartPending) {
        g_fakeBus.restartPending = false;
        g_fakeBus.state = TWAI_STATE_RUNNING;
    }
    return ESP_OK;
}
inline esp_err_t twai_stop() {
    g_fakeBus.stopCalls++;
    g_fakeBus.restartPending = true;
    return ESP_OK;
}
inline esp_err_t twai_reconfigure_alerts(uint32_t alerts, uint32_t*) {
    g_fakeBus.reconfigureCalls++;
    g_fakeBus.reconfigureAlerts = alerts;
    return ESP_OK;
}
inline esp_err_t twai_get_status_info(twai_status_info_t* st) {
    if (g_fakeBus.statusInfoShouldFail) return ESP_FAIL;
    st->state = g_fakeBus.state;
    st->tx_error_counter = g_fakeBus.tec;
    st->rx_error_counter = g_fakeBus.rec;
    return ESP_OK;
}
inline esp_err_t twai_transmit(const twai_message_t* msg, TickType_t) {
    g_fakeBus.lastTx = *msg;
    g_fakeBus.transmitCalls++;
    return ESP_OK;
}

#endif // TEST_TWAI_H
