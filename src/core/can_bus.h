#ifndef CORE_CAN_BUS_H
#define CORE_CAN_BUS_H

// Onboard CAN transceiver pins (screw terminals, 120R termination switch).
// Guarded: main.cpp may keep its own identical definition.
// Include order: GPIO_NUM_15/16 come from driver/gpio.h on firmware (pulled
// in via Arduino.h); the native test shim's Arduino.h defines them too.
// Include Arduino.h before this header in any new TU.
#ifndef CAN_TX_PIN
#define CAN_TX_PIN GPIO_NUM_15
#endif
#ifndef CAN_RX_PIN
#define CAN_RX_PIN GPIO_NUM_16
#endif

void canBusInit();                    // was initCAN()
bool canBusTxCleared();               // was obdTxCleared()
void canBusTryRecovery();             // was tryCanRecovery()
void canBusNoteTxError(const char* reason); // was noteTxError() — called from processCAN() on ERR_PASS alert

#ifdef UNIT_TEST
// Native-test hook: clears the failsafe cooldown statics between test cases
// (statics persist across cases otherwise, leaking inhibition). Never
// compiled into firmware — only tests/native/run.sh defines UNIT_TEST.
void canBusResetFailsafeForTest();
#endif
#endif
