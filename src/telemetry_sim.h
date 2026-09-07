#pragma once

#include <stdint.h>
#include <stdbool.h>

// Bench Telemetry Simulator (Demo Mode)
extern bool isDemoSimMode;

void initTelemetrySim();
void startDemoMode();
void stopDemoMode();
void toggleDemoMode();
void updateTelemetrySimulator();
