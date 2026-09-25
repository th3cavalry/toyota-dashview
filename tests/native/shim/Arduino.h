// Minimal Arduino shim so src/ modules compile natively (host g++).
// Provides only what profile.cpp and core/can_bus.cpp need: PROGMEM,
// millis(), Serial, FreeRTOS delay stubs.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstdarg>

#define PROGMEM

// GPIO numbers (real builds get these from driver/gpio.h).
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16

// millis() backing store. Kept as an inline function-local static so every
// translation unit shares one clock without needing an out-of-line definition.
inline unsigned long& _shimMillis() { static unsigned long m = 1000; return m; }
inline unsigned long millis() { return _shimMillis(); }
inline void setShimMillis(unsigned long m) { _shimMillis() = m; }
inline void setMillisForTest(unsigned long m) { _shimMillis() = m; }

// Serial stub: printf goes to stderr so test logs stay readable.
class ShimSerial {
public:
    void printf(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    void print(const char* s) { fprintf(stderr, "%s", s); }
    void println(const char* s = "") { fprintf(stderr, "%s\n", s); }
};
inline ShimSerial& _shimSerial() { static ShimSerial s; return s; }
#define Serial _shimSerial()

// FreeRTOS stubs
typedef int TickType_t;
inline void vTaskDelay(TickType_t ticks) { (void)ticks; }
#define pdMS_TO_TICKS(x) (x)
