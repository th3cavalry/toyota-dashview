#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include "driver/twai.h"
#include "version.h"
#include "profile.h"
#include "custom_dash.h"   // Custom Dash API (impl included later, post-palette)
#include "display.h"       // LVGL 9 render core (replaces LovyanGFX)
#include "fonts.h"         // fonts::Font0/2/4/7 -> LVGL Montserrat
#include "splash.h"        // DashView boot splash (canvas-drawn, no PNG decode)
#include "ui.h"            // LVGL UI shell (S2): globals defined in ui.cpp

// Persistent Settings (Flash NVS)
Preferences preferences;
// isDisplayFlipped / backlightEnabled now live in ui.cpp (S2)

// Forward decls (defined with the CH422G / display sections below)
void backlightOn();
void ch422gSetPin(uint8_t bit, bool level);
bool rtcStamp(char* out, size_t len);
void backlightOff();

// =========================================================================
// LVGL 9 Display — single PSRAM framebuffer, scanned through SRAM bounce buffers
// (the flicker fix). canvas is the drawing surface the UI code already uses.
// =========================================================================
extern LVGLCanvas canvas; // drawing alias — same name the UI code already uses

// =========================================================================
// Pin Definitions (Waveshare ESP32-S3-Touch-LCD-4.3B)
// =========================================================================

// Onboard CAN transceiver -> OBD-II (screw terminals, 120R termination switch)
#define CAN_TX_PIN         GPIO_NUM_15
#define CAN_RX_PIN         GPIO_NUM_16

// MicroSD (TF card slot) SPI pins. Chip-select is on the CH422G expander
// (EXIO4), so the SD library gets -1 and CS is driven manually.
#define SD_MOSI_PIN        11
#define SD_SCK_PIN         12
#define SD_MISO_PIN        13
#define SD_CS_PIN          -1

// Shared I2C bus: GT911 touch + CH422G expander + PCF85063 RTC
#define I2C_SDA_PIN        8
#define I2C_SCL_PIN        9
#define TP_INT_PIN         4      // GT911 interrupt (also selects I2C addr at boot)

// CH422G extended-IO bit positions (bit index = EXIO number, per LovyanGFX's
// LGFX_Waveshare_ESP32S3_Touch_LCD_43 preset)
#define EXIO_TP_RST        1
#define EXIO_LCD_BL        2
#define EXIO_LCD_RST       3
#define EXIO_SD_CS         4
#define EXIO_USB_SEL       5

// Screen Auto-Dim Timeout (60 Seconds)
#define SCREEN_TIMEOUT_MS          60000

// =========================================================================
// Wi-Fi Access Point & SavvyCAN Streaming Server
// =========================================================================
const char* WIFI_SSID = "Toyota-DashView";
const char* WIFI_PASS = "dashview123";
#define SAVVYCAN_PORT 23

WiFiServer tcpServer(SAVVYCAN_PORT);
WiFiClient savvyClient;
bool wifiClientConnected = false;
unsigned long wifiStreamedCount = 0;

// =========================================================================
// Selectable PIDs Definition
// =========================================================================
struct DatalogPid {
    const char* idStr;    // Unique key
    const char* label;    // Short UI label
    const char* header;   // CSV header field(s)
    uint8_t mode;         // OBD Mode (0x01, 0x21, 0x00 = Passive CAN)
    uint8_t pid;          // OBD PID
    bool enabled;         // Is selected for datalogging
};

DatalogPid availablePids[] = {
    {"RPM",    "RPM (Eng Speed)",  "RPM",             0x01, 0x0C, true},
    {"SPEED",  "SPEED (MPH)",      "Speed_MPH",       0x01, 0x0D, true},
    {"THR",    "THROTTLE (%)",     "Throttle_Pct",    0x01, 0x11, true},
    {"LOAD",   "LOAD (Eng Load%)", "Engine_Load_Pct", 0x01, 0x04, true},
    {"AFR",    "AFR (Cmd / Act)",  "Cmd_AFR,Act_AFR", 0x01, 0x24, true},
    {"KCLV",   "KCLV (Learned)",   "KCLV",            0x21, 0xA2, true},
    {"KFB",    "KFB (Knock FB)",   "Knock_FB_deg",    0x21, 0xA2, true},
    {"ECT",    "ECT (Coolant \xb0" "C)", "Coolant_C", 0x01, 0x05, true},
    {"GEAR",   "GEAR (Trans/Lock)","Gear,Lockup",     0x00, 0x00, true},
    {"IAT",    "IAT (Intake \xb0" "C)", "IAT_C",      0x01, 0x0F, false},
    {"MAF",    "MAF (Airflow g/s)","MAF_gps",         0x01, 0x10, false},
    {"TIMING", "TIMING (Ign Adv)", "Timing_Adv_deg",  0x01, 0x0E, false}
};
#define PID_COUNT (sizeof(availablePids) / sizeof(availablePids[0]))

// isPidConfigOpen defined in ui.cpp (S2)

// ---- Datalog selection by PROFILE SIGNAL KEY (NVS: "dl_set" + "dl_sel") ----
// Datalog columns, polls, and the picker all follow the active vehicle
// profile; selection is stored by key (indices shift between profiles).
// g_dlSelCount == -1 => no explicit choice yet: every signal is logged.
// If a profile hot-swap leaves zero selected keys present, we fall back to
// logging everything rather than writing an empty CSV.
static char g_dlSel[PROFILE_MAX_SIGNALS][24];
static int  g_dlSelCount  = -1;
static bool g_dlSelLoaded = false;
static char g_dlSelProf[24] = "";   // profile the selection was made against

static void dlSelSave() {
    String s;
    for (int i = 0; i < g_dlSelCount; i++) { if (i) s += ','; s += g_dlSel[i]; }
    preferences.begin("dashview", false);
    preferences.putBool("dl_set", g_dlSelCount >= 0);
    preferences.putString("dl_sel", s);
    preferences.putString("dl_prof", getProfileId());
    preferences.end();
    strncpy(g_dlSelProf, getProfileId(), sizeof(g_dlSelProf) - 1);
}
static void dlSelLoad() {
    if (g_dlSelLoaded) return;
    g_dlSelLoaded = true;
    preferences.begin("dashview", true);
    bool set = preferences.getBool("dl_set", false);
    String s = preferences.getString("dl_sel", "");
    preferences.getString("dl_prof", "").toCharArray(g_dlSelProf, sizeof(g_dlSelProf));
    preferences.end();
    g_dlSelCount = set ? 0 : -1;
    if (!set) return;
    int start = 0;
    while (start < (int)s.length() && g_dlSelCount < PROFILE_MAX_SIGNALS) {
        int comma = s.indexOf(',', start);
        if (comma < 0) comma = s.length();
        if (comma > start) {
            s.substring(start, comma).toCharArray(g_dlSel[g_dlSelCount], sizeof(g_dlSel[0]));
            g_dlSel[g_dlSelCount][sizeof(g_dlSel[0]) - 1] = 0;
            g_dlSelCount++;
        }
        start = comma + 1;
    }
}
static bool dlSelEnabledKey(const char* key) {
    for (int i = 0; i < g_dlSelCount; i++)
        if (!strcmp(g_dlSel[i], key)) return true;
    return false;
}
static void dlSelMaterialize() {  // expand "all" (-1) into explicit current keys
    if (g_dlSelCount >= 0) return;
    g_dlSelCount = 0;
    for (int i = 0; i < getSignalCount() && g_dlSelCount < PROFILE_MAX_SIGNALS; i++) {
        const SignalValue* s = getSignalByIndex(i);
        if (!s) continue;
        strncpy(g_dlSel[g_dlSelCount], s->key, sizeof(g_dlSel[0]) - 1);
        g_dlSel[g_dlSelCount][sizeof(g_dlSel[0]) - 1] = 0;
        g_dlSelCount++;
    }
}
static void dlSelToggleSig(int sigIdx) {
    const SignalValue* s = getSignalByIndex(sigIdx);
    if (!s) return;
    dlSelLoad();
    dlSelMaterialize();
    for (int i = 0; i < g_dlSelCount; i++) {
        if (strcmp(g_dlSel[i], s->key)) continue;
        for (int j = i; j < g_dlSelCount - 1; j++) strcpy(g_dlSel[j], g_dlSel[j + 1]);
        g_dlSelCount--;
        dlSelSave();
        return;
    }
    if (g_dlSelCount < PROFILE_MAX_SIGNALS) {
        strncpy(g_dlSel[g_dlSelCount], s->key, sizeof(g_dlSel[0]) - 1);
        g_dlSel[g_dlSelCount][sizeof(g_dlSel[0]) - 1] = 0;
        g_dlSelCount++;
    }
    dlSelSave();
}
static void dlSelAll()  { dlSelLoad(); g_dlSelCount = -1; dlSelSave(); }
static void dlSelNone() { dlSelLoad(); g_dlSelCount = 0;  dlSelSave(); }
static bool dlSelAnyInProfile() {  // does the explicit selection hit the profile?
    for (int i = 0; i < getSignalCount(); i++) {
        const SignalValue* s = getSignalByIndex(i);
        if (s && dlSelEnabledKey(s->key)) return true;
    }
    return false;
}
// Master question for column/poll/UI: is this signal logged right now?
// A stored selection made against a DIFFERENT profile that matches nothing
// here (e.g. tacoma picks, then swap to a Honda) falls back to logging
// everything; an explicit selection on the current profile is always honored,
// including the empty one (NONE).
static bool dlLogThis(const char* key) {
    dlSelLoad();
    if (g_dlSelCount < 0) return true;
    if (dlSelEnabledKey(key)) return true;
    if (strcmp(g_dlSelProf, getProfileId()) != 0 && !dlSelAnyInProfile()) return true;
    return false;
}
// isRawSnifferModalOpen / isSnifferPaused / isBootSplashActive defined in ui.cpp (S2)
bool isSnifferPaused = false;        // Freeze live frame view for inspection
bool isBootSplashActive = true;      // Keep boot splash until screen tapped or engine starts (RPM > 0)

// =========================================================================
// Logging Engine & State Management (Mutually Exclusive)
// =========================================================================
enum LoggingMode {
    LOG_IDLE    = 0,
    LOG_CANBUS  = 1, // Raw CAN frames (canbus_XXXX.csv)
    LOG_DATALOG = 2  // Decoded PID parameters (datalog_XXXX.csv)
};

LoggingMode currentLogMode = LOG_IDLE;
char currentLogFileName[40] = "None";
unsigned long logStartTime = 0;
unsigned long logEntryCount = 0;
unsigned long lastLogFlushTime = 0;
unsigned long lastDatalogSampleTime = 0;

// Hardware & Runtime Instances
SPIClass sdSPI(HSPI);
File activeLogFile;
bool sdMounted = false;
unsigned long packetCount = 0;
unsigned long rxOverflowCount = 0; // frames dropped due to RX queue overrun
unsigned long lastCanActivityTime = 0;
unsigned long lastSdRetryTime = 0;
unsigned long lastObdQueryTime = 0;
unsigned long lastUserActivityTime = 0;
bool isScreenDimmed = false;
uint8_t obdQueryIndex = 0;
unsigned long ppsCount = 0;
float currentPPS = 0;
unsigned long lastPPSCheck = 0;
unsigned long lastDisplayUpdate = 0;

// DisplayScreen enum + currentScreen now live in ui.h/ui.cpp (S2)

// Swipe Gesture Detection State
bool wasTouched = false;
int touchStartX = 0;
int touchStartY = 0;
int touchLastX = 0;
int touchLastY = 0;
unsigned long touchStartTime = 0;

// Live Vehicle Telemetry — TacomaTelemetry + vehicleData now live in ui.h/ui.cpp (S2)

// Ring buffer for CAN Sniffer View

// Ring buffer for CAN Sniffer View
struct RecentFrame {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
    unsigned long timestamp;
};
#define SNIFFER_HISTORY_SIZE 16
RecentFrame snifferHistory[SNIFFER_HISTORY_SIZE];
int snifferHead = 0;

// =========================================================================
// Wi-Fi GVRET Streaming to SavvyCAN
// =========================================================================
void initWiFiStreaming() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    IPAddress IP = WiFi.softAPIP();
    tcpServer.begin();
    tcpServer.setNoDelay(true);
    Serial.printf("[WIFI] Access Point Started: SSID '%s' (Pass: '%s')\n", WIFI_SSID, WIFI_PASS);
    Serial.printf("[WIFI] SavvyCAN Server Listening at %s:%d\n", IP.toString().c_str(), SAVVYCAN_PORT);
}

void streamFrameToSavvyCAN(const twai_message_t &msg) {
    if (!savvyClient || !savvyClient.connected()) {
        return;
    }

    uint8_t buffer[20];
    buffer[0] = 0xF1;
    buffer[1] = 0x00;

    uint32_t nowMicros = micros();
    buffer[2] = (uint8_t)(nowMicros & 0xFF);
    buffer[3] = (uint8_t)((nowMicros >> 8) & 0xFF);
    buffer[4] = (uint8_t)((nowMicros >> 16) & 0xFF);
    buffer[5] = (uint8_t)((nowMicros >> 24) & 0xFF);

    uint32_t id = msg.identifier;
    if (msg.extd) id |= 0x80000000;
    buffer[6] = (uint8_t)(id & 0xFF);
    buffer[7] = (uint8_t)((id >> 8) & 0xFF);
    buffer[8] = (uint8_t)((id >> 16) & 0xFF);
    buffer[9] = (uint8_t)((id >> 24) & 0xFF);

    buffer[10] = msg.data_length_code & 0x0F;

    for (int i = 0; i < msg.data_length_code && i < 8; i++) {
        buffer[11 + i] = msg.data[i];
    }
    buffer[11 + msg.data_length_code] = 0xF2;

    savvyClient.write(buffer, 12 + msg.data_length_code);
    wifiStreamedCount++;
}

void handleWiFiClients() {
    if (tcpServer.hasClient()) {
        if (!savvyClient || !savvyClient.connected()) {
            savvyClient = tcpServer.accept();
            savvyClient.setNoDelay(true);
            wifiClientConnected = true;
            Serial.println("[WIFI] >>> SavvyCAN Client Connected over Wi-Fi!");
        }
    }

    if (savvyClient && savvyClient.connected() && savvyClient.available() > 0) {
        uint8_t cmd = savvyClient.read();
        if (cmd == 0xE7) {
            uint8_t sub = savvyClient.read();
            if (sub == 0x00) {
                uint8_t resp[] = {0xE7, 0x01, 0x20, 0x01};
                savvyClient.write(resp, 4);
            }
        }
    }
}

// =========================================================================
// CAN Driver Initialization & Toyota OBD Queries
// =========================================================================
void initCAN() {
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

static void noteTxError(const char* reason) {
    txInhibitUntilMs = millis() + TXERROR_COOLDOWN_MS;
    if (!txInhibitLogged) {
        Serial.printf("[CAN-TX] SAFETY: OBD polling paused 5s (%s).\n", reason);
        txInhibitLogged = true;
    }
}

// Polls may only go out when: profile allows TX (not listen-only), the bus
// has been quiet of TX errors for the cooldown window, and the controller is
// error-active with near-zero TX error count.
static bool obdTxCleared() {
    if (isListenOnly()) return false;
    if ((long)(millis() - txInhibitUntilMs) < 0) return false;
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
        if (st.state != TWAI_STATE_RUNNING || st.tx_error_counter > 8 || st.rx_error_counter > 8) {
            noteTxError("TEC/REC elevated");
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
void tryCanRecovery() {
    noteTxError("bus-off recovery");
    Serial.println("[CAN] Bus-off detected -> attempting TWAI recovery...");
    twai_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    if (twai_start() == ESP_OK) {
        Serial.println("[CAN] TWAI restarted after bus-off.");
    } else {
        Serial.println("[CAN] TWAI restart failed (wiring/termination?).");
    }
}

// Query only what the log needs (profile signals marked for logging, mapped
// to their poll ids) plus anything the Custom Dash shows.
void sendToyotaObdQueries() {
    uint8_t qMode[16];
    uint8_t qPid[16];
    int queryCount = 0;

    // Profile-driven polls: every obd_poll signal the logger has selected.
    for (int i = 0; i < getSignalCount(); i++) {
        const SignalValue* s = getSignalByIndex(i);
        if (!s || !dlLogThis(s->key)) continue;
        uint8_t m, p;
        if (!profileSignalPollId(s->key, &m, &p)) continue;  // broadcast = no poll
        bool exists = false;
        for (int q = 0; q < queryCount; q++)
            if (qMode[q] == m && qPid[q] == p) { exists = true; break; }
        if (!exists && queryCount < 16) { qMode[queryCount] = m; qPid[queryCount] = p; queryCount++; }
    }

    // Gauges on the Custom Dash poll their PIDs even when datalogging off
    cdAppendQueries(qMode, qPid, queryCount, 16);

    if (queryCount == 0) return;

    obdQueryIndex = obdQueryIndex % queryCount;

    twai_message_t queryMsg;
    queryMsg.identifier = getReqId();
    queryMsg.extd = 0;
    queryMsg.rtr = 0;
    queryMsg.data_length_code = 8;
    memset(queryMsg.data, 0, 8);

    queryMsg.data[0] = 0x02;
    queryMsg.data[1] = qMode[obdQueryIndex];
    queryMsg.data[2] = qPid[obdQueryIndex];

    twai_transmit(&queryMsg, 0);
    obdQueryIndex = (obdQueryIndex + 1) % queryCount;
}

// =========================================================================
// ISO-TP (ISO 15765-2) Reassembly for OBD Responses (0x7E8 -> 0x7E0)
// Toyota Mode $21 responses (e.g. 21 A2 KCLV/KFB) are frequently multi-frame:
// a First Frame must be answered with a Flow Control CTS from 0x7E0, then
// Consecutive Frames are concatenated until the FF length is satisfied.
// =========================================================================
uint8_t  isotpBuf[64];
uint16_t isotpTotal = 0;     // payload bytes announced by First Frame
uint16_t isotpReceived = 0;  // bytes accumulated so far
bool     isotpActive = false;
uint8_t  isotpNextSeq = 1;   // expected CF sequence nibble (wraps after 0)
unsigned long isotpStartedAt = 0;

void sendIsotpFlowControl() {
    twai_message_t fc = {};
    fc.identifier = getReqId();
    fc.extd = 0;
    fc.rtr = 0;
    fc.data_length_code = 8;
    memset(fc.data, 0, 8);
    fc.data[0] = 0x30; // Flow Control, Continue To Send
    fc.data[1] = 0x00; // BS = 0 (block size unlimited)
    fc.data[2] = 0x00; // STmin = 0
    twai_transmit(&fc, 0);
}

// Decode a complete OBD payload: p[0]=mode(+0x40), p[1]=PID, p[2..]=data bytes
void decodeObdPayload(const uint8_t* p, uint16_t len) {
    if (len < 2) return;
    // Universal vehicle profile OBD response decode
    onObdPollResponse(p[0], p[1], &p[2], len >= 2 ? (len - 2) : 0, millis());
    if (p[0] == 0x61 && p[1] == 0xA2 && len >= 4) {
        float rawKclv = p[2] * 0.1f;
        if (rawKclv >= 10.0f && rawKclv <= 30.0f) {
            vehicleData.kclv = rawKclv;
        }
        vehicleData.knockFB = (int8_t)p[3] * 0.1f;
    }
    else if (p[0] == 0x41) {
        if (p[1] == 0x04 && len >= 3) {
            vehicleData.engineLoadPct = (p[2] * 100) / 255;
        }
        else if (p[1] == 0x05 && len >= 3) {
            vehicleData.coolantTempC = (int)p[2] - 40;
        }
        else if (p[1] == 0x0F && len >= 3) {
            vehicleData.iatC = (int)p[2] - 40;
        }
        else if (p[1] == 0x10 && len >= 4) {
            vehicleData.mafGps = ((p[2] << 8) | p[3]) / 100.0f;
        }
        else if (p[1] == 0x0E && len >= 3) {
            vehicleData.timingDeg = ((float)p[2] / 2.0f) - 64.0f;
        }
        else if (p[1] == 0x24 && len >= 4) {
            float lambda = (float)((p[2] << 8) | p[3]) / 32768.0f;
            if (lambda > 0.5f && lambda < 2.0f) {
                vehicleData.actualAfr = lambda * 14.7f;
            }
        }
        else if (p[1] == 0x44 && len >= 4) {
            float lambdaCmd = (float)((p[2] << 8) | p[3]) / 32768.0f;
            if (lambdaCmd > 0.5f && lambdaCmd < 2.0f) {
                vehicleData.commandedAfr = lambdaCmd * 14.7f;
            }
        }
    }
}

// ISO-TP state machine for 0x7E8 frames. Returns true when the frame is consumed.
bool handleObdIsoTp(const twai_message_t &msg) {
    uint8_t pciType = msg.data[0] >> 4;

    if (pciType == 0x0) { // Single Frame
        uint8_t len = msg.data[0] & 0x0F;
        isotpActive = false;
        if (len > 0 && len <= msg.data_length_code - 1) {
            decodeObdPayload(&msg.data[1], len);
        }
        return true;
    }
    if (pciType == 0x1) { // First Frame
        isotpTotal = ((uint16_t)(msg.data[0] & 0x0F) << 8) | msg.data[1];
        if (isotpTotal == 0 || isotpTotal > sizeof(isotpBuf)) {
            isotpActive = false;
            return true;
        }
        memcpy(isotpBuf, &msg.data[2], msg.data_length_code - 2);
        isotpReceived = msg.data_length_code - 2;
        isotpNextSeq = 1;
        isotpActive = true;
        isotpStartedAt = millis();
        sendIsotpFlowControl();
        return true;
    }
    if (pciType == 0x2 && isotpActive) { // Consecutive Frame
        // Drop stale sessions (>1 s) and out-of-order CFs
        if (millis() - isotpStartedAt > 1000 || (msg.data[0] & 0x0F) != isotpNextSeq) {
            isotpActive = false;
            return true;
        }
        uint16_t chunk = msg.data_length_code - 1;
        if (isotpReceived + chunk > isotpTotal) chunk = isotpTotal - isotpReceived;
        memcpy(&isotpBuf[isotpReceived], &msg.data[1], chunk);
        isotpReceived += chunk;
        isotpNextSeq = (isotpNextSeq + 1) & 0x0F;
        if (isotpReceived >= isotpTotal) {
            isotpActive = false;
            decodeObdPayload(isotpBuf, isotpTotal);
        }
        return true;
    }
    return false;
}

void decodeTacomaFrame(const twai_message_t &msg) {
    if (msg.identifier == getRespId() && msg.data_length_code >= 1) {
        handleObdIsoTp(msg);
    }
    // Universal vehicle profile decode
    onBroadcastFrame(msg.identifier, msg.data, msg.data_length_code, millis());
    if (msg.identifier == 0x0B4 && msg.data_length_code >= 8) {
        // NOTE: scale factor unverified against the Toyota signal spec (0x0B4
        // wheel-speed messages are commonly 0.05625/0.0625 km/h per bit).
        // Calibrate against a known-speed log before trusting this reading.
        uint16_t rawSpeed = (msg.data[5] << 8) | msg.data[6];
        vehicleData.speedMph = (rawSpeed * 0.621371f) / 100.0f;
    }
    else if (msg.identifier == 0x3BC && msg.data_length_code >= 5) {
        uint8_t lever = msg.data[0];
        uint8_t rawGear = msg.data[2] & 0x0F;
        vehicleData.tccLocked = (msg.data[3] & 0x80) || (msg.data[4] & 0x01);

        if (lever == 0x00) {
            strcpy(vehicleData.gear, "P");
        } else if (lever == 0x01) {
            strcpy(vehicleData.gear, "R");
        } else if (lever == 0x02) {
            strcpy(vehicleData.gear, "N");
        } else {
            if (rawGear >= 1 && rawGear <= 6) {
                snprintf(vehicleData.gear, sizeof(vehicleData.gear), "%d", rawGear);
            } else {
                strcpy(vehicleData.gear, "D");
            }
        }
    }
    else if (msg.identifier == 0x2C4 && msg.data_length_code >= 8) {
        vehicleData.rpm = ((msg.data[0] << 8) | msg.data[1]) / 4;
        vehicleData.throttlePct = (msg.data[4] * 100) / 255;
        vehicleData.engineLoadPct = (msg.data[2] * 100) / 255;
    }
}

void recordSnifferFrame(const twai_message_t &msg) {
    if (isSnifferPaused) return; // Freeze terminal buffer when paused
    snifferHistory[snifferHead].id = msg.identifier;
    snifferHistory[snifferHead].dlc = msg.data_length_code;
    snifferHistory[snifferHead].timestamp = millis();
    for (int i = 0; i < msg.data_length_code && i < 8; i++) {
        snifferHistory[snifferHead].data[i] = msg.data[i];
    }
    snifferHead = (snifferHead + 1) % SNIFFER_HISTORY_SIZE;
}

// =========================================================================
// MicroSD Card Setup & Non-Overwriting File Generator
// =========================================================================
bool mountSD() {
    // SD chip-select is wired to CH422G EXIO4, not a GPIO. Waveshare's own
    // bring-up holds CS asserted permanently (the card is the only device on
    // this SPI bus) and hands the SD stack ss = -1, which makes its internal
    // digitalWrite(pin) calls no-ops.
    ch422gSetPin(EXIO_SD_CS, false);
    ch422gSetPin(EXIO_USB_SEL, false); // keep FSUSB mux routing GPIO19/20 (not needed for SPI, matches demo)
    sdSPI.setHwCs(false);
    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, -1);
    if (SD.begin(-1, sdSPI, 20000000)) {
        sdMounted = true;
        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Serial.printf("[SD] MicroSD Mounted! Card size: %llu MB\n", cardSize);
        return true;
    }
    sdMounted = false;
    return false;
}

String generateUniqueFileName(const char* prefix) {
    if (!sdMounted && !mountSD()) {
        return "";
    }
    char filename[36];
    // Counter lives in NVS: stat()-ing up to 9999 candidate names froze the UI
    // for seconds on cards holding thousands of old logs.
    preferences.begin("dashview", false);
    uint16_t nextIdx = preferences.getUShort(prefix, 1);
    String path;
    for (int attempt = 0; attempt < 50; attempt++) {
        snprintf(filename, sizeof(filename), "/%s_%04d.csv", prefix, nextIdx);
        if (!SD.exists(filename)) break;
        nextIdx++;
    }
    preferences.putUShort(prefix, nextIdx + 1);
    preferences.end();
    path = String(filename);
    return path;
}

// Count active PIDs (profile signals the logger will write)
int getActivePidCount() {
    int count = 0;
    for (int i = 0; i < getSignalCount(); i++) {
        const SignalValue* s = getSignalByIndex(i);
        if (s && dlLogThis(s->key)) count++;
    }
    return count;
}

// =========================================================================
// Start / Stop Functions for Mutually Exclusive Loggers
// =========================================================================
void stopActiveLogger() {
    if (currentLogMode != LOG_IDLE && activeLogFile) {
        activeLogFile.flush();
        activeLogFile.close();
        Serial.printf("[LOGGER] Stopped & Saved: %s (Entries: %lu)\n", currentLogFileName, logEntryCount);
        currentLogMode = LOG_IDLE;
        strcpy(currentLogFileName, "None");
        logEntryCount = 0;
    }
}

bool startCanbusLogger() {
    if (currentLogMode != LOG_IDLE) {
        Serial.println("[LOGGER] Cannot start CAN Logger: Another log session is active!");
        return false;
    }

    String path = generateUniqueFileName("canbus");
    if (path.length() == 0) {
        Serial.println("[LOGGER] Failed to create canbus file: SD card not available.");
        return false;
    }

    activeLogFile = SD.open(path.c_str(), FILE_WRITE);
    if (activeLogFile) {
        char stamp[24];
        activeLogFile.printf("# RTC: %s (fallback = millis since boot)\n",
                             rtcStamp(stamp, sizeof(stamp)) ? stamp : "unset");
        activeLogFile.println("Timestamp_ms,CAN_ID,Ext,DLC,Data");
        activeLogFile.flush();
        currentLogMode = LOG_CANBUS;
        strncpy(currentLogFileName, path.c_str(), sizeof(currentLogFileName));
        logStartTime = millis();
        logEntryCount = 0;
        lastLogFlushTime = millis();
        Serial.printf("[LOGGER] >>> Started CAN Logger: %s\n", currentLogFileName);
        return true;
    }
    return false;
}

bool startDataLogger() {
    if (currentLogMode != LOG_IDLE) {
        Serial.println("[LOGGER] Cannot start Datalogger: Another log session is active!");
        return false;
    }

    String path = generateUniqueFileName("datalog");
    if (path.length() == 0) {
        Serial.println("[LOGGER] Failed to create datalog file: SD card not available.");
        return false;
    }

    activeLogFile = SD.open(path.c_str(), FILE_WRITE);
    if (activeLogFile) {
        char stamp[24];
        activeLogFile.printf("# RTC: %s (fallback = millis since boot)\n",
                             rtcStamp(stamp, sizeof(stamp)) ? stamp : "unset");
        // CSV columns = the profile signals selected for logging, in profile
        // order, so any vehicle profile's signals land in the file untouched.
        String header = "Timestamp_ms";
        for (int i = 0; i < getSignalCount(); i++) {
            const SignalValue* s = getSignalByIndex(i);
            if (s && dlLogThis(s->key)) { header += ','; header += s->key; }
        }
        activeLogFile.println(header);
        activeLogFile.flush();

        currentLogMode = LOG_DATALOG;
        strncpy(currentLogFileName, path.c_str(), sizeof(currentLogFileName));
        logStartTime = millis();
        logEntryCount = 0;
        lastLogFlushTime = millis();
        lastDatalogSampleTime = millis();
        Serial.printf("[LOGGER] >>> Started PID Datalogger: %s with %d active PIDs\n", currentLogFileName, getActivePidCount());
        return true;
    }
    return false;
}

// =========================================================================
// Display Power / Auto-Dimming Control
// The 4.3B backlight is a digital line on the CH422G expander (no PWM), so
// auto-dim = full backlight off; the GT911 stays powered and wakes the panel.
// =========================================================================
void wakeScreen() {
    lastUserActivityTime = millis();
    if (isScreenDimmed) {
        isScreenDimmed = false;
        backlightOn();
        Serial.println("[DISPLAY] Backlight restored.");
    }
}

void dimScreen() {
    if (!isScreenDimmed) {
        isScreenDimmed = true;
        backlightOff();
        Serial.println("[DISPLAY] Backlight off (60s idle; touch or traffic wakes).");
    }
}

// =========================================================================
// Settings Persistence (NVS Flash)
// =========================================================================
void loadSettings() {
    preferences.begin("dashview", true);
    isDisplayFlipped = preferences.getBool("flip180", false);
    backlightEnabled = preferences.getBool("bl_on", true);
    preferences.end();
    Serial.printf("[SETTINGS] Loaded: Orientation=%s, Backlight=%s\n",
                  isDisplayFlipped ? "180-DEG FLIPPED" : "NORMAL",
                  backlightEnabled ? "ON" : "OFF");
}

void saveDisplayFlipSetting(bool flip) {
    isDisplayFlipped = flip;
    preferences.begin("dashview", false);
    preferences.putBool("flip180", isDisplayFlipped);
    preferences.end();
    // RGB panels rotate in software inside the PSRAM framebuffer; rot 2 (180)
    // keeps the 800x480 logical size. Touch is mapped back in pollTouch().
    canvas.setRotation(isDisplayFlipped ? 2 : 0);
    Serial.printf("[SETTINGS] Display Orientation changed to: %s\n", isDisplayFlipped ? "FLIPPED 180 (INVERTED)" : "STANDARD (NORMAL)");
}

void saveBacklightSetting(bool on) {
    backlightEnabled = on;
    preferences.begin("dashview", false);
    preferences.putBool("bl_on", backlightEnabled);
    preferences.end();
    if (backlightEnabled && !isScreenDimmed) backlightOn();
    else if (!backlightEnabled) backlightOff();
    Serial.printf("[SETTINGS] Backlight set to: %s\n", backlightEnabled ? "ON" : "OFF");
}

// =========================================================================
// CH422G IO Expander (backlight, SD chip-select, touch/LCD resets)
// Datasheet registers: WR-SET 0x48, WR-OC 0x46, WR-IO 0x70 (8-bit I2C cmds)
// =========================================================================
// CH422G protocol quirk: the command is carried in the I2C ADDRESS byte
// (datasheet 8-bit codes 0x48/0x46/0x70, i.e. 7-bit 0x24/0x23/0x38) and the
// payload is a single data byte. This matches Waveshare's esp_io_expander_ch422g.
#define CH422G_ADDR_WR_SET 0x24  // 0x48>>1: mode/config
#define CH422G_ADDR_WR_OC  0x23  // 0x46>>1: EXIO8-11 config
#define CH422G_ADDR_WR_IO  0x38  // 0x70>>1: EXIO1-7 output data
#define CH422G_SET_IO_OE   0x01  // EXIO1-7 as outputs
                               // bit2 (OD_EN) stays 0 => push-pull

static uint8_t ch422gOutValue = 0xFF; // EXIO bit state (bit0 = EXIO1)

static bool ch422gWriteCmd(uint8_t addr7, uint8_t value) {
    Wire.beginTransmission(addr7);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool initCh422g() {
    // EXIO1-7 push-pull outputs (OD bit cleared), all lines high so the
    // active-low LCD/TP resets start released.
    if (!ch422gWriteCmd(CH422G_ADDR_WR_SET, CH422G_SET_IO_OE)) {
        Serial.println("[CH422G] Expander not responding (addr 0x24)!");
        return false;
    }
    ch422gWriteCmd(CH422G_ADDR_WR_OC, 0x0F); // EXIO8-11 push-pull
    ch422gOutValue = 0xFF;
    ch422gWriteCmd(CH422G_ADDR_WR_IO, ch422gOutValue);
    delay(10);
    Serial.println("[CH422G] IO expander ready (addr 0x24).");
    return true;
}

void ch422gSetPin(uint8_t bit, bool level) {
    if (level) ch422gOutValue |= (1 << bit);
    else       ch422gOutValue &= ~(1 << bit);
    ch422gWriteCmd(CH422G_ADDR_WR_IO, ch422gOutValue);
}

// Backlight on the 4.3B is wired to EXIO3 (digital only — the CH422G has no
// PWM, so Waveshare's own firmware also treats it strictly on/off).
void backlightOn()  { ch422gSetPin(EXIO_LCD_BL, true); }
void backlightOff() { ch422gSetPin(EXIO_LCD_BL, false); }

// =========================================================================
// PCF85063 RTC (I2C 0x51) — real-time timestamps for CSV logs
// =========================================================================
#define RTC_I2C_ADDR 0x51
struct RtcTime { uint16_t year; uint8_t month, day, hour, minute, second; bool valid; };

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return (d / 10) << 4 | (d % 10); }

bool rtcWriteBytes(uint8_t reg, const uint8_t* data, size_t len) {
    Wire.beginTransmission(RTC_I2C_ADDR);
    Wire.write(reg);
    Wire.write(data, len);
    return Wire.endTransmission() == 0;
}

void initRtc() {
    uint8_t ctrl1 = 0x00; // stop no clocks, normal mode
    rtcWriteBytes(0x00, &ctrl1, 1);

    uint8_t regs[7] = {0};
    Wire.beginTransmission(RTC_I2C_ADDR);
    Wire.write(0x04);
    if (Wire.endTransmission(false) != 0 ||
        Wire.requestFrom((uint16_t)RTC_I2C_ADDR, (uint8_t)7) != 7) {
        Serial.println("[RTC] PCF85063 not found at 0x51 — logs use millis only.");
        return;
    }
    Wire.readBytes(regs, 7);

    // OS bit in ctrl2-style status or invalid BCD means the RTC lost power:
    // seed it with the firmware build time.
    uint8_t sec = bcd2dec(regs[0] & 0x7F);
    bool invalid = (regs[0] & 0x80) || sec > 59 || bcd2dec(regs[1]) > 59 ||
                   bcd2dec(regs[2]) > 23 || 2000 + bcd2dec(regs[6]) < 2024;
    if (invalid) {
        // Seed: __DATE__ "Sep  3 2026"-style + __TIME__ build clock
        int mon = 1, day = 1, year = 2000;
        const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char mon3[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
        const char* mp = strstr(months, mon3);
        if (mp) mon = (int)(mp - months) / 3 + 1;
        day  = (__DATE__[4] == ' ') ? __DATE__[5] - '0' : (__DATE__[4] - '0') * 10 + (__DATE__[5] - '0');
        year = 2000 + ( __DATE__[9] - '0') * 10 + (__DATE__[10] - '0');
        uint8_t t[] = {
            0, 0, 0, // sec min hour (recovered from __TIME__ below)
            dec2bcd((uint8_t)day), 0, dec2bcd((uint8_t)mon),
            (uint8_t)(year - 2000)
        };
        t[2] = dec2bcd((uint8_t)((__TIME__[0] - '0') * 10 + (__TIME__[1] - '0')));
        t[1] = dec2bcd((uint8_t)((__TIME__[3] - '0') * 10 + (__TIME__[4] - '0')));
        t[0] = dec2bcd((uint8_t)((__TIME__[6] - '0') * 10 + (__TIME__[7] - '0')));
        rtcWriteBytes(0x04, t, 7);
        Serial.println("[RTC] Clock lost power — seeded with build time.");
    }
    Serial.println("[RTC] PCF85063 online.");
}

RtcTime readRtc() {
    RtcTime t{};
    uint8_t regs[7] = {0};
    Wire.beginTransmission(RTC_I2C_ADDR);
    Wire.write(0x04);
    if (Wire.endTransmission(false) != 0 ||
        Wire.requestFrom((uint16_t)RTC_I2C_ADDR, (uint8_t)7) != 7) {
        return t;
    }
    Wire.readBytes(regs, 7);
    if (regs[0] & 0x80) return t; // oscillator stopped
    t.second = bcd2dec(regs[0] & 0x7F);
    t.minute = bcd2dec(regs[1] & 0x7F);
    t.hour   = bcd2dec(regs[2] & 0x3F);
    t.day    = bcd2dec(regs[3] & 0x3F);
    t.month  = bcd2dec(regs[5] & 0x1F);
    t.year   = 2000 + bcd2dec(regs[6]);
    t.valid  = (t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31);
    return t;
}

// Fills 'out' with "YYYY-MM-DD HH:MM:SS" when the RTC is trustworthy.
bool rtcStamp(char* out, size_t len) {
    RtcTime t = readRtc();
    if (!t.valid) return false;
    snprintf(out, len, "%04u-%02u-%02u %02u:%02u:%02u", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return true;
}

// =========================================================================
// GT911 Capacitive Touch (I2C 0x5D/0x14, INT on GPIO4, reset on EXIO1)
// Coordinates are native panel pixels (0..799 x 0..479) — no transform.
// =========================================================================
// =========================================================================
// GT911 Capacitive Touch — init lives in display.cpp (11a218d: track reg is
// 0x814F, not 0x814E). The old monolith block here died with the monolith:
// old 0x814E track reg + no EXIO1 reset pulse (addr pin latched at boot).
// =========================================================================

void initGt911Touch() {
    // Reset pulse via the expander; INT state during reset latches the addr.
    ch422gSetPin(EXIO_TP_RST, false);
    delay(20);
    ch422gSetPin(EXIO_TP_RST, true);
    delay(100);
    displayTouchInit();
}
#undef C_DARK_BG
#undef C_CARD_BG
#undef C_CARD_BORDER
#undef C_CARD_INNER
#undef C_TRD_ORANGE
#undef C_TRD_RED
#undef C_TRD_BURGUNDY
#undef C_TEXT_WHITE
#undef C_TEXT_MUTED
#undef C_TEXT_CYAN
#undef C_GREEN_OK
#undef C_GOLD_LOCK

#define C_DARK_BG       canvas.color565(11, 14, 20)    // #0B0E14 Deep Obsidian
#define C_CARD_BG       canvas.color565(18, 23, 34)    // #121722 Carbon Dark Slate
#define C_CARD_BORDER   canvas.color565(36, 46, 64)    // #242E40 Refined Card Border
#define C_CARD_INNER    canvas.color565(12, 16, 23)    // #0C1017 Recessed Well Fill
#define C_CARD_HI       canvas.color565(26, 34, 48)    // #1A2230 Highlighted Card Fill
#define C_TRD_ORANGE    canvas.color565(255, 130, 20)  // #FF8214 TRD Heritage Orange
#define C_TRD_RED       canvas.color565(235, 18, 38)   // #EB1226 TRD Vibrant Red
#define C_TRD_BURGUNDY  canvas.color565(140, 15, 25)   // #8C0F19 TRD Deep Burgundy
#define C_TEXT_WHITE    TFT_WHITE                      // Pure Crisp White
#define C_TEXT_MUTED    canvas.color565(120, 135, 155) // #78879B Cool Slate Gray
#define C_TEXT_CYAN     canvas.color565(0, 215, 255)   // #00D7FF Ice Cyan Telemetry
#define C_GREEN_OK      canvas.color565(20, 205, 115)  // #14CD73 Nominal Green
#define C_GOLD_LOCK     canvas.color565(255, 195, 0)   // #FFC300 TCC Lock Gold

// Custom Dash implementation (uses the palette + globals above)
void drawHeaderBar(const char* title, bool forceFull = true);
void drawBottomNavBar(bool forceFull = false);
#include "custom_dash.inl"

// =========================================================================
// 800x480 UI Layout Constants (Waveshare 4.3B)
// =========================================================================
#define UI_W 800
#define UI_H 480
#define UI_HEADER_H 44
#define UI_NAVBAR_H 40
#define UI_CONTENT_W (UI_W - 24)   // side margin 12+12

// Full-Color 800x480 UI Page Renderers (TRD Motorsport Dark Edition)
// =========================================================================

void drawHeaderBar(const char* title, bool forceFull) {
    canvas.setTextDatum(0);
    if (forceFull) {
        // Deep dark header bar background
        canvas.fillRect(0, 0, UI_W, UI_HEADER_H, canvas.color565(12, 15, 22));

        // TRD Heritage Tri-Color Mini Stripes (Top Left)
        canvas.fillRect(0, 0, 6, UI_HEADER_H, C_TRD_ORANGE);
        canvas.fillRect(6, 0, 6, UI_HEADER_H, C_TRD_RED);
        canvas.fillRect(12, 0, 6, UI_HEADER_H, C_TRD_BURGUNDY);

        // Screen Title in clean Montserrat 20
        canvas.setTextColor(C_TEXT_WHITE, canvas.color565(12, 15, 22));
        canvas.setFont(fonts::Font4);
        canvas.drawString(title, 28, 10);

        // Subtle bottom border
        canvas.drawFastHLine(0, UI_HEADER_H - 1, UI_W, C_CARD_BORDER);
    }

    // Dynamic right side: clear rate badge and status pill region
    canvas.fillRect(530, 2, 268, UI_HEADER_H - 4, canvas.color565(12, 15, 22));

    // Live Message Rate Pill (x=536..654, w=118, h=30)
    canvas.fillRoundRect(536, 7, 118, 30, 5, C_CARD_INNER);
    canvas.drawRoundRect(536, 7, 118, 30, 5, C_CARD_BORDER);
    canvas.fillCircle(548, 22, 3, currentPPS > 0 ? C_GREEN_OK : C_TEXT_MUTED);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f msg/s", currentPPS);
    canvas.setTextColor(C_TEXT_CYAN);
    canvas.setFont(fonts::Font2);
    canvas.drawString(buf, 558, 14);

    // SD / REC Status Pill (x=666..788, w=122, h=30)
    if (currentLogMode != LOG_IDLE) {
        bool blink = ((millis() / 500) % 2 == 0);
        canvas.fillRoundRect(666, 7, 122, 30, 5, blink ? C_TRD_RED : canvas.color565(90, 15, 22));
        canvas.drawRoundRect(666, 7, 122, 30, 5, canvas.color565(255, 80, 80));
        canvas.setTextColor(C_TEXT_WHITE);
        canvas.setFont(fonts::Font2);
        canvas.drawCenterString((currentLogMode == LOG_CANBUS) ? "CAN REC" : "PID REC", 727, 14);
    } else {
        canvas.fillRoundRect(666, 7, 122, 30, 5, sdMounted ? canvas.color565(15, 36, 24) : C_CARD_INNER);
        canvas.drawRoundRect(666, 7, 122, 30, 5, sdMounted ? canvas.color565(30, 120, 60) : C_CARD_BORDER);
        canvas.setTextColor(sdMounted ? C_GREEN_OK : C_TEXT_MUTED);
        canvas.setFont(fonts::Font2);
        canvas.drawCenterString(sdMounted ? "SD READY" : "NO SD", 727, 14);
    }
}

void drawBottomNavBar(bool forceFull) {
    static int s_lastNavScreen = -1;

    if (forceFull || s_lastNavScreen < 0) {
        // Full navbar background and persistent navigation pills
        canvas.fillRect(0, UI_H - UI_NAVBAR_H, UI_W, UI_NAVBAR_H, canvas.color565(10, 13, 18));

        // < PREV pill button
        canvas.fillRoundRect(12, UI_H - UI_NAVBAR_H + 6, 96, 28, 6, C_CARD_INNER);
        canvas.drawRoundRect(12, UI_H - UI_NAVBAR_H + 6, 96, 28, 6, C_CARD_BORDER);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.setFont(fonts::Font2);
        canvas.drawCenterString("< PREV", 60, UI_H - UI_NAVBAR_H + 11);

        // NEXT > pill button
        canvas.fillRoundRect(UI_W - 108, UI_H - UI_NAVBAR_H + 6, 96, 28, 6, C_CARD_INNER);
        canvas.drawRoundRect(UI_W - 108, UI_H - UI_NAVBAR_H + 6, 96, 28, 6, C_CARD_BORDER);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.setFont(fonts::Font2);
        canvas.drawCenterString("NEXT >", UI_W - 60, UI_H - UI_NAVBAR_H + 11);
    }

    if (forceFull || s_lastNavScreen != currentScreen) {
        s_lastNavScreen = currentScreen;

        // Clear only the indicator dots region (x: 312..488, y: 452..470)
        int dotSpacing = 24;
        int startDotX = 400 - (((SCREEN_COUNT - 1) * dotSpacing) / 2);
        int dotRegionW = ((SCREEN_COUNT - 1) * dotSpacing) + 36;
        canvas.fillRect(startDotX - 18, UI_H - 28, dotRegionW, 18, canvas.color565(10, 13, 18));

        for (int i = 0; i < SCREEN_COUNT; i++) {
            int dx = startDotX + (i * dotSpacing);
            if (i == currentScreen) {
                canvas.fillRoundRect(dx - 12, UI_H - 24, 24, 8, 4, C_TRD_RED); // TRD Red active capsule
            } else {
                canvas.fillCircle(dx, UI_H - 20, 3, canvas.color565(55, 65, 85));
            }
        }
    }
}

// Page 0: Live Vehicle Cluster (TRD Motorsport Gauge)
void renderDashboard(bool forceFull = false) {
    static int s_rpm = -999;
    static char s_gear[8] = "";
    static bool s_tcc = false;
    static float s_cmdAfr = -999.0f, s_actAfr = -999.0f;
    static float s_kclv = -999.0f, s_kfb = -999.0f;
    static int s_thr = -999, s_load = -999;
    static unsigned long s_lastHeaderMs = 0;
    static unsigned long s_lastRibbonMs = 0;

    if (forceFull) {
        s_rpm = -999;
        s_gear[0] = '\0';
        s_tcc = false;
        s_cmdAfr = -999.0f;
        s_actAfr = -999.0f;
        s_kclv = -999.0f;
        s_kfb = -999.0f;
        s_thr = -999;
        s_load = -999;
        s_lastHeaderMs = millis();
        s_lastRibbonMs = millis();

        drawHeaderBar("TOYOTA DASHVIEW - CLUSTER", true);
        drawBottomNavBar();
    } else if (millis() - s_lastHeaderMs >= 1000) {
        s_lastHeaderMs = millis();
        drawHeaderBar("TOYOTA DASHVIEW - CLUSTER", false);
    }

    char buf[64];

    // 1. Full-Width Tachometer Bar (0 - 6000 RPM) with Recessed Track & Redline Zone
    if (forceFull || vehicleData.rpm != s_rpm) {
        s_rpm = vehicleData.rpm;
        int rpmY = 48;
        canvas.fillRoundRect(12, rpmY, 776, 50, 8, C_CARD_BG);
        canvas.drawRoundRect(12, rpmY, 776, 50, 8, C_CARD_BORDER);

        // Header Row: Label on left, prominent digital readout + unit on right
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.setFont(fonts::Font2);
        canvas.drawString("TACHOMETER", 24, rpmY + 5);

        snprintf(buf, sizeof(buf), "%d", vehicleData.rpm);
        canvas.setTextColor(C_TEXT_WHITE);
        canvas.setFont(fonts::Font5);
        canvas.drawRightString(buf, 715, rpmY + 1);

        canvas.setTextColor(C_TEXT_CYAN);
        canvas.setFont(fonts::Font2);
        canvas.drawString("RPM", 722, rpmY + 6);

        // Recessed Bar Track
        int trackX = 24, trackY = rpmY + 26, trackW = 752, trackH = 16;
        canvas.fillRoundRect(trackX, trackY, trackW, trackH, 4, C_CARD_INNER);
        canvas.drawRoundRect(trackX, trackY, trackW, trackH, 4, canvas.color565(36, 46, 64));

        // Redline Zone Accent (5200 - 6000 RPM: 651px to 752px)
        int redlineX = trackX + 651;
        canvas.fillRect(redlineX, trackY + 1, trackW - 653, trackH - 2, canvas.color565(60, 15, 22));

        // Graduation tick marks at 1k, 2k, 3k, 4k, 5k RPM
        for (int t = 1; t <= 5; t++) {
            int tx = trackX + (t * trackW) / 6;
            canvas.drawFastVLine(tx, trackY, trackH, canvas.color565(45, 55, 75));
        }

        // Active RPM Fill
        int rpmW = map(constrain(vehicleData.rpm, 0, 6000), 0, 6000, 0, trackW - 4);
        if (rpmW > 0) {
            int w1 = std::min(rpmW, 436);
            if (w1 > 0) canvas.fillRect(trackX + 2, trackY + 2, w1, trackH - 4, C_TEXT_CYAN);
            if (rpmW > 436) {
                int w2 = std::min(rpmW - 436, 648 - 436);
                if (w2 > 0) canvas.fillRect(trackX + 2 + 436, trackY + 2, w2, trackH - 4, C_TRD_ORANGE);
            }
            if (rpmW > 648) {
                int w3 = rpmW - 648;
                if (w3 > 0) canvas.fillRect(trackX + 2 + 648, trackY + 2, w3, trackH - 4, C_TRD_RED);
            }
        }
    }

    // 2. Center Hero: Transmission Gear & TCC Lockup Card (Left, w=244)
    if (forceFull || strcmp(vehicleData.gear, s_gear) != 0 || vehicleData.tccLocked != s_tcc) {
        strncpy(s_gear, vehicleData.gear, sizeof(s_gear) - 1);
        s_gear[sizeof(s_gear) - 1] = '\0';
        s_tcc = vehicleData.tccLocked;

        int gx = 12, gy = 104, gw = 244, gh = 164;
        canvas.fillRoundRect(gx, gy, gw, gh, 8, C_CARD_BG);
        canvas.drawRoundRect(gx, gy, gw, gh, 8, C_CARD_BORDER);
        canvas.fillRect(gx + 2, gy, gw - 4, 3, C_TRD_RED); // Top accent stripe

        canvas.setTextColor(C_TEXT_MUTED);
        canvas.setFont(fonts::Font2);
        canvas.drawCenterString("TRANSMISSION", gx + gw / 2, gy + 10);

        canvas.setFont(fonts::Font7);
        if (vehicleData.tccLocked && vehicleData.gear[0] >= '1' && vehicleData.gear[0] <= '6') {
            snprintf(buf, sizeof(buf), "%sL", vehicleData.gear);
            canvas.setTextColor(C_GOLD_LOCK);
        } else {
            snprintf(buf, sizeof(buf), "%s", vehicleData.gear);
            canvas.setTextColor(C_TEXT_WHITE);
        }
        canvas.drawCenterString(buf, gx + gw / 2, gy + 36);

        // TCC Lockup Status Pill
        int pillX = gx + 32, pillY = gy + 116, pillW = 180, pillH = 34;
        if (vehicleData.tccLocked) {
            canvas.fillRoundRect(pillX, pillY, pillW, pillH, 6, canvas.color565(180, 140, 0));
            canvas.drawRoundRect(pillX, pillY, pillW, pillH, 6, canvas.color565(255, 215, 0));
            canvas.setTextColor(TFT_BLACK);
            canvas.setFont(fonts::Font2);
            canvas.drawCenterString("TCC LOCKED", gx + gw / 2, pillY + 8);
        } else {
            canvas.fillRoundRect(pillX, pillY, pillW, pillH, 6, C_CARD_INNER);
            canvas.drawRoundRect(pillX, pillY, pillW, pillH, 6, C_CARD_BORDER);
            canvas.setTextColor(C_TEXT_MUTED);
            canvas.setFont(fonts::Font2);
            canvas.drawCenterString("TCC OPEN", gx + gw / 2, pillY + 8);
        }
    }

    // 3. Air-Fuel Ratio (AFR) Wideband Card (Middle, w=264)
    if (forceFull || vehicleData.commandedAfr != s_cmdAfr || vehicleData.actualAfr != s_actAfr) {
        s_cmdAfr = vehicleData.commandedAfr;
        s_actAfr = vehicleData.actualAfr;

        int ax = 268, ay = 104, aw = 264, ah = 164;
        canvas.fillRoundRect(ax, ay, aw, ah, 8, C_CARD_BG);
        canvas.drawRoundRect(ax, ay, aw, ah, 8, C_CARD_BORDER);
        canvas.fillRect(ax + 2, ay, aw - 4, 3, C_TEXT_CYAN); // Top accent stripe

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("AIR-FUEL RATIO", ax + aw / 2, ay + 10);

        // Column 1: Commanded Target AFR (w=118, h=72)
        int c1x = ax + 10, c1y = ay + 34, cw = 118, ch = 72;
        canvas.fillRoundRect(c1x, c1y, cw, ch, 6, C_CARD_INNER);
        canvas.drawRoundRect(c1x, c1y, cw, ch, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font0);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("TARGET", c1x + cw / 2, c1y + 5);

        snprintf(buf, sizeof(buf), "%.1f", vehicleData.commandedAfr);
        canvas.setFont(fonts::Font5);
        canvas.setTextColor(C_TEXT_WHITE);
        canvas.drawCenterString(buf, c1x + cw / 2, c1y + 20);

        snprintf(buf, sizeof(buf), "%.2f LAMBDA", vehicleData.commandedAfr / 14.7f);
        canvas.setFont(fonts::Font0);
        canvas.setTextColor(C_TEXT_CYAN);
        canvas.drawCenterString(buf, c1x + cw / 2, c1y + 50);

        // Column 2: Actual Wideband AFR (w=118, h=72)
        int c2x = ax + 136, c2y = ay + 34;
        canvas.fillRoundRect(c2x, c2y, cw, ch, 6, C_CARD_INNER);
        canvas.drawRoundRect(c2x, c2y, cw, ch, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font0);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("ACTUAL", c2x + cw / 2, c2y + 5);

        uint16_t actAfrColor = (vehicleData.actualAfr > 15.2f) ? C_TRD_RED : ((vehicleData.actualAfr < 12.0f) ? C_TRD_ORANGE : C_GREEN_OK);
        snprintf(buf, sizeof(buf), "%.1f", vehicleData.actualAfr);
        canvas.setFont(fonts::Font5);
        canvas.setTextColor(actAfrColor);
        canvas.drawCenterString(buf, c2x + cw / 2, c2y + 20);

        snprintf(buf, sizeof(buf), "%.2f LAMBDA", vehicleData.actualAfr / 14.7f);
        canvas.setFont(fonts::Font0);
        canvas.setTextColor(actAfrColor);
        canvas.drawCenterString(buf, c2x + cw / 2, c2y + 50);

        // Bottom Mini Gauge Track (10.0 to 18.0 span)
        int mTrackX = ax + 10, mTrackY = ay + 116, mTrackW = 244, mTrackH = 14;
        canvas.fillRoundRect(mTrackX, mTrackY, mTrackW, mTrackH, 4, C_CARD_INNER);
        canvas.drawRoundRect(mTrackX, mTrackY, mTrackW, mTrackH, 4, C_CARD_BORDER);

        // Stoichiometric marker (14.7 AFR is at (14.7 - 10) / 8 = 0.5875)
        int stoichX = mTrackX + (int)(0.5875f * (float)mTrackW);
        canvas.drawFastVLine(stoichX, mTrackY, mTrackH, C_GREEN_OK);

        // Live Actual Pip Indicator
        float afrFrac = (vehicleData.actualAfr - 10.0f) / 8.0f;
        int actPipX = mTrackX + constrain((int)(afrFrac * (float)(mTrackW - 6)), 0, mTrackW - 6);
        canvas.fillRoundRect(actPipX, mTrackY - 1, 6, mTrackH + 2, 2, actAfrColor);

        // Stoich reference subtext
        canvas.setFont(fonts::Font0);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("RICH 10.0          STOICH 14.7          LEAN 18.0", ax + aw / 2, ay + 138);
    }

    // 4. Knock Health (KCLV & KFB) Card (Right, w=244)
    if (forceFull || vehicleData.kclv != s_kclv || vehicleData.knockFB != s_kfb) {
        s_kclv = vehicleData.kclv;
        s_kfb = vehicleData.knockFB;

        int kx = 544, ky = 104, kw = 244, kh = 164;
        canvas.fillRoundRect(kx, ky, kw, kh, 8, C_CARD_BG);
        canvas.drawRoundRect(kx, ky, kw, kh, 8, C_CARD_BORDER);
        canvas.fillRect(kx + 2, ky, kw - 4, 3, C_TRD_ORANGE); // Top accent stripe

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("KNOCK HEALTH", kx + kw / 2, ky + 10);

        // Column 1: KCLV Learned Octane Value (w=108, h=72)
        int c1x = kx + 10, c1y = ky + 34, cw = 108, ch = 72;
        canvas.fillRoundRect(c1x, c1y, cw, ch, 6, C_CARD_INNER);
        canvas.drawRoundRect(c1x, c1y, cw, ch, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font0);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("KCLV", c1x + cw / 2, c1y + 5);

        uint16_t kclvColor = (vehicleData.kclv >= 19.0f) ? C_GREEN_OK : ((vehicleData.kclv >= 15.0f) ? C_TRD_ORANGE : C_TRD_RED);
        snprintf(buf, sizeof(buf), "%.1f", vehicleData.kclv);
        canvas.setFont(fonts::Font5);
        canvas.setTextColor(kclvColor);
        canvas.drawCenterString(buf, c1x + cw / 2, c1y + 20);

        canvas.setFont(fonts::Font0);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("LEARNED", c1x + cw / 2, c1y + 50);

        // Column 2: Feedback Retard (w=108, h=72)
        int c2x = kx + 126, c2y = ky + 34;
        canvas.fillRoundRect(c2x, c2y, cw, ch, 6, C_CARD_INNER);
        canvas.drawRoundRect(c2x, c2y, cw, ch, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font0);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("FEEDBACK", c2x + cw / 2, c2y + 5);

        uint16_t kfbColor = (vehicleData.knockFB < 0) ? C_TRD_RED : C_TEXT_CYAN;
        snprintf(buf, sizeof(buf), "%+2.1f", vehicleData.knockFB);
        canvas.setFont(fonts::Font5);
        canvas.setTextColor(kfbColor);
        canvas.drawCenterString(buf, c2x + cw / 2, c2y + 20);

        canvas.setFont(fonts::Font0);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("DEGREES", c2x + cw / 2, c2y + 50);

        // Bottom Evaluation Pill (w=224, h=34)
        int pillX = kx + 10, pillY = ky + 116, pillW = 224, pillH = 34;
        if (vehicleData.kclv >= 19.0f && vehicleData.knockFB >= 0.0f) {
            canvas.fillRoundRect(pillX, pillY, pillW, pillH, 6, canvas.color565(15, 38, 24));
            canvas.drawRoundRect(pillX, pillY, pillW, pillH, 6, canvas.color565(30, 120, 60));
            canvas.setTextColor(C_GREEN_OK);
            canvas.setFont(fonts::Font2);
            canvas.drawCenterString("OCTANE OPTIMAL (20.0)", kx + kw / 2, pillY + 8);
        } else if (vehicleData.knockFB < 0.0f) {
            canvas.fillRoundRect(pillX, pillY, pillW, pillH, 6, canvas.color565(45, 15, 18));
            canvas.drawRoundRect(pillX, pillY, pillW, pillH, 6, C_TRD_RED);
            canvas.setTextColor(C_TRD_RED);
            canvas.setFont(fonts::Font2);
            canvas.drawCenterString("KNOCK RETARD ACTIVE", kx + kw / 2, pillY + 8);
        } else {
            canvas.fillRoundRect(pillX, pillY, pillW, pillH, 6, canvas.color565(42, 30, 12));
            canvas.drawRoundRect(pillX, pillY, pillW, pillH, 6, C_TRD_ORANGE);
            canvas.setTextColor(C_TRD_ORANGE);
            canvas.setFont(fonts::Font2);
            canvas.drawCenterString("OCTANE ADAPTING", kx + kw / 2, pillY + 8);
        }
    }

    // 5. Dual Mini-Gauges: Throttle % & Engine Load %
    int botY = 276;
    if (forceFull || vehicleData.throttlePct != s_thr) {
        s_thr = vehicleData.throttlePct;
        canvas.fillRoundRect(12, botY, 382, 76, 8, C_CARD_BG);
        canvas.drawRoundRect(12, botY, 382, 76, 8, C_CARD_BORDER);

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("THROTTLE POSITION", 24, botY + 8);

        snprintf(buf, sizeof(buf), "%d%%", vehicleData.throttlePct);
        canvas.setFont(fonts::Font4);
        canvas.setTextColor(C_TEXT_CYAN);
        canvas.drawRightString(buf, 382, botY + 6);

        // Recessed Track
        canvas.fillRoundRect(24, botY + 36, 358, 26, 5, C_CARD_INNER);
        canvas.drawRoundRect(24, botY + 36, 358, 26, 5, C_CARD_BORDER);

        int thrW = map(constrain(vehicleData.throttlePct, 0, 100), 0, 100, 0, 352);
        if (thrW > 0) {
            canvas.fillRoundRect(27, botY + 39, thrW, 20, 3, C_TEXT_CYAN);
        }
        // Sub-ticks at 25%, 50%, 75%
        canvas.drawFastVLine(24 + 90, botY + 36, 26, canvas.color565(36, 46, 64));
        canvas.drawFastVLine(24 + 179, botY + 36, 26, canvas.color565(36, 46, 64));
        canvas.drawFastVLine(24 + 268, botY + 36, 26, canvas.color565(36, 46, 64));
    }

    if (forceFull || vehicleData.engineLoadPct != s_load) {
        s_load = vehicleData.engineLoadPct;
        canvas.fillRoundRect(406, botY, 382, 76, 8, C_CARD_BG);
        canvas.drawRoundRect(406, botY, 382, 76, 8, C_CARD_BORDER);

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("CALCULATED ENGINE LOAD", 418, botY + 8);

        snprintf(buf, sizeof(buf), "%d%%", vehicleData.engineLoadPct);
        canvas.setFont(fonts::Font4);
        canvas.setTextColor(C_TRD_ORANGE);
        canvas.drawRightString(buf, 776, botY + 6);

        // Recessed Track
        canvas.fillRoundRect(418, botY + 36, 358, 26, 5, C_CARD_INNER);
        canvas.drawRoundRect(418, botY + 36, 358, 26, 5, C_CARD_BORDER);

        int loadW = map(constrain(vehicleData.engineLoadPct, 0, 100), 0, 100, 0, 352);
        if (loadW > 0) {
            canvas.fillRoundRect(421, botY + 39, loadW, 20, 3, C_TRD_ORANGE);
        }
        // Sub-ticks at 25%, 50%, 75%
        canvas.drawFastVLine(418 + 90, botY + 36, 26, canvas.color565(36, 46, 64));
        canvas.drawFastVLine(418 + 179, botY + 36, 26, canvas.color565(36, 46, 64));
        canvas.drawFastVLine(418 + 268, botY + 36, 26, canvas.color565(36, 46, 64));
    }

    // 6. Status Ribbon (Wi-Fi, CAN RX, Free Memory, Real-Time Clock)
    static int s_lastWifiClient = -1;
    static unsigned long s_lastPktCount = 0xFFFFFFFF;
    static unsigned int s_lastHeapKB = 0;
    static char s_lastRtcStamp[16] = "";

    int ribY = 360;
    int t0x = 20, ty = ribY + 8, tw = 184, th = 56;
    int t1x = 212;
    int t2x = 404;
    int t3x = 596;

    if (forceFull) {
        canvas.fillRoundRect(12, ribY, 776, 72, 8, C_CARD_BG);
        canvas.drawRoundRect(12, ribY, 776, 72, 8, C_CARD_BORDER);

        // Tile 0: Wi-Fi Hotspot
        canvas.fillRoundRect(t0x, ty, tw, th, 6, C_CARD_INNER);
        canvas.drawRoundRect(t0x, ty, tw, th, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("WI-FI HOTSPOT", t0x + tw / 2, ty + 6);

        // Tile 1: CAN Bus Traffic
        canvas.fillRoundRect(t1x, ty, tw, th, 6, C_CARD_INNER);
        canvas.drawRoundRect(t1x, ty, tw, th, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("CAN BUS FRAMES", t1x + tw / 2, ty + 6);

        // Tile 2: Free Memory (Heap)
        canvas.fillRoundRect(t2x, ty, tw, th, 6, C_CARD_INNER);
        canvas.drawRoundRect(t2x, ty, tw, th, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("FREE MEMORY", t2x + tw / 2, ty + 6);

        // Tile 3: Real-Time Clock
        canvas.fillRoundRect(t3x, ty, tw, th, 6, C_CARD_INNER);
        canvas.drawRoundRect(t3x, ty, tw, th, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawCenterString("REAL-TIME CLOCK", t3x + tw / 2, ty + 6);

        s_lastWifiClient = -1;
        s_lastPktCount = 0xFFFFFFFF;
        s_lastHeapKB = 0;
        s_lastRtcStamp[0] = '\0';
    }

    if (forceFull || millis() - s_lastRibbonMs >= 1000) {
        s_lastRibbonMs = millis();

        // Tile 0: Wi-Fi Hotspot Status
        bool wifiClient = (WiFi.status() == WL_CONNECTED || WiFi.softAPgetStationNum() > 0);
        if (forceFull || (wifiClient ? 1 : 0) != s_lastWifiClient) {
            s_lastWifiClient = wifiClient ? 1 : 0;
            canvas.fillRect(t0x + 4, ty + 22, tw - 8, 28, C_CARD_INNER);
            canvas.setFont(fonts::Font2);
            canvas.setTextColor(wifiClient ? C_GREEN_OK : C_TEXT_CYAN);
            canvas.drawCenterString(wifiClient ? "CLIENT ACTIVE" : "HOTSPOT READY", t0x + tw / 2, ty + 28);
        }

        // Tile 1: CAN Bus Traffic
        if (forceFull || packetCount != s_lastPktCount) {
            s_lastPktCount = packetCount;
            canvas.fillRect(t1x + 4, ty + 22, tw - 8, 28, C_CARD_INNER);
            snprintf(buf, sizeof(buf), "%lu pkts", packetCount);
            canvas.setFont(fonts::Font2);
            canvas.setTextColor(C_TEXT_WHITE);
            canvas.drawCenterString(buf, t1x + tw / 2, ty + 28);
        }

        // Tile 2: Free Memory (Heap)
        unsigned int heapKB = (unsigned int)(ESP.getFreeHeap() / 1024);
        if (forceFull || heapKB != s_lastHeapKB) {
            s_lastHeapKB = heapKB;
            canvas.fillRect(t2x + 4, ty + 22, tw - 8, 28, C_CARD_INNER);
            snprintf(buf, sizeof(buf), "%u KB HEAP", heapKB);
            canvas.setFont(fonts::Font2);
            canvas.setTextColor(C_GREEN_OK);
            canvas.drawCenterString(buf, t2x + tw / 2, ty + 28);
        }

        // Tile 3: Real-Time Clock
        char stamp[24];
        if (rtcStamp(stamp, sizeof(stamp))) {
            if (forceFull || strcmp(s_lastRtcStamp, stamp + 11) != 0) {
                strncpy(s_lastRtcStamp, stamp + 11, sizeof(s_lastRtcStamp) - 1);
                canvas.fillRect(t3x + 4, ty + 22, tw - 8, 28, C_CARD_INNER);
                canvas.setFont(fonts::Font2);
                canvas.setTextColor(C_TEXT_WHITE);
                canvas.drawCenterString(stamp + 11, t3x + tw / 2, ty + 28);
            }
        } else {
            if (forceFull || strcmp(s_lastRtcStamp, "RTC SYNC") != 0) {
                strcpy(s_lastRtcStamp, "RTC SYNC");
                canvas.fillRect(t3x + 4, ty + 22, tw - 8, 28, C_CARD_INNER);
                canvas.setFont(fonts::Font2);
                canvas.setTextColor(C_TEXT_MUTED);
                canvas.drawCenterString("RTC SYNC", t3x + tw / 2, ty + 28);
            }
        }
    }
}

// Floating Overlay Sub-Screen: Raw Packet Monitor Modal
void renderRawSnifferModal() {
    // Header Bar
    canvas.fillRect(0, 0, UI_W, UI_HEADER_H, canvas.color565(12, 15, 22));
    canvas.fillRect(0, 0, 6, UI_HEADER_H, C_TRD_ORANGE);
    canvas.fillRect(6, 0, 6, UI_HEADER_H, C_TRD_RED);
    canvas.fillRect(12, 0, 6, UI_HEADER_H, C_TRD_BURGUNDY);

    canvas.setTextColor(C_TEXT_WHITE, canvas.color565(12, 15, 22));
    canvas.setFont(fonts::Font4);
    canvas.drawString("RAW CAN STREAM", 28, 10);

    // Status Pill: STREAMING (Cyan) vs PAUSED (Orange)
    if (isSnifferPaused) {
        canvas.fillRoundRect(650, 7, 136, 30, 5, canvas.color565(80, 45, 10));
        canvas.drawRoundRect(650, 7, 136, 30, 5, C_TRD_ORANGE);
        canvas.setTextColor(C_TRD_ORANGE);
        canvas.setFont(fonts::Font2);
        canvas.drawCenterString("PAUSED", 718, 14);
    } else {
        canvas.fillRoundRect(640, 7, 146, 30, 5, canvas.color565(10, 40, 50));
        canvas.drawRoundRect(640, 7, 146, 30, 5, C_TEXT_CYAN);
        canvas.setTextColor(C_TEXT_CYAN);
        canvas.setFont(fonts::Font2);
        canvas.drawCenterString("STREAMING", 713, 14);
    }

    canvas.drawFastHLine(0, UI_HEADER_H - 1, UI_W, C_CARD_BORDER);

    // Table Column Header
    canvas.fillRect(12, 50, 776, 26, C_CARD_INNER);
    canvas.drawRoundRect(12, 50, 776, 26, 4, C_CARD_BORDER);
    canvas.setFont(fonts::Font2);
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("CAN ID", 24, 55);
    canvas.drawString("DLC", 150, 55);
    canvas.drawString("HEX PAYLOAD (BYTES 0..7)", 230, 55);

    // Render Frame Rows (up to 10 rows)
    char buf[96];
    int rowY = 80;
    for (int i = 0; i < 10; i++) {
        int idx = (snifferHead - 1 - i + SNIFFER_HISTORY_SIZE) % SNIFFER_HISTORY_SIZE;
        uint16_t rowBg = (i % 2 == 0) ? C_CARD_BG : canvas.color565(22, 28, 40);
        canvas.fillRoundRect(12, rowY, 776, 30, 4, rowBg);

        if (snifferHistory[idx].id != 0 || snifferHistory[idx].dlc != 0) {
            snprintf(buf, sizeof(buf), "0x%03lX", (unsigned long)snifferHistory[idx].id);
            canvas.setTextColor(C_TRD_ORANGE);
            canvas.setFont(fonts::Font4);
            canvas.drawString(buf, 24, rowY + 3);

            snprintf(buf, sizeof(buf), "[%d]", snifferHistory[idx].dlc);
            canvas.setTextColor(C_TEXT_MUTED);
            canvas.setFont(fonts::Font2);
            canvas.drawString(buf, 150, rowY + 7);

            char hexBuf[48] = "";
            for (int b = 0; b < snifferHistory[idx].dlc && b < 8; b++) {
                char bStr[6];
                snprintf(bStr, sizeof(bStr), "%02X ", snifferHistory[idx].data[b]);
                strcat(hexBuf, bStr);
            }
            canvas.setTextColor(C_TEXT_WHITE);
            canvas.setFont(fonts::Font4);
            canvas.drawString(hexBuf, 230, rowY + 3);
        } else {
            canvas.setTextColor(C_TEXT_MUTED);
            canvas.setFont(fonts::Font0);
            canvas.drawString("-- Waiting for bus traffic --", 230, rowY + 9);
        }

        rowY += 32;
    }

    // Bottom Action Deck (Pause, Clear, Back)
    int botY = 412;

    // 1. [ PAUSE / RESUME ] Button (x: 12..192)
    uint16_t pauseBg = isSnifferPaused ? C_TRD_ORANGE : canvas.color565(25, 35, 52);
    uint16_t pauseBorder = isSnifferPaused ? canvas.color565(255, 180, 50) : canvas.color565(60, 100, 160);
    canvas.fillRoundRect(12, botY, 180, 44, 6, pauseBg);
    canvas.drawRoundRect(12, botY, 180, 44, 6, pauseBorder);
    canvas.setTextColor(isSnifferPaused ? TFT_BLACK : C_TEXT_WHITE);
    canvas.setFont(fonts::Font4);
    canvas.drawCenterString(isSnifferPaused ? "RESUME" : "PAUSE", 102, botY + 8);

    // 2. [ CLEAR ] Button (x: 204..364)
    canvas.fillRoundRect(204, botY, 160, 44, 6, canvas.color565(30, 32, 42));
    canvas.drawRoundRect(204, botY, 160, 44, 6, C_CARD_BORDER);
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.setFont(fonts::Font4);
    canvas.drawCenterString("CLEAR", 284, botY + 8);

    // 3. [ BACK / CLOSE ] Button (x: 376..788)
    canvas.fillRoundRect(376, botY, 412, 44, 6, C_TRD_RED);
    canvas.drawRoundRect(376, botY, 412, 44, 6, canvas.color565(255, 100, 100));
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.setFont(fonts::Font4);
    canvas.drawCenterString("BACK / CLOSE", 582, botY + 8);
}

// Page 1: Live CAN Sniffer & Traffic Monitor
void renderSniffer(bool forceFull = false) {
    if (isRawSnifferModalOpen) {
        renderRawSnifferModal();
        return;
    }

    static unsigned long s_lastSnifferTick = 0;
    static unsigned long s_lastPktCount = 0;

    if (forceFull) {
        drawHeaderBar("CAN SNIFFER & TRAFFIC MONITOR", true);

        char buf[96];
        unsigned long elapsedSec = (currentLogMode != LOG_IDLE) ? ((millis() - logStartTime) / 1000) : 0;
        bool isCanActive = (currentLogMode == LOG_CANBUS);
        bool canDisabled = (currentLogMode == LOG_DATALOG);

        // Card 1: CAN Sniffer & Raw Frame Logger Control (y: 52..138, h: 86)
        uint16_t canBgColor = isCanActive ? canvas.color565(55, 14, 20) : (canDisabled ? canvas.color565(16, 18, 24) : C_CARD_BG);
        uint16_t canBorderColor = isCanActive ? C_TRD_RED : (canDisabled ? canvas.color565(35, 40, 52) : C_CARD_BORDER);

        canvas.fillRoundRect(12, 52, 776, 86, 8, canBgColor);
        canvas.drawRoundRect(12, 52, 776, 86, 8, canBorderColor);
        canvas.fillRect(14, 52, 6, 86, isCanActive ? C_TRD_RED : C_TRD_ORANGE);

        canvas.setFont(fonts::Font4);
        if (isCanActive) {
            canvas.setTextColor(C_TRD_RED);
            canvas.drawString("[STOP CAN LOGGING]", 34, 60);
            canvas.setFont(fonts::Font2);
            snprintf(buf, sizeof(buf), "REC: %s (%lu frames, %lum%02lus)", currentLogFileName, logEntryCount, elapsedSec / 60, elapsedSec % 60);
            canvas.setTextColor(canvas.color565(255, 200, 200));
            canvas.drawString(buf, 34, 106);
        } else {
            canvas.setTextColor(canDisabled ? C_TEXT_MUTED : C_TEXT_WHITE);
            canvas.drawString("[START CAN LOGGING]", 34, 60);
            canvas.setFont(fonts::Font2);
            canvas.setTextColor(canDisabled ? canvas.color565(80, 85, 100) : C_TEXT_MUTED);
            canvas.drawString(canDisabled ? "Locked (Stop PID Datalogger first)" : "Logs raw vehicle bus traffic -> canbus_XXXX.csv", 34, 106);
        }

        // Card 2: CAN Bus Traffic & Statistics Deck (y: 146..282, h: 136)
        canvas.fillRoundRect(12, 146, 776, 136, 8, C_CARD_BG);
        canvas.drawRoundRect(12, 146, 776, 136, 8, C_CARD_BORDER);
        canvas.fillRect(14, 146, 6, 136, C_TEXT_CYAN);

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("CAN BUS TRAFFIC & STATUS", 34, 156);

        // Tile 1: Total Frames Inset Well (w=356, h=54)
        canvas.fillRoundRect(34, 176, 356, 54, 6, C_CARD_INNER);
        canvas.drawRoundRect(34, 176, 356, 54, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font0);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("TOTAL PACKETS RECEIVED", 46, 182);

        snprintf(buf, sizeof(buf), "%lu pkts", packetCount);
        canvas.setTextColor(C_TEXT_CYAN);
        canvas.setFont(fonts::Font5);
        canvas.drawString(buf, 46, 198);

        // Tile 2: Packet Rate Inset Well (w=366, h=54)
        canvas.fillRoundRect(402, 176, 374, 54, 6, C_CARD_INNER);
        canvas.drawRoundRect(402, 176, 374, 54, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font0);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("LIVE BUS MESSAGE RATE", 414, 182);

        snprintf(buf, sizeof(buf), "%.0f msg/s", currentPPS);
        canvas.setTextColor(C_GREEN_OK);
        canvas.setFont(fonts::Font5);
        canvas.drawString(buf, 414, 198);

        // Bottom Protocol Line
        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        snprintf(buf, sizeof(buf), "TWAI Controller: 500 kbps HS-CAN (TX GPIO%d / RX GPIO%d)", CAN_TX_PIN, CAN_RX_PIN);
        canvas.drawString(buf, 34, 246);

        // Card 3: Raw Packet Stream Terminal Launcher (y: 292..428, h: 136)
        canvas.fillRoundRect(12, 292, 776, 136, 8, C_CARD_BG);
        canvas.drawRoundRect(12, 292, 776, 136, 8, C_CARD_BORDER);
        canvas.fillRect(14, 292, 6, 136, C_TRD_BURGUNDY);

        canvas.fillRoundRect(34, 304, 742, 112, 6, canvas.color565(22, 28, 40));
        canvas.drawRoundRect(34, 304, 742, 112, 6, canvas.color565(45, 60, 85));

        canvas.setFont(fonts::Font4);
        canvas.setTextColor(C_TEXT_WHITE);
        canvas.drawString("[+] VIEW LIVE RAW PACKET STREAM", 50, 318);

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_CYAN);
        canvas.drawString("Tap to open live scrolling terminal with pause, clear & frame inspection", 50, 350);

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Monitors all standard 11-bit and extended 29-bit bus frames in real time", 50, 380);

        drawBottomNavBar();
        s_lastSnifferTick = millis();
        s_lastPktCount = packetCount;
    } else if (millis() - s_lastSnifferTick >= 500) {
        s_lastSnifferTick = millis();
        drawHeaderBar("CAN SNIFFER & TRAFFIC MONITOR", false);

        if (currentLogMode == LOG_CANBUS) {
            char buf[96];
            unsigned long elapsedSec = (millis() - logStartTime) / 1000;
            canvas.fillRect(34, 104, 740, 24, canvas.color565(55, 14, 20));
            canvas.setFont(fonts::Font2);
            snprintf(buf, sizeof(buf), "REC: %s (%lu frames, %lum%02lus)", currentLogFileName, logEntryCount, elapsedSec / 60, elapsedSec % 60);
            canvas.setTextColor(canvas.color565(255, 200, 200));
            canvas.drawString(buf, 34, 106);
        }

        if (packetCount != s_lastPktCount) {
            s_lastPktCount = packetCount;
            char buf[96];
            canvas.fillRect(46, 196, 330, 30, C_CARD_INNER);
            snprintf(buf, sizeof(buf), "%lu pkts", packetCount);
            canvas.setTextColor(C_TEXT_CYAN);
            canvas.setFont(fonts::Font5);
            canvas.drawString(buf, 46, 198);

            canvas.fillRect(414, 196, 350, 30, C_CARD_INNER);
            snprintf(buf, sizeof(buf), "%.0f msg/s", currentPPS);
            canvas.setTextColor(C_GREEN_OK);
            canvas.setFont(fonts::Font5);
            canvas.drawString(buf, 414, 198);
        }
    }
}

// Sub-Screen: Interactive PID Selector Modal
#define DL_PICKER_PER_PAGE 21
int g_dlPickerPage = 0;

void renderPidSelector(bool forceFull = false) {
    static int s_lastPage = -1;
    static int s_lastPidCount = -1;
    int curPidCount = getActivePidCount();
    if (!forceFull && s_lastPage == g_dlPickerPage && s_lastPidCount == curPidCount) {
        return;
    }
    s_lastPage = g_dlPickerPage;
    s_lastPidCount = curPidCount;

    char titleBuf[64];
    int total = getSignalCount();
    int pages = (total + DL_PICKER_PER_PAGE - 1) / DL_PICKER_PER_PAGE;
    if (g_dlPickerPage >= pages) g_dlPickerPage = 0;
    snprintf(titleBuf, sizeof(titleBuf), "SELECT DATALOG SIGNALS pg %d/%d (%d logged)",
             g_dlPickerPage + 1, pages < 1 ? 1 : pages, curPidCount);
    drawHeaderBar(titleBuf, true);

    int startY = 52;
    int rowHeight = 42;
    int colWidth = 250;

    for (int slot = 0; slot < DL_PICKER_PER_PAGE; slot++) {
        int sigIdx = g_dlPickerPage * DL_PICKER_PER_PAGE + slot;
        const SignalValue* s = getSignalByIndex(sigIdx);
        if (!s) break;
        bool on = dlLogThis(s->key);
        SignalMeta m{};
        getSignalMeta(s->key, &m);
        int col = (slot % 3);
        int row = (slot / 3);
        int bx = 12 + col * (colWidth + 8);
        int by = startY + row * (rowHeight + 6);

        canvas.fillRoundRect(bx, by, colWidth, rowHeight, 6, on ? canvas.color565(32, 18, 24) : C_CARD_BG);
        canvas.drawRoundRect(bx, by, colWidth, rowHeight, 6, on ? C_TRD_RED : C_CARD_BORDER);
        canvas.setFont(fonts::Font2);
        canvas.setTextColor(on ? C_TRD_RED : C_TEXT_MUTED);
        canvas.drawString(on ? "[X]" : "[ ]", bx + 10, by + 13);
        canvas.setTextColor(on ? C_TEXT_WHITE : C_TEXT_MUTED);
        char lbl[32];
        if (m.unit[0]) snprintf(lbl, sizeof(lbl), "%.18s (%.6s)", s->key, m.unit);
        else           snprintf(lbl, sizeof(lbl), "%.20s", s->key);
        canvas.drawString(lbl, bx + 46, by + 13);
    }

    int botActionY = 412;
    struct { int x, w; const char* t; } btns[] = {
        { 12,  100, "<"}, { 124, 100, ">"}, { 236, 140, "ALL"},
        { 388, 140, "NONE" }, { 540, 248, "DONE" }
    };
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.setFont(fonts::Font4);
    for (auto& b : btns) {
        canvas.fillRoundRect(b.x, botActionY, b.w, 44, 6, canvas.color565(25, 35, 52));
        canvas.drawRoundRect(b.x, botActionY, b.w, 44, 6, canvas.color565(60, 110, 180));
        canvas.drawCenterString(b.t, b.x + b.w / 2, botActionY + 8);
    }
}

// Page 2: Dedicated PID Vehicle Datalogger & Parameter Recording Deck
void renderLoggerControl(bool forceFull = false) {
    if (isPidConfigOpen) {
        renderPidSelector(forceFull);
        return;
    }

    static unsigned long s_lastLoggerTick = 0;

    if (forceFull) {
        drawHeaderBar("PID VEHICLE DATALOGGER", true);

        char buf[96];
        unsigned long elapsedSec = (currentLogMode != LOG_IDLE) ? ((millis() - logStartTime) / 1000) : 0;
        bool isDatalogActive = (currentLogMode == LOG_DATALOG);
        bool datalogDisabled = (currentLogMode == LOG_CANBUS);

        // 1. BUTTON 1: PID Datalogger Start / Stop (y: 52..138, h: 86)
        uint16_t dlBgColor = isDatalogActive ? canvas.color565(55, 30, 10) : (datalogDisabled ? canvas.color565(16, 18, 24) : C_CARD_BG);
        uint16_t dlBorderColor = isDatalogActive ? C_TRD_ORANGE : (datalogDisabled ? canvas.color565(35, 40, 52) : C_CARD_BORDER);

        canvas.fillRoundRect(12, 52, 776, 86, 8, dlBgColor);
        canvas.drawRoundRect(12, 52, 776, 86, 8, dlBorderColor);
        canvas.fillRect(14, 52, 6, 86, isDatalogActive ? C_TRD_ORANGE : C_TEXT_CYAN);

        canvas.setFont(fonts::Font4);
        if (isDatalogActive) {
            canvas.setTextColor(C_TRD_ORANGE);
            canvas.drawString("[STOP PID DATALOG]", 34, 60);
            canvas.setFont(fonts::Font2);
            snprintf(buf, sizeof(buf), "REC: %s (%lu samples, %lum%02lus)", currentLogFileName, logEntryCount, elapsedSec / 60, elapsedSec % 60);
            canvas.setTextColor(canvas.color565(255, 230, 180));
            canvas.drawString(buf, 34, 106);
        } else {
            canvas.setTextColor(datalogDisabled ? C_TEXT_MUTED : C_TEXT_WHITE);
            canvas.drawString("[START PID DATALOG]", 34, 60);
            canvas.setFont(fonts::Font2);
            canvas.setTextColor(datalogDisabled ? canvas.color565(80, 85, 100) : C_TEXT_MUTED);
            snprintf(buf, sizeof(buf), "Logs %d Selected PIDs -> datalog_XXXX.csv", getActivePidCount());
            canvas.drawString(datalogDisabled ? "Locked (Stop CAN Logger on Page 1 first)" : buf, 34, 106);
        }

        // 2. Active Parameters Preview Deck (y: 146..282, h: 136)
        canvas.fillRoundRect(12, 146, 776, 136, 8, C_CARD_BG);
        canvas.drawRoundRect(12, 146, 776, 136, 8, C_CARD_BORDER);
        canvas.fillRect(14, 146, 6, 136, C_TRD_ORANGE);

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_WHITE);
        snprintf(buf, sizeof(buf), "Active Parameters (%d Selected):", getActivePidCount());
        canvas.drawString(buf, 34, 156);

        // Build list of active signal keys
        String tagList = "";
        int count = 0;
        for (int i = 0; i < getSignalCount(); i++) {
            const SignalValue* s = getSignalByIndex(i);
            if (s && dlLogThis(s->key)) {
                if (tagList.length() > 560) { tagList += "..."; break; }
                tagList += "[";
                tagList += s->key;
                tagList += "] ";
                count++;
            }
        }
        if (count == 0) tagList = "(none selected)";

        canvas.fillRoundRect(34, 178, 742, 54, 6, C_CARD_INNER);
        canvas.drawRoundRect(34, 178, 742, 54, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_CYAN);
        canvas.drawString(tagList.c_str(), 46, 186);

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Sampling at 10 Hz over ISO-TP diagnostics to MicroSD card", 34, 246);

        // 3. Configure Recorded PIDs Button (y: 292..428, h: 136)
        canvas.fillRoundRect(12, 292, 776, 136, 8, C_CARD_BG);
        canvas.drawRoundRect(12, 292, 776, 136, 8, C_CARD_BORDER);
        canvas.fillRect(14, 292, 6, 136, C_TRD_BURGUNDY);

        canvas.fillRoundRect(34, 304, 742, 112, 6, canvas.color565(22, 28, 40));
        canvas.drawRoundRect(34, 304, 742, 112, 6, canvas.color565(45, 60, 85));

        canvas.setFont(fonts::Font4);
        canvas.setTextColor(C_TEXT_WHITE);
        snprintf(buf, sizeof(buf), "[+] CONFIGURE RECORDED PIDs (%d Active)", getActivePidCount());
        canvas.drawString(buf, 50, 318);

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_CYAN);
        canvas.drawString("Tap here to customize parameters recorded to SD (10 Hz rate)", 50, 350);

        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Profile-defined vehicle signals and OBD-II standard PIDs supported", 50, 380);

        drawBottomNavBar();
        s_lastLoggerTick = millis();
    } else if (millis() - s_lastLoggerTick >= 500) {
        s_lastLoggerTick = millis();
        drawHeaderBar("PID VEHICLE DATALOGGER", false);

        if (currentLogMode == LOG_DATALOG) {
            char buf[96];
            unsigned long elapsedSec = (millis() - logStartTime) / 1000;
            canvas.fillRect(34, 104, 740, 24, canvas.color565(55, 30, 10));
            canvas.setFont(fonts::Font2);
            snprintf(buf, sizeof(buf), "REC: %s (%lu samples, %lum%02lus)", currentLogFileName, logEntryCount, elapsedSec / 60, elapsedSec % 60);
            canvas.setTextColor(canvas.color565(255, 230, 180));
            canvas.drawString(buf, 34, 106);
        }
    }
}

// Page 3: Wi-Fi & SavvyCAN Streaming (Wireless Cockpit)
void renderWiFi(bool forceFull = false) {
    static unsigned long s_lastWiFiTick = 0;
    static unsigned long s_lastStreamedCount = 0;
    static bool s_lastConn = false;

    if (forceFull) {
        drawHeaderBar("WI-FI SAVVYCAN STREAMING", true);

        canvas.fillRoundRect(12, 52, 776, 380, 8, C_CARD_BG);
        canvas.drawRoundRect(12, 52, 776, 380, 8, C_CARD_BORDER);
        canvas.fillRect(14, 52, 772, 4, C_TEXT_CYAN);

        canvas.setFont(fonts::Font4);

        // SSID
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Hotspot SSID:", 34, 70);
        canvas.setTextColor(C_TEXT_WHITE);
        canvas.drawString(WIFI_SSID, 320, 70);

        // Password
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Password:", 34, 114);
        canvas.setTextColor(C_TRD_ORANGE);
        canvas.drawString(WIFI_PASS, 320, 114);

        // SavvyCAN Server Port
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("SavvyCAN Server:", 34, 158);
        canvas.setTextColor(C_GREEN_OK);
        canvas.drawString("192.168.4.1:23", 320, 158);

        // Client Status Pill
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Client Status:", 34, 202);
        bool isConn = (savvyClient && savvyClient.connected());
        if (isConn) {
            char buf[48];
            snprintf(buf, sizeof(buf), "CONNECTED (%lu pkts)", wifiStreamedCount);
            canvas.setTextColor(C_GREEN_OK);
            canvas.drawString(buf, 320, 202);
        } else {
            canvas.setTextColor(C_TRD_ORANGE);
            canvas.drawString("Waiting for Laptop...", 320, 202);
        }

        canvas.drawFastHLine(34, 248, 732, C_CARD_BORDER);

        // Help Text Card
        canvas.fillRoundRect(34, 262, 732, 150, 6, C_CARD_INNER);
        canvas.drawRoundRect(34, 262, 732, 150, 6, C_CARD_BORDER);
        canvas.setFont(fonts::Font2);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("1. On your laptop, join the Wi-Fi hotspot SSID shown above.", 54, 280);
        canvas.drawString("2. In SavvyCAN: Add Network Connection -> host 192.168.4.1, port 23.", 54, 314);
        canvas.drawString("3. Live vehicle bus traffic streams wirelessly, no cables required.", 54, 348);
        canvas.drawString("4. Diagnostic PIDs and raw frames stream simultaneously.", 54, 382);

        drawBottomNavBar();
        s_lastWiFiTick = millis();
        s_lastConn = isConn;
        s_lastStreamedCount = wifiStreamedCount;
    } else if (millis() - s_lastWiFiTick >= 1000) {
        s_lastWiFiTick = millis();
        drawHeaderBar("WI-FI SAVVYCAN STREAMING", false);

        bool isConn = (savvyClient && savvyClient.connected());
        if (isConn != s_lastConn || (isConn && wifiStreamedCount != s_lastStreamedCount)) {
            s_lastConn = isConn;
            s_lastStreamedCount = wifiStreamedCount;
            canvas.fillRect(320, 198, 450, 32, C_CARD_BG);
            canvas.setFont(fonts::Font4);
            if (isConn) {
                char buf[48];
                snprintf(buf, sizeof(buf), "CONNECTED (%lu pkts)", wifiStreamedCount);
                canvas.setTextColor(C_GREEN_OK);
                canvas.drawString(buf, 320, 202);
            } else {
                canvas.setTextColor(C_TRD_ORANGE);
                canvas.drawString("Waiting for Laptop...", 320, 202);
            }
        }
    }
}

// Page 4: System Diagnostics & Hardware Health
void renderSystem(bool forceFull = false) {
    static unsigned long s_lastSysTick = 0;

    const int startRowY = 66;
    const int rowStep = 44;

    if (forceFull) {
        drawHeaderBar("HARDWARE DIAGNOSTICS", true);

        canvas.fillRoundRect(12, 52, 776, 380, 8, C_CARD_BG);
        canvas.drawRoundRect(12, 52, 776, 380, 8, C_CARD_BORDER);
        canvas.fillRect(14, 52, 772, 4, C_TRD_RED);

        char buf[64];
        canvas.setFont(fonts::Font4);

        // Row dividers
        for (int r = 1; r < 8; r++) {
            canvas.drawFastHLine(34, startRowY + r * rowStep - 8, 732, canvas.color565(26, 32, 44));
        }

        // Row 0: Firmware Version
        int ry = startRowY;
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Firmware Ver:", 34, ry);
        snprintf(buf, sizeof(buf), "%s (%s)", APP_VERSION_STR, APP_BUILD_DATE);
        canvas.setTextColor(C_TRD_ORANGE);
        canvas.drawString(buf, 340, ry);

        // Row 1: MCU Platform
        ry += rowStep;
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("MCU Platform:", 34, ry);
        snprintf(buf, sizeof(buf), "ESP32-S3 @ %u MHz", (unsigned int)getCpuFrequencyMhz());
        canvas.setTextColor(C_TEXT_WHITE);
        canvas.drawString(buf, 340, ry);

        // Row 2: Flash & PSRAM
        ry += rowStep;
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Flash & PSRAM:", 34, ry);
        snprintf(buf, sizeof(buf), "16MB Flash | %uMB OPI PSRAM", (unsigned int)(ESP.getPsramSize() / (1024 * 1024)));
        canvas.setTextColor(C_TEXT_WHITE);
        canvas.drawString(buf, 340, ry);

        // Row 3: Free Heap
        ry += rowStep;
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Free Heap:", 34, ry);
        snprintf(buf, sizeof(buf), "%u KB", (unsigned int)(ESP.getFreeHeap() / 1024));
        canvas.setTextColor(C_GREEN_OK);
        canvas.drawString(buf, 340, ry);

        // Row 4: Free PSRAM
        ry += rowStep;
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Free PSRAM:", 34, ry);
        snprintf(buf, sizeof(buf), "%u KB", (unsigned int)(ESP.getFreePsram() / 1024));
        canvas.setTextColor(C_GREEN_OK);
        canvas.drawString(buf, 340, ry);

        // Row 5: MicroSD
        ry += rowStep;
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("MicroSD Card:", 34, ry);
        snprintf(buf, sizeof(buf), "%s (%s)", sdMounted ? "Mounted FAT32" : "Unmounted", (currentLogMode != LOG_IDLE) ? "LOGGING" : "READY");
        canvas.setTextColor(sdMounted ? C_GREEN_OK : C_TRD_RED);
        canvas.drawString(buf, 340, ry);

        // Row 6: CAN Interface
        ry += rowStep;
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("CAN Interface:", 34, ry);
        snprintf(buf, sizeof(buf), "GPIO%d/GPIO%d (500k HS-CAN)", CAN_TX_PIN, CAN_RX_PIN);
        canvas.setTextColor(C_TEXT_CYAN);
        canvas.drawString(buf, 340, ry);

        // Row 7: RTC
        ry += rowStep;
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("RTC (PCF85063):", 34, ry);
        char stamp[24];
        if (rtcStamp(stamp, sizeof(stamp))) {
            canvas.setTextColor(C_GREEN_OK);
            canvas.drawString(stamp, 340, ry);
        } else {
            canvas.setTextColor(C_TRD_RED);
            canvas.drawString("NOT RESPONDING", 340, ry);
        }

        drawBottomNavBar();
        s_lastSysTick = millis();
    } else if (millis() - s_lastSysTick >= 1000) {
        s_lastSysTick = millis();
        drawHeaderBar("HARDWARE DIAGNOSTICS", false);

        char buf[64];
        canvas.setFont(fonts::Font4);

        // Update Free Heap (Row 3)
        canvas.fillRect(340, startRowY + 3 * rowStep - 2, 420, 28, C_CARD_BG);
        snprintf(buf, sizeof(buf), "%u KB", (unsigned int)(ESP.getFreeHeap() / 1024));
        canvas.setTextColor(C_GREEN_OK);
        canvas.drawString(buf, 340, startRowY + 3 * rowStep);

        // Update Free PSRAM (Row 4)
        canvas.fillRect(340, startRowY + 4 * rowStep - 2, 420, 28, C_CARD_BG);
        snprintf(buf, sizeof(buf), "%u KB", (unsigned int)(ESP.getFreePsram() / 1024));
        canvas.setTextColor(C_GREEN_OK);
        canvas.drawString(buf, 340, startRowY + 4 * rowStep);

        // Update RTC (Row 7)
        char stamp[24];
        canvas.fillRect(340, startRowY + 7 * rowStep - 2, 420, 28, C_CARD_BG);
        if (rtcStamp(stamp, sizeof(stamp))) {
            canvas.setTextColor(C_GREEN_OK);
            canvas.drawString(stamp, 340, startRowY + 7 * rowStep);
        } else {
            canvas.setTextColor(C_TRD_RED);
            canvas.drawString("NOT RESPONDING", 340, startRowY + 7 * rowStep);
        }
    }
}

// Page 5: Settings & Display Configuration
// =========================================================================
// Vehicle Profile selection (SD card profiles + NVS persistence)
// =========================================================================

// Load /profiles/<id>.json from SD into the profile engine and persist the
// choice in NVS ("prof"). id empty = clear selection, revert to built-in.
// NVS is written ONLY after the profile validates, so a typo'd or corrupt
// file can never leave NVS pointing at a selection that won't load.
static bool applyProfileSelection(const char* id) {
    if (id && id[0] != 0) {
        if (!sdMounted) return false;
        char profPath[64];
        snprintf(profPath, sizeof(profPath), "/profiles/%s.json", id);
        if (!SD.exists(profPath)) return false;
        File pf = SD.open(profPath, FILE_READ);
        if (!pf) return false;
        String profJson = pf.readString();
        pf.close();
        if (!loadProfile(profJson.c_str())) {
            Serial.println("[PROFILE] Profile JSON invalid - selection kept, built-in still active.");
            return false;
        }
        Serial.printf("[PROFILE] Profile active: %s (%s)\n", getProfileName(), getProfileId());
    } else {
        loadDefaultProfile();
        Serial.printf("[PROFILE] Reverted to built-in: %s (%s)\n", getProfileName(), getProfileId());
        id = "";
    }
    preferences.begin("dashview", false);
    preferences.putString("prof", id ? id : "");
    preferences.end();
    return true;
}

// SD profile picker: scan /profiles/*.json once per SD mount into a small table
static char g_profileIds[6][24];
static int  g_profileCount = 0;
static bool g_profileScanMounted = false;

static void scanProfileDir() {
    g_profileCount = 0;
    if (!sdMounted) { g_profileScanMounted = false; return; }
    File dir = SD.open("/profiles");
    if (!dir) { g_profileScanMounted = true; return; }
    File entry = dir.openNextFile();
    while (entry && g_profileCount < 6) {
        const char* nm = entry.name(); // full path like "/profiles/foo.json"
        size_t l = strlen(nm);
        if (!entry.isDirectory() && l > 5 && strcmp(nm + l - 5, ".json") == 0) {
            const char* base = strrchr(nm, '/');
            base = base ? base + 1 : nm;
            size_t idlen = strlen(base) - 5;
            if (idlen > 0 && idlen < 24) {
                strncpy(g_profileIds[g_profileCount], base, idlen);
                g_profileIds[g_profileCount][idlen] = 0;
                g_profileCount++;
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    g_profileScanMounted = true;
}

// Cell hit-test for the profile picker strip (4 cells at y 272-314).
// Returns: 0..2 = scanned profile index, 3 = BUILT-IN cell, -1 = miss.
static int profileCellHit(int x, int y) {
    if (y < 272 || y > 314) return -1;
    const int xs[4] = {34, 222, 410, 598};
    for (int i = 0; i < 4; i++)
        if (x >= xs[i] && x < xs[i] + 176) return i;
    return -1;
}

void renderSettings(bool forceFull = false) {
    if (!forceFull) return;
    drawHeaderBar("TOYOTA DASHVIEW - SETTINGS", true);

    char buf[64];

    // Card 1: Display Orientation (180-deg Flip)
    // Box: x=12, y=52, w=776, h=86
    canvas.fillRoundRect(12, 52, 776, 86, 8, C_CARD_BG);
    canvas.drawRoundRect(12, 52, 776, 86, 8, C_CARD_BORDER);
    canvas.fillRect(14, 52, 6, 86, C_TRD_RED);

    canvas.setFont(fonts::Font2);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawString("Display Orientation (180 deg Flip)", 34, 58);

    uint16_t flipBtnBg = isDisplayFlipped ? C_TRD_RED : canvas.color565(25, 35, 50);
    uint16_t flipBtnBorder = isDisplayFlipped ? canvas.color565(255, 100, 100) : canvas.color565(60, 100, 160);
    canvas.fillRoundRect(34, 88, 732, 40, 6, flipBtnBg);
    canvas.drawRoundRect(34, 88, 732, 40, 6, flipBtnBorder);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.setFont(fonts::Font4);
    snprintf(buf, sizeof(buf), "%s  (TAP TO FLIP)", isDisplayFlipped ? "FLIPPED 180 (INVERTED)" : "STANDARD 0 (NORMAL)");
    canvas.drawCenterString(buf, 400, 96);

    // Card 2: Backlight ON/OFF (4.3B backlight is a digital line on the
    // CH422G expander — there is no PWM dimming on this board)
    // Box: x=12, y=146, w=776, h=86
    canvas.fillRoundRect(12, 146, 776, 86, 8, C_CARD_BG);
    canvas.drawRoundRect(12, 146, 776, 86, 8, C_CARD_BORDER);
    canvas.fillRect(14, 146, 6, 86, C_TRD_ORANGE);

    canvas.setFont(fonts::Font2);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawString("Backlight (auto-dims after 60s idle)", 34, 152);

    uint16_t blBtnBg = backlightEnabled ? C_TRD_ORANGE : C_CARD_INNER;
    uint16_t blBtnBorder = backlightEnabled ? canvas.color565(255, 180, 50) : canvas.color565(60, 100, 160);
    canvas.fillRoundRect(34, 182, 732, 40, 6, blBtnBg);
    canvas.drawRoundRect(34, 182, 732, 40, 6, blBtnBorder);
    canvas.setTextColor(backlightEnabled ? TFT_BLACK : C_TEXT_MUTED);
    canvas.setFont(fonts::Font4);
    canvas.drawCenterString(backlightEnabled ? "STATE: ON  (TAP TO OFF)" : "STATE: OFF  (TAP TO ON)", 400, 190);

    // Card 3: Vehicle Profile picker. Tapping a cell hot-swaps the decode
    // engine (no reboot) and persists the choice in NVS key "prof".
    // Box: x=12, y=240, w=776, h=110
    canvas.fillRoundRect(12, 240, 776, 110, 8, C_CARD_BG);
    canvas.drawRoundRect(12, 240, 776, 110, 8, C_CARD_BORDER);
    canvas.fillRect(14, 240, 6, 110, canvas.color565(60, 160, 90));

    canvas.setFont(fonts::Font2);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawString("Vehicle Profile (SD: /profiles)", 34, 246);

    // Is the built-in profile the active selection? (NVS "prof" empty)
    preferences.begin("dashview", true);
    bool builtinActive = preferences.getString("prof", "").length() == 0;
    preferences.end();

    if (!g_profileScanMounted) {
        canvas.fillRoundRect(34, 272, 732, 42, 6, C_CARD_INNER);
        canvas.drawRoundRect(34, 272, 732, 42, 6, canvas.color565(60, 100, 160));
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.setFont(fonts::Font2);
        canvas.drawCenterString("No SD card or no /profiles folder - built-in profile active", 400, 286);
    } else {
        // 4 cells: up to 3 scanned SD profiles + fixed BUILT-IN
        const int cellX[4] = {34, 222, 410, 598};
        canvas.setFont(fonts::Font2);
        for (int i = 0; i < 4; i++) {
            bool active;
            char label[24];
            if (i < 3) {
                if (i >= g_profileCount) {
                    canvas.fillRoundRect(cellX[i], 272, 176, 42, 6, canvas.color565(18, 22, 30));
                    canvas.drawRoundRect(cellX[i], 272, 176, 42, 6, canvas.color565(36, 44, 58));
                    continue;
                }
                active = !builtinActive && strcmp(getProfileId(), g_profileIds[i]) == 0;
                snprintf(label, sizeof(label), "%.18s", g_profileIds[i]);
            } else {
                active = builtinActive;
                snprintf(label, sizeof(label), "BUILT-IN");
            }
            canvas.fillRoundRect(cellX[i], 272, 176, 42, 6, active ? C_TRD_RED : C_CARD_INNER);
            canvas.drawRoundRect(cellX[i], 272, 176, 42, 6, active ? canvas.color565(255, 120, 120) : canvas.color565(60, 100, 160));
            canvas.setTextColor(active ? C_TEXT_WHITE : C_TEXT_MUTED);
            canvas.drawCenterString(label, cellX[i] + 88, 286);
        }
    }

    canvas.setFont(fonts::Font2);
    canvas.setTextColor(C_TEXT_MUTED);
    snprintf(buf, sizeof(buf), "Active: %s [%s]", getProfileName(), getProfileId());
    canvas.drawString(buf, 34, 322);

    // Card 4: Reboot Controller
    // Box: x=12, y=358, w=776, h=52
    canvas.fillRoundRect(12, 358, 776, 52, 8, C_CARD_BG);
    canvas.drawRoundRect(12, 358, 776, 52, 8, C_CARD_BORDER);
    canvas.fillRect(14, 358, 6, 52, C_TRD_RED);

    canvas.fillRoundRect(34, 362, 732, 44, 6, C_CARD_INNER);
    canvas.drawRoundRect(34, 362, 732, 44, 6, canvas.color565(160, 40, 50));
    canvas.setTextColor(canvas.color565(255, 90, 90));
    canvas.setFont(fonts::Font4);
    canvas.drawCenterString("REBOOT CONTROLLER", 400, 372);

    drawBottomNavBar();
}

// Copy FRESH profile signals (decoded by profile.cpp from broadcasts and OBD
// responses) into the legacy gauge struct. A signal is trusted only if it was
// updated within the last 1.5 s, so stale values never overwrite live ones
// when a profile omits a signal or the bus goes quiet.
static void syncProfileSignals() {
    unsigned long now = millis();
    if (signalAge("rpm", now) < 1500)          vehicleData.rpm = (int)getSignal("rpm");
    if (signalAge("speed", now) < 1500) {
        vehicleData.speedMph = (int)getSignal("speed");
    } else if (signalAge("speed_kmh", now) < 1500) {
        vehicleData.speedMph = (int)(getSignal("speed_kmh") * 0.621371f);
    }
    if (signalAge("throttle", now) < 1500)       vehicleData.throttlePct = (int)getSignal("throttle");
    if (signalAge("load", now) < 1500)           vehicleData.engineLoadPct = (int)getSignal("load");
    if (signalAge("coolant", now) < 1500)        vehicleData.coolantTempC = (int)getSignal("coolant");
    if (signalAge("iat", now) < 1500)            vehicleData.iatC = (int)getSignal("iat");
    if (signalAge("maf", now) < 1500)            vehicleData.mafGps = getSignal("maf");
    if (signalAge("timing", now) < 1500)         vehicleData.timingDeg = getSignal("timing");
    if (signalAge("afr_actual", now) < 1500)     vehicleData.actualAfr = getSignal("afr_actual");
    if (signalAge("afr_commanded", now) < 1500)  vehicleData.commandedAfr = getSignal("afr_commanded");
    if (signalAge("kclv", now) < 1500)           vehicleData.kclv = getSignal("kclv");
    if (signalAge("knockfb", now) < 1500)        vehicleData.knockFB = getSignal("knockfb");
    if (signalAge("tcc_locked", now) < 1500)     vehicleData.tccLocked = getSignal("tcc_locked") >= 0.5f;
    const char* g = (signalAge("gear", now) < 1500) ? getSignalText("gear") : nullptr;
    if (g && g[0]) {
        strncpy(vehicleData.gear, g, sizeof(vehicleData.gear) - 1);
        vehicleData.gear[sizeof(vehicleData.gear) - 1] = '\0';
    }
}

void updateDisplay() {
    static DisplayScreen s_lastScreen = (DisplayScreen)-1;
    static bool s_lastEditMode = false;
    static int s_lastEditorKind = -1;
    static bool s_lastPidConfig = false;
    static bool s_lastSnifferModal = false;
    static bool s_lastFlipped = false;
    static bool s_lastBl = true;
    static const char* s_lastProfileId = nullptr;

    const char* curProf = getProfileId();
    bool profChanged = (s_lastProfileId != curProf) && 
                       (!s_lastProfileId || !curProf || strcmp(s_lastProfileId, curProf) != 0);

    bool screenChanged = (currentScreen != s_lastScreen) ||
                         (g_cdEditMode != s_lastEditMode) ||
                         (g_cdEditorKind != s_lastEditorKind) ||
                         (isPidConfigOpen != s_lastPidConfig) ||
                         (isRawSnifferModalOpen != s_lastSnifferModal) ||
                         (isDisplayFlipped != s_lastFlipped) ||
                         (backlightEnabled != s_lastBl) ||
                         profChanged;

    if (screenChanged) {
        canvas.beginOffscreen();

        bool isModal = isPidConfigOpen || isRawSnifferModalOpen;
        s_lastScreen = currentScreen;
        s_lastEditMode = g_cdEditMode;
        s_lastEditorKind = (int)g_cdEditorKind;
        s_lastPidConfig = isPidConfigOpen;
        s_lastSnifferModal = isRawSnifferModalOpen;
        s_lastFlipped = isDisplayFlipped;
        s_lastBl = backlightEnabled;
        s_lastProfileId = curProf;

        if (isModal) {
            canvas.fillScreen(C_DARK_BG);
        } else {
            canvas.fillContentArea(C_DARK_BG);
            drawBottomNavBar(true);
        }
    }

    switch (currentScreen) {
        case SCREEN_DASHBOARD: renderDashboard(screenChanged);     break;
        case SCREEN_CUSTOM:    renderCustomDash(screenChanged);    break;
        case SCREEN_SNIFFER:   renderSniffer(screenChanged);       break;
        case SCREEN_LOGGER:    renderLoggerControl(screenChanged); break;
        case SCREEN_WIFI:      renderWiFi(screenChanged);          break;
        case SCREEN_SYSTEM:    renderSystem(screenChanged);        break;
        case SCREEN_SETTINGS:  renderSettings(screenChanged);      break;
        default:               renderDashboard(screenChanged);     break;
    }

    if (screenChanged) {
        canvas.endOffscreen();
    }
}

// =========================================================================
// Official TRD Red Boot Splash (LVGL 9 PNG Decoded via lodepng)
// =========================================================================
void showToyotaBootSplash() {
    canvas.beginOffscreen();
    if (!drawToyotaBootSplash(canvas)) {
        Serial.println("[SPLASH] Splash render failed — clearing to black.");
        canvas.fillScreen(0);
    }
    canvas.endOffscreen();
    isBootSplashActive = true;
}

// =========================================================================
// Screen Navigation Functions
// =========================================================================
void nextScreen() {
    if (isPidConfigOpen) return;
    currentScreen = static_cast<DisplayScreen>((currentScreen + 1) % SCREEN_COUNT);
    wakeScreen();
    lastUserActivityTime = millis();
    updateDisplay();
    Serial.printf("[SWIPE] Switched to Next Screen -> Page %d\n", currentScreen);
}

void prevScreen() {
    if (isPidConfigOpen) return;
    currentScreen = static_cast<DisplayScreen>((currentScreen - 1 + SCREEN_COUNT) % SCREEN_COUNT);
    wakeScreen();
    lastUserActivityTime = millis();
    updateDisplay();
    Serial.printf("[SWIPE] Switched to Prev Screen <- Page %d\n", currentScreen);
}

// =========================================================================
// Capacitive Touch & Gesture / Button Tap Engine
// =========================================================================
#define SWIPE_MIN_DIST_PX 120  // Comfortable swipe distance on 800px display

void handleTouch() {
    int touchX = 0, touchY = 0;
    bool touched = pollTouch(touchX, touchY);

    if (touched) {
        wakeScreen();
        if (!wasTouched) {
            wasTouched = true;
            touchStartX = touchX;
            touchStartY = touchY;
            touchLastX  = touchX;
            touchLastY  = touchY;
            touchStartTime = millis();
            Serial.printf("[TOUCH] Press at (%d, %d)\n", touchX, touchY);
            if (currentScreen == SCREEN_CUSTOM) cdHandlePress(touchX, touchY);
        } else {
            touchLastX = touchX;
            touchLastY = touchY;
            if (cdTouchActive()) cdHandleDrag(touchX, touchY);
        }
    } else if (wasTouched) {
        wasTouched = false;
        int deltaX = touchLastX - touchStartX;
        int deltaY = touchLastY - touchStartY;
        unsigned long duration = millis() - touchStartTime;

        // Custom Dash drags / editor taps consume the whole gesture
        if (currentScreen == SCREEN_CUSTOM || g_cdEditorKind != CD_EDIT_NONE) {
            if (cdHandleRelease(touchLastX, touchLastY)) return;
        }

        Serial.printf("[TOUCH] Release at (%d, %d) | deltaX=%d, deltaY=%d, dur=%lums\n", 
                      touchLastX, touchLastY, deltaX, deltaY, duration);

        // 1. Horizontal Swipe (>= 120px)
        if (abs(deltaX) >= SWIPE_MIN_DIST_PX && !isPidConfigOpen && !isRawSnifferModalOpen) {
            if (deltaX <= -SWIPE_MIN_DIST_PX) {
                nextScreen(); // Drag right-to-left -> Next Page
                Serial.printf("[SWIPE] Left swipe (%d px) -> Next Screen (%d)\n", deltaX, currentScreen);
            } else if (deltaX >= SWIPE_MIN_DIST_PX) {
                prevScreen(); // Drag left-to-right -> Prev Page
                Serial.printf("[SWIPE] Right swipe (%d px) -> Prev Screen (%d)\n", deltaX, currentScreen);
            }
            return;
        }

        // 2. Stationary Button / Card Tap (Minimal movement < 35px, duration < 1200ms)
        if (abs(deltaX) < 35 && abs(deltaY) < 35 && duration < 1200) {
            // Check Bottom Navigation Bar first (< PREV, NEXT >, or page dots)
            if (!isPidConfigOpen && !isRawSnifferModalOpen && g_cdEditorKind == CD_EDIT_NONE && touchLastY >= (UI_H - UI_NAVBAR_H - 15)) {
                if (touchLastX <= 220) {
                    prevScreen();
                    return;
                } else if (touchLastX >= (UI_W - 220)) {
                    nextScreen();
                    return;
                } else {
                    int dotSpacing = 24;
                    int startDotX = 400 - (((SCREEN_COUNT - 1) * dotSpacing) / 2);
                    for (int i = 0; i < SCREEN_COUNT; i++) {
                        int dx = startDotX + (i * dotSpacing);
                        if (abs(touchLastX - dx) <= 16) {
                            currentScreen = static_cast<DisplayScreen>(i);
                            wakeScreen();
                            lastUserActivityTime = millis();
                            updateDisplay();
                            Serial.printf("[NAVBAR] Tapped Page %d\n", currentScreen);
                            return;
                        }
                    }
                }
                return;
            }
            // A. Page 1 (CAN Sniffer Page)
            if (currentScreen == SCREEN_SNIFFER) {
                // When Floating Raw Packet Terminal is Open
                if (isRawSnifferModalOpen) {
                    // Button 1: [ PAUSE / RESUME ] (x: 12..192, y: 412..456)
                    if (touchLastX >= 12 && touchLastX <= 192 && touchLastY >= 412 && touchLastY <= 456) {
                        isSnifferPaused = !isSnifferPaused;
                        Serial.printf("[SNIFFER MODAL] Toggled Pause -> %s\n", isSnifferPaused ? "PAUSED" : "STREAMING");
                        return;
                    }
                    // Button 2: [ CLEAR ] (x: 204..364, y: 412..456)
                    else if (touchLastX >= 204 && touchLastX <= 364 && touchLastY >= 412 && touchLastY <= 456) {
                        for (int i = 0; i < SNIFFER_HISTORY_SIZE; i++) {
                            snifferHistory[i].id = 0;
                            snifferHistory[i].dlc = 0;
                        }
                        snifferHead = 0;
                        Serial.println("[SNIFFER MODAL] Cleared history buffer.");
                        return;
                    }
                    // Button 3: [ BACK / CLOSE ] (x: 376..788, y: 412..456)
                    else if (touchLastX >= 376 && touchLastX <= 788 && touchLastY >= 412 && touchLastY <= 456) {
                        isRawSnifferModalOpen = false;
                        Serial.println("[SNIFFER MODAL] Closed modal -> Returning to Sniffer Page.");
                        return;
                    }
                    return;
                }
                // When Normal Page 1 Sniffer View is Open
                else {
                    // Card 1: CAN Logger Start / Stop (y: 52 - 138)
                    if (touchLastY >= 52 && touchLastY <= 138) {
                        if (currentLogMode == LOG_CANBUS) {
                            stopActiveLogger();
                        } else if (currentLogMode == LOG_IDLE) {
                            startCanbusLogger();
                        }
                        return;
                    }
                    // Card 3: Open Raw Packet Terminal (y: 270 - 430)
                    else if (touchLastY >= 270 && touchLastY <= 430) {
                        isRawSnifferModalOpen = true;
                        Serial.println("[SNIFFER] Opened Floating Raw Packet Terminal.");
                        return;
                    }
                }
            }
            // B. Page 2 (PID Datalogger Screen)
            else if (currentScreen == SCREEN_LOGGER) {
                // When PID Config Modal is Open
                if (isPidConfigOpen) {
                    int startY = 52;
                    int rowHeight = 42;
                    int colWidth = 250;

                    for (int slot = 0; slot < DL_PICKER_PER_PAGE; slot++) {
                        int sigIdx = g_dlPickerPage * DL_PICKER_PER_PAGE + slot;
                        const SignalValue* s = getSignalByIndex(sigIdx);
                        if (!s) break;
                        int col = (slot % 3);
                        int row = (slot / 3);
                        int bx = 12 + col * (colWidth + 8);
                        int by = startY + row * (rowHeight + 6);

                        if (touchLastX >= bx && touchLastX <= bx + colWidth &&
                            touchLastY >= by && touchLastY <= by + rowHeight) {
                            dlSelToggleSig(sigIdx);
                            Serial.printf("[DL PICKER] Toggled %s\n", s->key);
                            return;
                        }
                    }

                    int botY = 412;
                    if (touchLastY >= botY && touchLastY <= botY + 44) {
                        int pages = (getSignalCount() + DL_PICKER_PER_PAGE - 1) / DL_PICKER_PER_PAGE;
                        if (touchLastX >= 12 && touchLastX <= 112) {
                            if (g_dlPickerPage > 0) g_dlPickerPage--;
                        } else if (touchLastX >= 124 && touchLastX <= 224) {
                            if (g_dlPickerPage + 1 < pages) g_dlPickerPage++;
                        } else if (touchLastX >= 236 && touchLastX <= 376) {
                            dlSelAll();
                        } else if (touchLastX >= 388 && touchLastX <= 528) {
                            dlSelNone();
                        } else if (touchLastX >= 540 && touchLastX <= 788) {
                            isPidConfigOpen = false;
                        }
                        return;
                    }
                }
                // When Normal Page 2 View is Open
                else {
                    // Card 1: PID Datalogger Start / Stop (y: 52 - 138)
                    if (touchLastY >= 52 && touchLastY <= 138) {
                        if (currentLogMode == LOG_DATALOG) {
                            stopActiveLogger();
                        } else if (currentLogMode == LOG_IDLE) {
                            startDataLogger();
                        }
                        return;
                    }
                    // Card 3: Configure PIDs Button (y: 270 - 430)
                    else if (touchLastY >= 270 && touchLastY <= 430) {
                        if (currentLogMode == LOG_IDLE) {
                            isPidConfigOpen = true;
                            Serial.println("[PID PICKER] Opened PID Config Screen.");
                        }
                        return;
                    }
                }
            }
            // C. Page 5 (Settings Page)
            else if (currentScreen == SCREEN_SETTINGS) {
                // Card 1: 180-deg Display Flip (y: 52 - 138)
                if (touchLastY >= 52 && touchLastY <= 138) {
                    saveDisplayFlipSetting(!isDisplayFlipped);
                    return;
                }
                // Card 2: Backlight ON/OFF toggle (y: 146 - 232)
                else if (touchLastY >= 146 && touchLastY <= 232) {
                    saveBacklightSetting(!backlightEnabled);
                    return;
                }
                // Card 3: Vehicle Profile cells (y: 272 - 314)
                else if (touchLastY >= 272 && touchLastY <= 314 && g_profileScanMounted) {
                    int cell = profileCellHit(touchLastX, touchLastY);
                    if (cell >= 0) {
                        if (cell == 3) {
                            applyProfileSelection("");
                        } else if (cell < g_profileCount) {
                            if (!applyProfileSelection(g_profileIds[cell]))
                                Serial.printf("[PROFILE] Select '%s' failed - keeping current profile.\n", g_profileIds[cell]);
                        }
                    }
                    return;
                }
                // Card 4: Reboot Controller (y: 358 - 410)
                else if (touchLastY >= 358 && touchLastY <= 410) {
                    Serial.println("[SETTINGS] Reboot requested -> Restarting ESP32...");
                    delay(200);
                    ESP.restart();
                    return;
                }
            }
        }
    }
}

// =========================================================================
// Main Loop & CAN Processing
// =========================================================================
void processCAN() {
    // Non-blocking alert check: react to bus-off and note RX overruns instead
    // of silently losing frames when TCP/SD backpressure slows the drain.
    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) == ESP_OK && alerts) {
        if (alerts & TWAI_ALERT_BUS_OFF) tryCanRecovery();
        if (alerts & TWAI_ALERT_ERR_PASS)
            noteTxError("error-passive");
        if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
            rxOverflowCount++;
            Serial.println("[CAN] RX queue full - frames dropped!");
        }
    }

    twai_message_t message;
    int drained = 0;
    // Bound the drain per loop pass: each frame can block on a TCP write or an
    // SD printf, and an unbounded loop starves touch/UI under full bus load.
    while (drained++ < 64 && twai_receive(&message, 0) == ESP_OK) {
        packetCount++;
        ppsCount++;
        lastCanActivityTime = millis();

        streamFrameToSavvyCAN(message);
        decodeTacomaFrame(message);
        recordSnifferFrame(message);

        // Mode A: Log Raw CAN Bus Frame (if CAN Logger is active)
        if (currentLogMode == LOG_CANBUS && activeLogFile) {
            logEntryCount++;
            activeLogFile.printf("%lu,0x%03lX,%d,%d,", millis(), (unsigned long)message.identifier, message.extd, message.data_length_code);
            for (int i = 0; i < message.data_length_code; i++) {
                activeLogFile.printf("%02X", message.data[i]);
                if (i < message.data_length_code - 1) activeLogFile.print(" ");
            }
            activeLogFile.println();

            if (logEntryCount % 50 == 0 || (millis() - lastLogFlushTime >= 1000)) {
                activeLogFile.flush();
                lastLogFlushTime = millis();
            }
        }
    }
}

void processDatalogging() {
    // Mode B: Log Selected Vehicle PIDs at 10 Hz (every 100ms)
    if (currentLogMode == LOG_DATALOG && activeLogFile && (millis() - lastDatalogSampleTime >= 100)) {
        lastDatalogSampleTime = millis();
        logEntryCount++;

        String row = String(millis());
        unsigned long now = millis();
        for (int i = 0; i < getSignalCount(); i++) {
            const SignalValue* s = getSignalByIndex(i);
            if (!s || !dlLogThis(s->key)) continue;
            row += ',';
            // 5 s gate: the OBD poller rotates ~4 queries/s, so a polled
            // signal legitimately refreshes only every few seconds. Blanking
            // at 1.5 s (display freshness) would leave the CSV mostly empty;
            // 5 s still catches key-off / silent bus.
            if (s->valid && signalAge(s->key, now) < 5000) {
                if (s->hasText) {
                    row += s->text;                         // enum text (gear...)
                } else {
                    SignalMeta m{};
                    getSignalMeta(s->key, &m);
                    char vbuf[16];
                    snprintf(vbuf, sizeof(vbuf), "%.*f", m.decimals, s->value);
                    row += vbuf;
                }
            }
        }

        activeLogFile.println(row);

        if (logEntryCount % 20 == 0 || (millis() - lastLogFlushTime >= 1000)) {
            activeLogFile.flush();
            lastLogFlushTime = millis();
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n=== %s %s (ESP32-S3 Touch LCD 4.3B) ===\n", APP_NAME, APP_VERSION_STR);

    // 0. Load persistent settings (180-deg flip & backlight)
    loadSettings();
    loadDefaultProfile();
    Serial.printf("[PROFILE] Vehicle Profile Loaded: %s (%s)\n", getProfileName(), getProfileId());

    // 1. Shared I2C bus: GT911 touch + CH422G expander + PCF85063 RTC
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);

    // 2. CH422G gates several subsystems, so it comes up first.
    initCh422g();
    backlightOff();                     // stay dark until the first frame is on screen
    ch422gSetPin(EXIO_SD_CS, true);     // release SD CS until mountSD()
    ch422gSetPin(EXIO_USB_SEL, false);  // USB stays on the ESP32-S3

    // 3. Release LCD + touch-panel resets (active-low on the expander)
    ch422gSetPin(EXIO_LCD_RST, false); delay(20);  ch422gSetPin(EXIO_LCD_RST, true);
    ch422gSetPin(EXIO_TP_RST, false);  delay(120); ch422gSetPin(EXIO_TP_RST, true);
    delay(200);

    // 4. Display: LVGL 9 + esp_lcd_panel_rgb on a single PSRAM framebuffer,
    //    scanned out through SRAM bounce buffers (the flicker fix). LVGL renders
    //    DIRECT into that FB — widgets repaint dirty rects in place.
    if (!canvas.init(800, 480, 14000000)) {
        Serial.println("[DISPLAY] LVGL panel init FAILED — halting.");
        while (true) { delay(1000); }
    }
    canvas.setRotation(isDisplayFlipped ? 2 : 0);
    Serial.printf("[DISPLAY] Panel ready: %dx%d, PSRAM frame buffer: %s\n",
                  canvas.width(), canvas.height(), psramFound() ? "yes" : "NO (memory_type mismatch!)");
    fontsInit();                         // resolve fonts::Font0/2/4/7 to LVGL Montserrat

    backlightOn();
    displayTouchInit();                  // GT911 -> LVGL indev (11a218d fix kept)
    initRtc();

    // 5. Boot splash: "DashView" wordmark, canvas-drawn (no PNG decode)
    if (!drawToyotaBootSplash(canvas)) {
        Serial.println("[SPLASH] Splash render failed — clearing to black.");
        canvas.fillScreen(0);
    }

    // 6. Wi-Fi SoftAP & SavvyCAN Streaming Server
    initWiFiStreaming();

    // 7. CAN Bus & MicroSD
    initCAN();
    mountSD();

    // 7b. Vehicle profile override: /profiles/<id>.json on SD, id chosen in
    // NVS key "prof". The built-in default loaded in step 0 stays in effect
    // when no card / no selection / invalid JSON.
    scanProfileDir();
    if (sdMounted) {
        preferences.begin("dashview", true);
        String profId = preferences.getString("prof", "");
        preferences.end();
        if (profId.length() > 0) {
            char profPath[64];
            snprintf(profPath, sizeof(profPath), "/profiles/%s.json", profId.c_str());
            bool sdProfileLoaded = false;
            if (SD.exists(profPath)) {
                File pf = SD.open(profPath, FILE_READ);
                if (pf) {
                    String profJson = pf.readString();
                    pf.close();
                    sdProfileLoaded = loadProfile(profJson.c_str());
                    if (!sdProfileLoaded)
                        Serial.println("[PROFILE] SD profile JSON invalid - keeping built-in default.");
                }
            } else {
                Serial.printf("[PROFILE] NVS profile '%s' not found on SD (%s) - using built-in default.\n", profId.c_str(), profPath);
            }
            if (sdProfileLoaded)
                Serial.printf("[PROFILE] SD profile loaded: %s (%s)\n", getProfileName(), getProfileId());
        }
    }

    // 8. Custom Dash: load saved gauge layout
    cdLoadPrefs();
    if (g_cdGaugeCount == 0) cdSeedDefaults();

    lastUserActivityTime = millis();
}

void loop() {
    if (Serial.available()) {
        char cmd = (char)Serial.read();
        if (cmd == 'r') {
            Serial.println("[SYSTEM] Soft rebooting...");
            delay(100);
            ESP.restart();
        } else if (cmd == 't') {
            int tx = 0, ty = 0;
            bool touched = pollTouch(tx, ty);
            Serial.printf("[TOUCH-DEBUG] addr=0x%02X touched=%d at (%d, %d)\n", displayTouchGetAddr(), touched, tx, ty);
        } else if (cmd == 's') {
            Serial.printf("[STATUS] Screen=%d PPS=%.1f Heap=%lu PSRAM=%lu\n",
                          currentScreen, currentPPS, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
        } else if (cmd == 'c' || cmd == 'C') {
            uint8_t* fb = canvas.frameBuffer();
            if (fb) {
                Serial.println("\n---SCREENSHOT:START:800:480:RGB565---");
                Serial.flush();
                const size_t total = 800 * 480 * 2;
                const size_t chunk = 4096;
                for (size_t i = 0; i < total; i += chunk) {
                    size_t toWrite = total - i;
                    if (toWrite > chunk) toWrite = chunk;
                    Serial.write(fb + i, toWrite);
                    delayMicroseconds(50);
                }
                Serial.flush();
                Serial.println("\n---SCREENSHOT:END---");
            } else {
                Serial.println("\n---SCREENSHOT:ERROR:NO_FB---");
            }
        } else if (cmd >= '0' && cmd <= '6') {
            currentScreen = (DisplayScreen)(cmd - '0');
            isBootSplashActive = false;
            wakeScreen();
            lastUserActivityTime = millis();
            updateDisplay();
            Serial.printf("[SCREEN] Switched to screen %d\n", currentScreen);
        } else if (cmd == 'm') {
            vehicleData.rpm = 2450;
            vehicleData.speedMph = 45;
            strcpy(vehicleData.gear, "4");
            vehicleData.tccLocked = true;
            vehicleData.commandedAfr = 14.7f;
            vehicleData.actualAfr = 14.65f;
            vehicleData.kclv = 20.0f;
            vehicleData.knockFB = 0.0f;
            vehicleData.throttlePct = 32;
            vehicleData.engineLoadPct = 48;
            vehicleData.coolantTempC = 88;
            vehicleData.iatC = 26;
            vehicleData.mafGps = 22.4f;
            vehicleData.timingDeg = 16.0f;
            currentPPS = 185.0f;
            packetCount = 84210;
            wakeScreen();
            lastUserActivityTime = millis();
            updateDisplay();
            Serial.println("[MOCK] Injected active driving telemetry");
        } else if (cmd == 'z') {
            vehicleData.rpm = 0;
            vehicleData.speedMph = 0;
            strcpy(vehicleData.gear, "P");
            vehicleData.tccLocked = false;
            vehicleData.commandedAfr = 14.7f;
            vehicleData.actualAfr = 14.7f;
            vehicleData.kclv = 20.0f;
            vehicleData.knockFB = 0.0f;
            vehicleData.throttlePct = 0;
            vehicleData.engineLoadPct = 0;
            vehicleData.coolantTempC = 88;
            vehicleData.iatC = 25;
            vehicleData.mafGps = 0.0f;
            vehicleData.timingDeg = 10.0f;
            currentPPS = 0.0f;
            wakeScreen();
            lastUserActivityTime = millis();
            updateDisplay();
            Serial.println("[MOCK] Cleared telemetry to idle");
        }
    }

    if (isBootSplashActive) {
        handleWiFiClients();
        processCAN();

        // 1. Check if screen tapped
        int touchX = 0, touchY = 0;
        if (pollTouch(touchX, touchY)) {
            currentScreen = SCREEN_DASHBOARD; // ALWAYS enter Dashboard (Page 0)
            isBootSplashActive = false;
            wasTouched = false;
            wakeScreen();
            lastUserActivityTime = millis();
            Serial.println("[SPLASH] Screen tapped -> Exiting splash to Main Dashboard (Page 0).");
            delay(120);
            return;
        }

        // 2. Check if engine started (RPM > 0 or Speed > 0)
        if (vehicleData.rpm > 0 || vehicleData.speedMph > 0) {
            currentScreen = SCREEN_DASHBOARD; // ALWAYS enter Dashboard (Page 0)
            isBootSplashActive = false;
            wasTouched = false;
            wakeScreen();
            lastUserActivityTime = millis();
            Serial.printf("[SPLASH] Engine started (RPM: %d) -> Exiting splash to Main Dashboard (Page 0).\n", vehicleData.rpm);
            return;
        }

        // 3. Auto-dismiss splash after 3 seconds timeout
        if (millis() > 3000) {
            currentScreen = SCREEN_DASHBOARD;
            isBootSplashActive = false;
            wasTouched = false;
            wakeScreen();
            lastUserActivityTime = millis();
            Serial.println("[SPLASH] Splash timed out (3s) -> Auto-entering Main Dashboard.");
            return;
        }

        return;
    }

    handleTouch();
    handleWiFiClients();
    processCAN();
    processDatalogging();

    // Auto-Dim to 15% brightness after 60s of inactivity
    if (!isScreenDimmed && (millis() - lastUserActivityTime >= SCREEN_TIMEOUT_MS) && (millis() - lastCanActivityTime >= SCREEN_TIMEOUT_MS)) {
        dimScreen();
    }

    // Periodic Toyota OBD-II active queries
    // TX failsafe: obdTxCleared() pauses polling on any bus error activity.
    if (millis() - lastCanActivityTime < 3000 && (millis() - lastObdQueryTime >= 250) && obdTxCleared()) {
        lastObdQueryTime = millis();
        sendToyotaObdQueries();
    }

    // MicroSD retry
    if (!sdMounted && (millis() - lastSdRetryTime >= 5000)) {
        lastSdRetryTime = millis();
        if (mountSD()) scanProfileDir();
    }

    // Update Message Rate calculation
    if (millis() - lastPPSCheck >= 1000) {
        currentPPS = (float)ppsCount * 1000.0f / (millis() - lastPPSCheck);
        ppsCount = 0;
        lastPPSCheck = millis();
    }

    // 30 FPS Display Refresh
    static uint32_t lastFrameTime = 0;
    const uint32_t frameInterval = 33; // ~30 FPS (1000ms / 30)
    if (millis() - lastFrameTime >= frameInterval) {
        lastFrameTime = millis();
        syncProfileSignals();
        updateDisplay();
    }
}
