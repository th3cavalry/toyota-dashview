// Minimal Arduino shim so src/profile.cpp compiles natively (host g++).
// The profile engine only needs PROGMEM and millis() from Arduino.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cmath>

#define PROGMEM

inline unsigned long& _shimMillis() { static unsigned long m = 1000; return m; }
inline unsigned long millis() { return _shimMillis(); }
inline void setShimMillis(unsigned long m) { _shimMillis() = m; }
