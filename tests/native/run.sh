#!/usr/bin/env bash
# Native unit tests for the profile engine. No ESP hardware needed.
set -euo pipefail
cd "$(dirname "$0")/../.."
ARDUINOJSON=.pio/libdeps/waveshare-touch-43b/ArduinoJson/src
if [ ! -d "$ARDUINOJSON" ]; then
  echo "ArduinoJson libdeps missing — run 'pio run' once first." >&2
  exit 2
fi
mkdir -p tests/native/build
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
  -I tests/native/shim -I src -I "$ARDUINOJSON" \
  tests/native/test_profile.cpp src/profile.cpp \
  -o tests/native/build/test_profile
exec tests/native/build/test_profile
