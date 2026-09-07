#include "telemetry_sim.h"
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "profile.h"

// Forward declaration of legacy vehicle struct from main.cpp
struct TacomaTelemetry {
    char gear[4];
    bool tccLocked;
    int rpm;
    int speedMph;
    float coolantTempC;
    float iatC;
    int throttlePct;
    int engineLoadPct;
    float timingDeg;
    float mafGps;
    float commandedAfr;
    float actualAfr;
    float kclv;
    float knockFB;
};
extern TacomaTelemetry vehicleData;
extern float currentPPS;
extern unsigned long lastCanActivityTime;

bool isDemoSimMode = false;
static unsigned long s_simStartTime = 0;

void initTelemetrySim() {
    isDemoSimMode = false;
}

void startDemoMode() {
    isDemoSimMode = true;
    s_simStartTime = millis();
    Serial.println("[DEMO] Telemetry Simulator Started!");
}

void stopDemoMode() {
    isDemoSimMode = false;
    Serial.println("[DEMO] Telemetry Simulator Stopped.");
}

void toggleDemoMode() {
    if (isDemoSimMode) stopDemoMode();
    else startDemoMode();
}

void updateTelemetrySimulator() {
    if (!isDemoSimMode) return;

    unsigned long now = millis();
    unsigned long elapsed = (now - s_simStartTime) % 16000; // 16-second driving loop
    lastCanActivityTime = now;
    currentPPS = 420.0f + (sinf(now / 1000.0f) * 35.0f);

    if (elapsed < 2000) {
        // Phase 1: Idle at stoplight (0 - 2s)
        strncpy(vehicleData.gear, "D", sizeof(vehicleData.gear) - 1);
        vehicleData.tccLocked = false;
        vehicleData.rpm = 720 + (rand() % 40);
        vehicleData.speedMph = 0;
        vehicleData.throttlePct = 0;
        vehicleData.engineLoadPct = 18;
        vehicleData.commandedAfr = 14.7f;
        vehicleData.actualAfr = 14.7f + ((rand() % 20 - 10) * 0.02f);
        vehicleData.kclv = 21.6f;
        vehicleData.knockFB = 0.0f;
        vehicleData.coolantTempC = 88.0f;
        vehicleData.iatC = 26.0f;
        vehicleData.timingDeg = 12.0f;
        vehicleData.mafGps = 4.2f;

    } else if (elapsed < 9000) {
        // Phase 2: Hard acceleration through gears (2s - 9s)
        float t = (elapsed - 2000) / 7000.0f; // 0.0 -> 1.0
        vehicleData.speedMph = (int)(t * 72.0f);
        vehicleData.throttlePct = 78 + (int)(sinf(t * 12.0f) * 10.0f);
        vehicleData.engineLoadPct = 82;
        vehicleData.commandedAfr = 12.5f;
        vehicleData.actualAfr = 12.4f + ((rand() % 20 - 10) * 0.02f);
        vehicleData.kclv = 21.8f;
        vehicleData.knockFB = (t > 0.4f && t < 0.6f) ? -0.8f : 0.0f;
        vehicleData.coolantTempC = 89.0f + t * 3.0f;
        vehicleData.iatC = 27.0f;
        vehicleData.timingDeg = 16.0f + t * 8.0f;
        vehicleData.mafGps = 45.0f + t * 90.0f;

        // Gear shifts based on speed
        if (vehicleData.speedMph < 18) {
            strncpy(vehicleData.gear, "1", sizeof(vehicleData.gear) - 1);
            vehicleData.rpm = 2000 + (int)((vehicleData.speedMph / 18.0f) * 3200);
            vehicleData.tccLocked = false;
        } else if (vehicleData.speedMph < 36) {
            strncpy(vehicleData.gear, "2", sizeof(vehicleData.gear) - 1);
            vehicleData.rpm = 2400 + (int)(((vehicleData.speedMph - 18) / 18.0f) * 2800);
            vehicleData.tccLocked = false;
        } else if (vehicleData.speedMph < 54) {
            strncpy(vehicleData.gear, "3", sizeof(vehicleData.gear) - 1);
            vehicleData.rpm = 2600 + (int)(((vehicleData.speedMph - 36) / 18.0f) * 2600);
            vehicleData.tccLocked = false;
        } else {
            strncpy(vehicleData.gear, "4", sizeof(vehicleData.gear) - 1);
            vehicleData.rpm = 2800 + (int)(((vehicleData.speedMph - 54) / 18.0f) * 2200);
            vehicleData.tccLocked = false;
        }

    } else if (elapsed < 13000) {
        // Phase 3: Highway Cruising in 6th Gear with Lockup (9s - 13s)
        strncpy(vehicleData.gear, "6", sizeof(vehicleData.gear) - 1);
        vehicleData.tccLocked = true;
        vehicleData.speedMph = 70 + (int)(sinf(now / 800.0f) * 3.0f);
        vehicleData.rpm = 1750 + (int)(sinf(now / 800.0f) * 80.0f);
        vehicleData.throttlePct = 24;
        vehicleData.engineLoadPct = 32;
        vehicleData.commandedAfr = 14.7f;
        vehicleData.actualAfr = 14.7f + ((rand() % 20 - 10) * 0.03f);
        vehicleData.kclv = 22.1f;
        vehicleData.knockFB = 0.0f;
        vehicleData.coolantTempC = 91.0f;
        vehicleData.iatC = 25.0f;
        vehicleData.timingDeg = 24.0f;
        vehicleData.mafGps = 28.0f;

    } else {
        // Phase 4: Deceleration / Coastdown to idle (13s - 16s)
        float t = (elapsed - 13000) / 3000.0f; // 0.0 -> 1.0
        vehicleData.speedMph = (int)((1.0f - t) * 68.0f);
        vehicleData.rpm = 1200 + (int)((1.0f - t) * 800.0f);
        vehicleData.throttlePct = 0;
        vehicleData.engineLoadPct = 14;
        vehicleData.commandedAfr = 14.7f;
        vehicleData.actualAfr = 16.2f; // Lean deceleration fuel cut-off (DFCO)
        vehicleData.tccLocked = (t < 0.5f);
        if (vehicleData.speedMph > 40) strncpy(vehicleData.gear, "5", sizeof(vehicleData.gear) - 1);
        else if (vehicleData.speedMph > 20) strncpy(vehicleData.gear, "3", sizeof(vehicleData.gear) - 1);
        else strncpy(vehicleData.gear, "D", sizeof(vehicleData.gear) - 1);
        vehicleData.kclv = 21.8f;
        vehicleData.knockFB = 0.0f;
        vehicleData.coolantTempC = 90.0f;
        vehicleData.iatC = 26.0f;
        vehicleData.timingDeg = 10.0f;
        vehicleData.mafGps = 6.0f;
    }

    vehicleData.gear[sizeof(vehicleData.gear) - 1] = '\0';
}
