#ifndef CORE_CAN_BUS_H
#define CORE_CAN_BUS_H

// Onboard CAN transceiver pins (screw terminals, 120R termination switch).
// Guarded: main.cpp may keep its own identical definition.
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
#endif