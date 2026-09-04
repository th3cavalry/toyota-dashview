#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include "driver/twai.h"
#include <LovyanGFX.hpp>
#include "version.h"
#include "toyota_splash.h"

// Persistent Settings (Flash NVS)
Preferences preferences;
bool isDisplayFlipped = false; // Persistent: false = normal, true = 180 deg (software push)
bool backlightEnabled = true;  // Persistent: CH422G digital backlight (no PWM on 4.3B)

// Forward decls (defined with the CH422G / display sections below)
void backlightOn();
void backlightOff();
void pushCanvasToPanel();

// =========================================================================
// Waveshare ESP32-S3-Touch-LCD-4.3B Hardware Configuration
// 800x480 ST7265 RGB panel (frame buffer in 8MB OPI PSRAM), GT911 capacitive
// touch, CH422G IO expander (backlight / SD CS / touch reset) and a PCF85063
// RTC sharing I2C on GPIO8/9, onboard CAN transceiver on GPIO15/16.
// =========================================================================
class LGFX_Waveshare43B : public lgfx::LGFX_Device {
    lgfx::Panel_RGB _panel_instance;
    lgfx::Bus_RGB   _bus_instance;

public:
    LGFX_Waveshare43B(void) {
        {
            auto cfg = _panel_instance.config();
            cfg.memory_width  = 800;
            cfg.memory_height = 480;
            cfg.panel_width   = 800;
            cfg.panel_height  = 480;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            _panel_instance.config(cfg);
        }

        {
            // Frame buffer lives in PSRAM (panel refreshes via GDMA).
            auto cfg = _panel_instance.config_detail();
            cfg.use_psram = 1;
            _panel_instance.config_detail(cfg);
        }

        {
            auto cfg = _bus_instance.config();
            cfg.panel = &_panel_instance;
            // RGB565 data lines (esp_lcd DATA0..15 -> LGFX d0..d15, LSB first)
            cfg.pin_d0  = 14; // B0
            cfg.pin_d1  = 38; // B1
            cfg.pin_d2  = 18; // B2
            cfg.pin_d3  = 17; // B3
            cfg.pin_d4  = 10; // B4
            cfg.pin_d5  = 39; // G0
            cfg.pin_d6  = 0;  // G1
            cfg.pin_d7  = 45; // G2
            cfg.pin_d8  = 48; // G3
            cfg.pin_d9  = 47; // G4
            cfg.pin_d10 = 21; // G5
            cfg.pin_d11 = 1;  // R0
            cfg.pin_d12 = 2;  // R1
            cfg.pin_d13 = 42; // R2
            cfg.pin_d14 = 41; // R3
            cfg.pin_d15 = 40; // R4
            cfg.pin_henable = 5;  // DE
            cfg.pin_vsync   = 3;
            cfg.pin_hsync   = 46;
            cfg.pin_pclk    = 7;
            cfg.freq_write  = 16000000; // 16 MHz pixel clock (Waveshare timing)
            cfg.hsync_polarity    = 0;
            cfg.hsync_front_porch = 8;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch  = 8;
            cfg.vsync_polarity    = 0;
            cfg.vsync_front_porch = 16;
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch  = 16;
            cfg.pclk_active_neg   = 1;
            cfg.pclk_idle_high    = 0;
            // Bounce buffer keeps PSRAM bandwidth spikes from tearing the panel.
            cfg.bounce_buffer_size_px = 800 * 10;
            _bus_instance.config(cfg);
        }
        _panel_instance.setBus(&_bus_instance);

        setPanel(&_panel_instance);
    }
};

LGFX_Waveshare43B tft;
LGFX_Sprite canvas(&tft); // PSRAM canvas; pushed to the panel at 30 FPS

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

// CH422G extended-IO bit positions (EXIO1..EXIO8 -> bits 0..7)
#define EXIO_TP_RST        0
#define EXIO_LCD_BL        1
#define EXIO_LCD_RST       2
#define EXIO_SD_CS         3
#define EXIO_USB_SEL       4

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

bool isPidConfigOpen = false;        // Is the PID selection modal/view open
bool isRawSnifferModalOpen = false;  // Is the floating raw packet terminal modal open
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

// 6 Dedicated Full-Color UI Screens
enum DisplayScreen {
    SCREEN_DASHBOARD = 0,
    SCREEN_SNIFFER   = 1,
    SCREEN_LOGGER    = 2, // Dedicated Datalog & CAN Logger Control Page
    SCREEN_WIFI      = 3,
    SCREEN_SYSTEM    = 4,
    SCREEN_SETTINGS  = 5, // Settings & 180-deg Display Flip Page
    SCREEN_COUNT     = 6
};
DisplayScreen currentScreen = SCREEN_DASHBOARD;

// Swipe Gesture Detection State
bool wasTouched = false;
int touchStartX = 0;
int touchStartY = 0;
int touchLastX = 0;
int touchLastY = 0;
unsigned long touchStartTime = 0;

// Live Vehicle Telemetry
struct TacomaTelemetry {
    char gear[4] = "P";         // P, R, N, 1, 2, 3, 4, 5, 6
    bool tccLocked = false;     // Torque Converter Lockup (TCC)
    int rpm = 0;
    int speedMph = 0;
    float commandedAfr = 14.7f; // Target / Commanded AFR
    float actualAfr = 14.7f;    // Live Wideband A/F Sensor AFR
    float kclv = 20.0f;         // Knock Correct Learn Value
    float knockFB = 0.0f;       // Knock Feedback (deg)
    int throttlePct = 0;        // Throttle %
    int engineLoadPct = 0;      // Calculated Engine Load %
    int coolantTempC = 88;      // Coolant Temp °C
    int iatC = 25;              // Intake Air Temp °C
    float mafGps = 0.0f;        // MAF Airflow g/s
    float timingDeg = 10.0f;    // Ignition Timing Advance deg
} vehicleData;

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
            savvyClient = tcpServer.available();
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
#define OBD_REQUEST_ID  0x7E0
#define OBD_RESPONSE_ID 0x7E8

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

// Restart the TWAI peripheral after a bus-off (recovery requires stop/start).
void tryCanRecovery() {
    Serial.println("[CAN] Bus-off detected -> attempting TWAI recovery...");
    twai_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    if (twai_start() == ESP_OK) {
        Serial.println("[CAN] TWAI restarted after bus-off.");
    } else {
        Serial.println("[CAN] TWAI restart failed (wiring/termination?).");
    }
}

// Query only active selected PIDs
void sendToyotaObdQueries() {
    struct QueryItem {
        uint8_t mode;
        uint8_t pid;
    };
    QueryItem activeQueries[10];
    int queryCount = 0;

    for (size_t i = 0; i < PID_COUNT; i++) {
        if (availablePids[i].enabled && availablePids[i].mode != 0x00) {
            bool exists = false;
            for (int q = 0; q < queryCount; q++) {
                if (activeQueries[q].mode == availablePids[i].mode && activeQueries[q].pid == availablePids[i].pid) {
                    exists = true;
                    break;
                }
            }
            if (!exists && queryCount < 10) {
                activeQueries[queryCount].mode = availablePids[i].mode;
                activeQueries[queryCount].pid = availablePids[i].pid;
                queryCount++;
            }
        }
    }

    if (queryCount == 0) return;

    obdQueryIndex = obdQueryIndex % queryCount;

    twai_message_t queryMsg;
    queryMsg.identifier = OBD_REQUEST_ID;
    queryMsg.extd = 0;
    queryMsg.rtr = 0;
    queryMsg.data_length_code = 8;
    memset(queryMsg.data, 0, 8);

    queryMsg.data[0] = 0x02;
    queryMsg.data[1] = activeQueries[obdQueryIndex].mode;
    queryMsg.data[2] = activeQueries[obdQueryIndex].pid;

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
    fc.identifier = OBD_REQUEST_ID;
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
    if (msg.identifier == OBD_RESPONSE_ID && msg.data_length_code >= 1) {
        handleObdIsoTp(msg);
    }
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

// Count active PIDs
int getActivePidCount() {
    int count = 0;
    for (size_t i = 0; i < PID_COUNT; i++) {
        if (availablePids[i].enabled) count++;
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
        // Build dynamic CSV header with only selected PIDs
        String header = "Timestamp_ms";
        for (size_t i = 0; i < PID_COUNT; i++) {
            if (availablePids[i].enabled) {
                header += ",";
                header += availablePids[i].header;
            }
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
    tft.setRotation(isDisplayFlipped ? 2 : 0);
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
        Wire.requestFrom(RTC_I2C_ADDR, (uint8_t)7) != 7) {
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
        Wire.requestFrom(RTC_I2C_ADDR, (uint8_t)7) != 7) {
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
#define GT911_REG_POINT_STAT 0x814E
static uint8_t gt911Addr = 0;

void initGt911Touch() {
    // Reset pulse via the expander; INT state during reset latches the addr.
    ch422gSetPin(EXIO_TP_RST, false);
    delay(20);
    ch422gSetPin(EXIO_TP_RST, true);
    delay(100);

    const uint8_t candidates[2] = {0x5D, 0x14};
    for (uint8_t addr : candidates) {
        Wire.beginTransmission(addr);
        Wire.write((uint8_t)(GT911_REG_POINT_STAT >> 8));
        Wire.write((uint8_t)(GT911_REG_POINT_STAT & 0xFF));
        if (Wire.endTransmission(false) == 0 && Wire.requestFrom(addr, (uint8_t)1) == 1) {
            Wire.read(); // discard
            gt911Addr = addr;
            Serial.printf("[TOUCH] GT911 online at 0x%02X (INT: GPIO4)\n", addr);
            return;
        }
    }
    Serial.println("[TOUCH] No GT911 found at 0x5D/0x14 — touch disabled.");
}

bool pollTouch(int &screenX, int &screenY) {
    if (!gt911Addr) return false;

    Wire.beginTransmission(gt911Addr);
    Wire.write((uint8_t)(GT911_REG_POINT_STAT >> 8));
    Wire.write((uint8_t)(GT911_REG_POINT_STAT & 0xFF));
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(gt911Addr, (uint8_t)1) != 1) {
        return false;
    }
    uint8_t status = Wire.read();
    if (!(status & 0x80)) return false;          // no new data
    uint8_t count = status & 0x0F;
    if (count == 0 || count > 5) {               // flush flag and bail
        Wire.beginTransmission(gt911Addr);
        Wire.write((uint8_t)(GT911_REG_POINT_STAT >> 8));
        Wire.write((uint8_t)(GT911_REG_POINT_STAT & 0xFF));
        Wire.write(0);
        Wire.endTransmission();
        return false;
    }

    // Point 1 track data: [id, xL, xH, yL, yH, ...] from 0x8150
    Wire.beginTransmission(gt911Addr);
    Wire.write((uint8_t)(0x8150 >> 8));
    Wire.write((uint8_t)(0x8150 & 0xFF));
    bool ok = (Wire.endTransmission(false) == 0 && Wire.requestFrom(gt911Addr, (uint8_t)5) == 5);
    uint8_t pt[5] = {0};
    if (ok) Wire.readBytes(pt, 5);

    // Clear the buffer-ready flag so the controller updates again
    Wire.beginTransmission(gt911Addr);
    Wire.write((uint8_t)(GT911_REG_POINT_STAT >> 8));
    Wire.write((uint8_t)(GT911_REG_POINT_STAT & 0xFF));
    Wire.write(0);
    Wire.endTransmission();

    if (!ok) return false;
    int rawX = pt[1] | (pt[2] << 8);
    int rawY = pt[3] | (pt[4] << 8);
    if (rawX > 799) rawX = 799;
    if (rawY > 479) rawY = 479;

    // 180-deg mount flip: map panel coordinates back into canvas space so all
    // hit-box rectangles can stay written in normal orientation.
    screenX = isDisplayFlipped ? 799 - rawX : rawX;
    screenY = isDisplayFlipped ? 479 - rawY : rawY;
    return true;
}

// =========================================================================
// TRD Dark-Mode Motorsport UI Palette & Helper Macros
// =========================================================================
#define C_DARK_BG       canvas.color565(10, 12, 16)    // #0A0C10 Deep Jet Black
#define C_CARD_BG       canvas.color565(18, 22, 30)    // #12161E Carbon Dark Card
#define C_CARD_BORDER   canvas.color565(40, 48, 65)    // #283041 Subtle Card Border
#define C_CARD_INNER    canvas.color565(13, 16, 22)    // #0D1016 Darker Inner Fill
#define C_TRD_ORANGE    canvas.color565(245, 130, 32)  // #F58220 TRD Heritage Orange
#define C_TRD_RED       canvas.color565(235, 10, 30)   // #EB0A1E TRD Vibrant Red
#define C_TRD_BURGUNDY  canvas.color565(150, 15, 25)   // #960F19 TRD Deep Burgundy
#define C_TEXT_WHITE    TFT_WHITE                      // Pure Crisp White
#define C_TEXT_MUTED    canvas.color565(130, 140, 160) // Cool Slate Gray
#define C_TEXT_CYAN     canvas.color565(0, 220, 255)   // Ice Cyan Telemetry
#define C_GREEN_OK      canvas.color565(40, 220, 100)  // Nominal Green
#define C_GOLD_LOCK     canvas.color565(255, 205, 0)   // TCC Lock Gold

// =========================================================================
// Full-Color 320x240 UI Page Renderers (TRD Motorsport Dark Edition)
// =========================================================================

void drawHeaderBar(const char* title) {
    // Deep dark header bar
    canvas.fillRect(0, 0, 320, 24, canvas.color565(14, 16, 22));
    
    // TRD Heritage Tri-Color Mini Stripes (Top Left)
    canvas.fillRect(0, 0, 5, 24, C_TRD_ORANGE);
    canvas.fillRect(5, 0, 5, 24, C_TRD_RED);
    canvas.fillRect(10, 0, 5, 24, C_TRD_BURGUNDY);

    // Screen Title
    canvas.setTextColor(C_TEXT_WHITE, canvas.color565(14, 16, 22));
    canvas.setFont(&fonts::Font2);
    canvas.drawString(title, 22, 4);

    // Live Message Rate
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f msg/s", currentPPS);
    canvas.setTextColor(C_TEXT_CYAN, canvas.color565(14, 16, 22));
    canvas.drawRightString(buf, 242, 4);

    // SD / REC Status Pill
    if (currentLogMode != LOG_IDLE) {
        bool blink = ((millis() / 500) % 2 == 0);
        canvas.fillRoundRect(248, 3, 68, 18, 3, blink ? C_TRD_RED : canvas.color565(80, 10, 15));
        canvas.setTextColor(C_TEXT_WHITE);
        canvas.setFont(&fonts::Font0);
        canvas.drawCenterString((currentLogMode == LOG_CANBUS) ? "CAN REC" : "PID REC", 282, 7);
    } else {
        canvas.fillRoundRect(256, 3, 60, 18, 3, sdMounted ? canvas.color565(15, 38, 22) : canvas.color565(30, 32, 40));
        canvas.drawRoundRect(256, 3, 60, 18, 3, sdMounted ? canvas.color565(40, 140, 60) : canvas.color565(60, 65, 80));
        canvas.setTextColor(sdMounted ? C_GREEN_OK : C_TEXT_MUTED);
        canvas.setFont(&fonts::Font0);
        canvas.drawCenterString(sdMounted ? "SD OK" : "NO SD", 286, 7);
    }

    canvas.drawFastHLine(0, 24, 320, C_CARD_BORDER);
}

void drawBottomNavBar() {
    canvas.fillRect(0, 218, 320, 22, canvas.color565(12, 14, 18));
    canvas.drawFastHLine(0, 218, 320, C_CARD_BORDER);

    canvas.setTextColor(C_TEXT_MUTED);
    canvas.setFont(&fonts::Font2);
    canvas.drawString("< PREV", 10, 222);

    int dotSpacing = 16;
    int startDotX = 160 - (((SCREEN_COUNT - 1) * dotSpacing) / 2);
    for (int i = 0; i < SCREEN_COUNT; i++) {
        int dx = startDotX + (i * dotSpacing);
        if (i == currentScreen) {
            canvas.fillRoundRect(dx - 5, 226, 12, 6, 3, C_TRD_RED); // TRD Red active capsule
        } else {
            canvas.fillCircle(dx, 228, 2, canvas.color565(55, 62, 78));
        }
    }

    canvas.drawRightString("NEXT >", 310, 222);
}

// Page 0: Live Vehicle Cluster (TRD Motorsport Gauge)
void renderDashboard() {
    drawHeaderBar("TOYOTA DASHVIEW - CLUSTER");

    // 1. Tachometer Bar (0 - 6000 RPM) with TRD Motorsport color bands
    int rpmY = 30;
    canvas.fillRoundRect(10, rpmY, 300, 22, 4, C_CARD_BG);
    canvas.drawRoundRect(10, rpmY, 300, 22, 4, C_CARD_BORDER);
    
    int rpmWidth = map(constrain(vehicleData.rpm, 0, 6000), 0, 6000, 0, 294);
    if (rpmWidth > 0) {
        for (int i = 0; i < rpmWidth; i++) {
            uint16_t barColor;
            if (i < 165) {
                barColor = canvas.color565(0, 200, 160); // Slate cyan-green
            } else if (i < 235) {
                barColor = C_TRD_ORANGE;                 // TRD Heritage Orange
            } else {
                barColor = C_TRD_RED;                    // TRD Redline
            }
            canvas.drawFastVLine(13 + i, rpmY + 3, 16, barColor);
        }
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "%d RPM", vehicleData.rpm);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.setFont(&fonts::Font2);
    canvas.drawString("TACHOMETER", 18, rpmY + 4);
    canvas.drawRightString(buf, 302, rpmY + 4);

    // 2. Center Hero: Gear & Torque Converter Lockup (Left Card)
    canvas.fillRoundRect(10, 58, 92, 94, 6, C_CARD_BG);
    canvas.drawRoundRect(10, 58, 92, 94, 6, C_CARD_BORDER);
    canvas.fillRect(12, 58, 88, 3, C_TRD_RED); // TRD Red Accent Line
    
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.setFont(&fonts::Font0);
    canvas.drawCenterString("GEAR", 56, 66);

    canvas.setFont(&fonts::Font7);
    if (vehicleData.tccLocked && vehicleData.gear[0] >= '1' && vehicleData.gear[0] <= '6') {
        snprintf(buf, sizeof(buf), "%sL", vehicleData.gear);
        canvas.setTextColor(C_GOLD_LOCK);
    } else {
        snprintf(buf, sizeof(buf), "%s", vehicleData.gear);
        canvas.setTextColor(C_TEXT_WHITE);
    }
    canvas.drawCenterString(buf, 56, 78);

    // Lockup status badge
    if (vehicleData.tccLocked) {
        canvas.fillRoundRect(16, 130, 80, 16, 3, canvas.color565(180, 140, 0));
        canvas.setTextColor(TFT_BLACK);
        canvas.setFont(&fonts::Font2);
        canvas.drawCenterString("LOCKED", 56, 131);
    } else {
        canvas.fillRoundRect(16, 130, 80, 16, 3, C_CARD_INNER);
        canvas.setTextColor(C_TEXT_MUTED);
        canvas.setFont(&fonts::Font0);
        canvas.drawCenterString("OPEN", 56, 134);
    }

    // 3. Air-Fuel Ratio (AFR) Wideband Card (Top Right)
    canvas.fillRoundRect(108, 58, 202, 45, 6, C_CARD_BG);
    canvas.drawRoundRect(108, 58, 202, 45, 6, C_CARD_BORDER);
    canvas.fillRect(110, 58, 198, 3, C_TEXT_CYAN);

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(canvas.color565(160, 190, 240));
    snprintf(buf, sizeof(buf), "Cmd AFR: %.1f (%.2f L)", vehicleData.commandedAfr, vehicleData.commandedAfr / 14.7f);
    canvas.drawString(buf, 116, 65);

    uint16_t actAfrColor = (vehicleData.actualAfr > 15.2f) ? C_TRD_RED : ((vehicleData.actualAfr < 12.0f) ? C_TRD_ORANGE : C_GREEN_OK);
    canvas.setTextColor(actAfrColor);
    snprintf(buf, sizeof(buf), "Act AFR: %.1f (%.2f L)", vehicleData.actualAfr, vehicleData.actualAfr / 14.7f);
    canvas.drawString(buf, 116, 83);

    // 4. Knock Health (KCLV & KFB) Card (Bottom Right)
    canvas.fillRoundRect(108, 107, 202, 45, 6, C_CARD_BG);
    canvas.drawRoundRect(108, 107, 202, 45, 6, C_CARD_BORDER);
    canvas.fillRect(110, 107, 198, 3, C_TRD_ORANGE);

    uint16_t kclvColor = (vehicleData.kclv >= 19.0f) ? C_GREEN_OK : ((vehicleData.kclv >= 15.0f) ? C_TRD_ORANGE : C_TRD_RED);
    canvas.setTextColor(kclvColor);
    snprintf(buf, sizeof(buf), "KCLV: %.1f", vehicleData.kclv);
    canvas.drawString(buf, 116, 113);

    uint16_t kfbColor = (vehicleData.knockFB < 0) ? C_TRD_RED : C_TEXT_CYAN;
    canvas.setTextColor(kfbColor);
    snprintf(buf, sizeof(buf), "KFB: %+2.1f deg", vehicleData.knockFB);
    canvas.drawString(buf, 214, 113);

    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Learned Knock Value (20.0 = Nominal)", 116, 134);

    // 5. Dual Mini-Gauges: Throttle % & Engine Load %
    int botY = 158;
    canvas.fillRoundRect(10, botY, 145, 52, 4, C_CARD_BG);
    canvas.drawRoundRect(10, botY, 145, 52, 4, C_CARD_BORDER);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.setFont(&fonts::Font2);
    snprintf(buf, sizeof(buf), "Throttle: %d%%", vehicleData.throttlePct);
    canvas.drawString(buf, 16, botY + 4);
    canvas.drawRoundRect(16, botY + 24, 133, 16, 3, C_CARD_BORDER);
    int thrWidth = map(constrain(vehicleData.throttlePct, 0, 100), 0, 100, 0, 129);
    if (thrWidth > 0) {
        canvas.fillRect(18, botY + 26, thrWidth, 12, C_TEXT_CYAN);
    }

    canvas.fillRoundRect(165, botY, 145, 52, 4, C_CARD_BG);
    canvas.drawRoundRect(165, botY, 145, 52, 4, C_CARD_BORDER);
    canvas.setTextColor(C_TEXT_WHITE);
    snprintf(buf, sizeof(buf), "Load: %d%%", vehicleData.engineLoadPct);
    canvas.drawString(buf, 171, botY + 4);
    canvas.drawRoundRect(171, botY + 24, 133, 16, 3, C_CARD_BORDER);
    int loadWidth = map(constrain(vehicleData.engineLoadPct, 0, 100), 0, 100, 0, 129);
    if (loadWidth > 0) {
        canvas.fillRect(173, botY + 26, loadWidth, 12, C_TRD_ORANGE);
    }

    drawBottomNavBar();
}

// Floating Overlay Sub-Screen: Raw Packet Monitor Modal
void renderRawSnifferModal() {
    // Header Bar
    canvas.fillRect(0, 0, 320, 24, canvas.color565(14, 16, 22));
    canvas.fillRect(0, 0, 5, 24, C_TRD_ORANGE);
    canvas.fillRect(5, 0, 5, 24, C_TRD_RED);
    canvas.fillRect(10, 0, 5, 24, C_TRD_BURGUNDY);

    canvas.setTextColor(C_TEXT_WHITE, canvas.color565(14, 16, 22));
    canvas.setFont(&fonts::Font2);
    canvas.drawString("RAW CAN STREAM", 22, 4);

    // Status Pill: STREAMING (Cyan) vs PAUSED (Orange)
    if (isSnifferPaused) {
        canvas.fillRoundRect(236, 3, 76, 18, 3, canvas.color565(80, 45, 10));
        canvas.drawRoundRect(236, 3, 76, 18, 3, C_TRD_ORANGE);
        canvas.setTextColor(C_TRD_ORANGE);
        canvas.setFont(&fonts::Font0);
        canvas.drawCenterString("PAUSED", 274, 7);
    } else {
        canvas.fillRoundRect(220, 3, 92, 18, 3, canvas.color565(10, 40, 50));
        canvas.drawRoundRect(220, 3, 92, 18, 3, C_TEXT_CYAN);
        canvas.setTextColor(C_TEXT_CYAN);
        canvas.setFont(&fonts::Font0);
        canvas.drawCenterString("STREAMING", 266, 7);
    }

    canvas.drawFastHLine(0, 24, 320, C_CARD_BORDER);

    // Table Column Header
    canvas.fillRect(8, 28, 304, 16, C_CARD_INNER);
    canvas.drawRoundRect(8, 28, 304, 16, 3, C_CARD_BORDER);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("CAN ID", 16, 32);
    canvas.drawString("DLC", 72, 32);
    canvas.drawString("HEX PAYLOAD (BYTES 0..7)", 105, 32);

    // Render Frame Rows (up to 6 rows)
    char buf[64];
    int rowY = 48;
    for (int i = 0; i < 6; i++) {
        int idx = (snifferHead - 1 - i + SNIFFER_HISTORY_SIZE) % SNIFFER_HISTORY_SIZE;
        uint16_t rowBg = (i % 2 == 0) ? C_CARD_BG : canvas.color565(22, 26, 36);
        canvas.fillRoundRect(8, rowY, 304, 24, 4, rowBg);
        canvas.drawRoundRect(8, rowY, 304, 24, 4, C_CARD_BORDER);

        if (snifferHistory[idx].id != 0 || snifferHistory[idx].dlc != 0) {
            snprintf(buf, sizeof(buf), "0x%03X", snifferHistory[idx].id);
            canvas.setTextColor(C_TRD_ORANGE);
            canvas.setFont(&fonts::Font2);
            canvas.drawString(buf, 14, rowY + 4);

            snprintf(buf, sizeof(buf), "[%d]", snifferHistory[idx].dlc);
            canvas.setTextColor(C_TEXT_MUTED);
            canvas.drawString(buf, 72, rowY + 4);

            char hexBuf[36] = "";
            for (int b = 0; b < snifferHistory[idx].dlc && b < 8; b++) {
                char bStr[6];
                snprintf(bStr, sizeof(bStr), "%02X ", snifferHistory[idx].data[b]);
                strcat(hexBuf, bStr);
            }
            canvas.setTextColor(C_TEXT_WHITE);
            canvas.drawString(hexBuf, 105, rowY + 4);
        } else {
            canvas.setTextColor(C_TEXT_MUTED);
            canvas.setFont(&fonts::Font0);
            canvas.drawString("-- Waiting for bus traffic --", 105, rowY + 7);
        }

        rowY += 26;
    }

    // Bottom Action Deck (Pause, Clear, Back)
    int botY = 206;

    // 1. [ PAUSE / RESUME ] Button (x: 12..98)
    uint16_t pauseBg = isSnifferPaused ? C_TRD_ORANGE : canvas.color565(25, 35, 52);
    uint16_t pauseBorder = isSnifferPaused ? canvas.color565(255, 180, 50) : canvas.color565(60, 100, 160);
    canvas.fillRoundRect(12, botY, 86, 28, 4, pauseBg);
    canvas.drawRoundRect(12, botY, 86, 28, 4, pauseBorder);
    canvas.setTextColor(isSnifferPaused ? TFT_BLACK : C_TEXT_WHITE);
    canvas.setFont(&fonts::Font2);
    canvas.drawCenterString(isSnifferPaused ? "RESUME" : "PAUSE", 55, botY + 6);

    // 2. [ CLEAR ] Button (x: 104..180)
    canvas.fillRoundRect(104, botY, 76, 28, 4, canvas.color565(30, 32, 42));
    canvas.drawRoundRect(104, botY, 76, 28, 4, C_CARD_BORDER);
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.setFont(&fonts::Font2);
    canvas.drawCenterString("CLEAR", 142, botY + 6);

    // 3. [ ✖ BACK / CLOSE ] Button (x: 186..308)
    canvas.fillRoundRect(186, botY, 122, 28, 4, C_TRD_RED);
    canvas.drawRoundRect(186, botY, 122, 28, 4, canvas.color565(255, 100, 100));
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.setFont(&fonts::Font2);
    canvas.drawCenterString("BACK / CLOSE", 247, botY + 6);
}

// Page 1: Live CAN Sniffer & Traffic Monitor
void renderSniffer() {
    if (isRawSnifferModalOpen) {
        renderRawSnifferModal();
        return;
    }

    drawHeaderBar("CAN SNIFFER & TRAFFIC MONITOR");

    char buf[64];
    unsigned long elapsedSec = (currentLogMode != LOG_IDLE) ? ((millis() - logStartTime) / 1000) : 0;
    bool isCanActive = (currentLogMode == LOG_CANBUS);
    bool canDisabled = (currentLogMode == LOG_DATALOG);

    // Card 1: CAN Sniffer & Raw Frame Logger Control
    // Box: x=12, y=28, w=296, h=54
    uint16_t canBgColor = isCanActive ? canvas.color565(55, 14, 20) : (canDisabled ? canvas.color565(16, 18, 24) : C_CARD_BG);
    uint16_t canBorderColor = isCanActive ? C_TRD_RED : (canDisabled ? canvas.color565(35, 40, 52) : C_CARD_BORDER);

    canvas.fillRoundRect(12, 28, 296, 54, 6, canBgColor);
    canvas.drawRoundRect(12, 28, 296, 54, 6, canBorderColor);
    canvas.fillRect(14, 28, 4, 54, isCanActive ? C_TRD_RED : C_TRD_ORANGE);

    canvas.setFont(&fonts::Font4);
    if (isCanActive) {
        canvas.setTextColor(C_TRD_RED);
        canvas.drawString("[STOP CAN LOGGING]", 26, 32);
        canvas.setFont(&fonts::Font2);
        snprintf(buf, sizeof(buf), "REC: %s (%lu frames, %lum%02lus)", currentLogFileName, logEntryCount, elapsedSec / 60, elapsedSec % 60);
        canvas.setTextColor(canvas.color565(255, 200, 200));
        canvas.drawString(buf, 26, 56);
    } else {
        canvas.setTextColor(canDisabled ? C_TEXT_MUTED : C_TEXT_WHITE);
        canvas.drawString("[START CAN LOGGING]", 26, 32);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(canDisabled ? canvas.color565(80, 85, 100) : C_TEXT_MUTED);
        canvas.drawString(canDisabled ? "Locked (Stop PID Datalogger first)" : "Logs raw vehicle bus traffic -> canbus_XXXX.csv", 26, 56);
    }

    // Card 2: CAN Bus Traffic & Statistics Deck
    // Box: x=12, y=88, w=296, h=58
    canvas.fillRoundRect(12, 88, 296, 58, 6, C_CARD_BG);
    canvas.drawRoundRect(12, 88, 296, 58, 6, C_CARD_BORDER);
    canvas.fillRect(14, 88, 4, 58, C_TEXT_CYAN);

    canvas.setFont(&fonts::Font2);
    // Top Row
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Total Frames:", 24, 94);
    snprintf(buf, sizeof(buf), "%lu pkts", packetCount);
    canvas.setTextColor(C_TEXT_CYAN);
    canvas.drawString(buf, 110, 94);

    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Rate:", 196, 94);
    snprintf(buf, sizeof(buf), "%.0f msg/s", currentPPS);
    canvas.setTextColor(C_GREEN_OK);
    canvas.drawString(buf, 238, 94);

    // Bottom Row
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("TWAI Mode:", 24, 118);
    canvas.setTextColor(C_GREEN_OK);
    canvas.drawString("500 kbps HS-CAN", 110, 118);

    // Card 3: Raw Packet Stream Terminal Launcher Button
    // Box: x=12, y=152, w=296, h=52
    canvas.fillRoundRect(12, 152, 296, 52, 6, canvas.color565(20, 24, 34));
    canvas.drawRoundRect(12, 152, 296, 52, 6, canvas.color565(45, 60, 85));
    canvas.fillRect(14, 152, 4, 52, C_TRD_BURGUNDY);

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawString("[+] VIEW LIVE RAW PACKET STREAM", 26, 158);

    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Tap to open live scrolling terminal with pause & frame inspection", 26, 180);

    drawBottomNavBar();
}

// Sub-Screen: Interactive PID Selector Modal
void renderPidSelector() {
    char titleBuf[48];
    snprintf(titleBuf, sizeof(titleBuf), "SELECT DATALOG PIDs (%d/%d)", getActivePidCount(), (int)PID_COUNT);
    drawHeaderBar(titleBuf);

    int startY = 28;
    int rowHeight = 28;
    int colWidth = 144;

    for (size_t i = 0; i < PID_COUNT; i++) {
        int col = (i % 2);
        int row = (i / 2);
        int bx = 12 + col * (colWidth + 8);
        int by = startY + row * (rowHeight + 2);

        uint16_t boxBg = availablePids[i].enabled ? canvas.color565(32, 18, 24) : C_CARD_BG;
        uint16_t boxBorder = availablePids[i].enabled ? C_TRD_RED : C_CARD_BORDER;
        uint16_t txtColor = availablePids[i].enabled ? C_TEXT_WHITE : C_TEXT_MUTED;

        canvas.fillRoundRect(bx, by, colWidth, rowHeight, 4, boxBg);
        canvas.drawRoundRect(bx, by, colWidth, rowHeight, 4, boxBorder);

        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(availablePids[i].enabled ? C_TRD_RED : C_TEXT_MUTED);
        canvas.drawString(availablePids[i].enabled ? "[X]" : "[ ]", bx + 8, by + 5);

        canvas.setTextColor(txtColor);
        canvas.drawString(availablePids[i].label, bx + 34, by + 5);
    }

    int botActionY = 210;
    
    // [ ALL ] Button
    canvas.fillRoundRect(12, botActionY, 65, 26, 4, canvas.color565(25, 35, 52));
    canvas.drawRoundRect(12, botActionY, 65, 26, 4, canvas.color565(60, 110, 180));
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.setFont(&fonts::Font2);
    canvas.drawCenterString("ALL", 44, botActionY + 5);

    // [ NONE ] Button
    canvas.fillRoundRect(85, botActionY, 65, 26, 4, canvas.color565(45, 20, 25));
    canvas.drawRoundRect(85, botActionY, 65, 26, 4, C_TRD_BURGUNDY);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawCenterString("NONE", 117, botActionY + 5);

    // [ SAVE & RETURN ] Button
    canvas.fillRoundRect(160, botActionY, 148, 26, 4, C_TRD_RED);
    canvas.drawRoundRect(160, botActionY, 148, 26, 4, canvas.color565(255, 100, 100));
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawCenterString("SAVE & RETURN", 234, botActionY + 5);
}

// Page 2: Dedicated PID Vehicle Datalogger & Parameter Recording Deck
void renderLoggerControl() {
    if (isPidConfigOpen) {
        renderPidSelector();
        return;
    }

    drawHeaderBar("PID VEHICLE DATALOGGER");

    char buf[64];
    unsigned long elapsedSec = (currentLogMode != LOG_IDLE) ? ((millis() - logStartTime) / 1000) : 0;
    bool isDatalogActive = (currentLogMode == LOG_DATALOG);
    bool datalogDisabled = (currentLogMode == LOG_CANBUS);

    // 1. BUTTON 1: PID Datalogger Start / Stop
    // Box: x=12, y=28, w=296, h=54
    uint16_t dlBgColor = isDatalogActive ? canvas.color565(55, 30, 10) : (datalogDisabled ? canvas.color565(16, 18, 24) : C_CARD_BG);
    uint16_t dlBorderColor = isDatalogActive ? C_TRD_ORANGE : (datalogDisabled ? canvas.color565(35, 40, 52) : C_CARD_BORDER);

    canvas.fillRoundRect(12, 28, 296, 54, 6, dlBgColor);
    canvas.drawRoundRect(12, 28, 296, 54, 6, dlBorderColor);
    canvas.fillRect(14, 28, 4, 54, isDatalogActive ? C_TRD_ORANGE : C_TEXT_CYAN);

    canvas.setFont(&fonts::Font4);
    if (isDatalogActive) {
        canvas.setTextColor(C_TRD_ORANGE);
        canvas.drawString("[STOP PID DATALOG]", 26, 32);
        canvas.setFont(&fonts::Font2);
        snprintf(buf, sizeof(buf), "REC: %s (%lu samples, %lum%02lus)", currentLogFileName, logEntryCount, elapsedSec / 60, elapsedSec % 60);
        canvas.setTextColor(canvas.color565(255, 230, 180));
        canvas.drawString(buf, 26, 56);
    } else {
        canvas.setTextColor(datalogDisabled ? C_TEXT_MUTED : C_TEXT_WHITE);
        canvas.drawString("[START PID DATALOG]", 26, 32);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(datalogDisabled ? canvas.color565(80, 85, 100) : C_TEXT_MUTED);
        snprintf(buf, sizeof(buf), "Logs %d Selected PIDs -> datalog_XXXX.csv", getActivePidCount());
        canvas.drawString(datalogDisabled ? "Locked (Stop CAN Logger on Page 1 first)" : buf, 26, 56);
    }

    // 2. Active Parameters Preview Deck
    // Box: x=12, y=88, w=296, h=58
    canvas.fillRoundRect(12, 88, 296, 58, 6, C_CARD_BG);
    canvas.drawRoundRect(12, 88, 296, 58, 6, C_CARD_BORDER);
    canvas.fillRect(14, 88, 4, 58, C_TRD_ORANGE);

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_TEXT_WHITE);
    snprintf(buf, sizeof(buf), "Active Parameters (%d of %d Selected):", getActivePidCount(), (int)PID_COUNT);
    canvas.drawString(buf, 24, 94);

    // Build list of active PID tags
    String tagList = "";
    int count = 0;
    for (size_t i = 0; i < PID_COUNT && count < 7; i++) {
        if (availablePids[i].enabled) {
            tagList += "[";
            tagList += availablePids[i].idStr;
            tagList += "] ";
            count++;
        }
    }
    if (getActivePidCount() > 7) tagList += "...";
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT_CYAN);
    canvas.drawString(tagList.c_str(), 24, 118);

    // 3. Configure Recorded PIDs Button
    // Box: x=12, y=152, w=296, h=52
    canvas.fillRoundRect(12, 152, 296, 52, 6, C_CARD_BG);
    canvas.drawRoundRect(12, 152, 296, 52, 6, C_CARD_BORDER);
    canvas.fillRect(14, 152, 4, 52, C_TRD_BURGUNDY);

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_TEXT_WHITE);
    snprintf(buf, sizeof(buf), "[+] CONFIGURE RECORDED PIDs (%d Active)", getActivePidCount());
    canvas.drawString(buf, 26, 158);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Tap here to customize parameters recorded to SD (10 Hz rate)", 26, 180);

    drawBottomNavBar();
}

// Page 3: Wi-Fi & SavvyCAN Streaming (Wireless Cockpit)
void renderWiFi() {
    drawHeaderBar("WI-FI SAVVYCAN STREAMING");

    canvas.fillRoundRect(12, 32, 296, 178, 6, C_CARD_BG);
    canvas.drawRoundRect(12, 32, 296, 178, 6, C_CARD_BORDER);
    canvas.fillRect(14, 32, 292, 3, C_TEXT_CYAN);

    canvas.setFont(&fonts::Font2);
    
    // SSID
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Hotspot SSID:", 24, 46);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawString(WIFI_SSID, 140, 46);

    // Password
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Password:", 24, 74);
    canvas.setTextColor(C_TRD_ORANGE);
    canvas.drawString(WIFI_PASS, 140, 74);

    // SavvyCAN Server Port
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("SavvyCAN Server:", 24, 102);
    canvas.setTextColor(C_GREEN_OK);
    canvas.drawString("192.168.4.1:23", 160, 102);

    // Client Status Pill
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Client Status:", 24, 130);
    if (savvyClient && savvyClient.connected()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "CONNECTED (%lu pkts)", wifiStreamedCount);
        canvas.setTextColor(C_GREEN_OK);
        canvas.drawString(buf, 140, 130);
    } else {
        canvas.setTextColor(C_TRD_ORANGE);
        canvas.drawString("Waiting for Laptop...", 140, 130);
    }

    // Help Text
    canvas.fillRoundRect(20, 160, 280, 36, 4, C_CARD_INNER);
    canvas.drawRoundRect(20, 160, 280, 36, 4, C_CARD_BORDER);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("In SavvyCAN: Add Network Connection -> 192.168.4.1:23", 26, 166);
    canvas.drawString("Streams live vehicle bus traffic wirelessly without cables.", 26, 178);

    drawBottomNavBar();
}

// Page 4: System Diagnostics & Hardware Health
void renderSystem() {
    drawHeaderBar("HARDWARE DIAGNOSTICS");

    canvas.fillRoundRect(12, 32, 296, 178, 6, C_CARD_BG);
    canvas.drawRoundRect(12, 32, 296, 178, 6, C_CARD_BORDER);
    canvas.fillRect(14, 32, 292, 3, C_TRD_RED);

    char buf[64];
    canvas.setFont(&fonts::Font2);

    // Firmware Version (SemVer)
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Firmware Ver:", 24, 44);
    snprintf(buf, sizeof(buf), "%s (%s)", APP_VERSION_STR, APP_BUILD_DATE);
    canvas.setTextColor(C_TRD_ORANGE);
    canvas.drawString(buf, 136, 44);

    // MCU
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("MCU Platform:", 24, 70);
    snprintf(buf, sizeof(buf), "ESP32-S3 @ %d MHz", getCpuFrequencyMhz());
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawString(buf, 136, 70);

    // Flash & PSRAM
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Flash & RAM:", 24, 96);
    snprintf(buf, sizeof(buf), "16MB Flash | %dMB PSRAM", ESP.getPsramSize() / (1024 * 1024));
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawString(buf, 136, 96);

    // Free Heap
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Free Heap:", 24, 122);
    snprintf(buf, sizeof(buf), "%u KB (Healthy)", ESP.getFreeHeap() / 1024);
    canvas.setTextColor(C_GREEN_OK);
    canvas.drawString(buf, 136, 122);

    // MicroSD
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("MicroSD Card:", 24, 148);
    snprintf(buf, sizeof(buf), "%s (%s)", sdMounted ? "Mounted FAT32" : "Unmounted", (currentLogMode != LOG_IDLE) ? "LOGGING" : "READY");
    canvas.setTextColor(sdMounted ? C_GREEN_OK : C_TRD_RED);
    canvas.drawString(buf, 136, 148);

    // CAN Interface
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("CAN Interface:", 24, 174);
    snprintf(buf, sizeof(buf), "IO%d/IO%d (500k HS-CAN)", CAN_TX_PIN, CAN_RX_PIN);
    canvas.setTextColor(C_TEXT_CYAN);
    canvas.drawString(buf, 136, 174);

    drawBottomNavBar();
}

// Page 5: Settings & Display Configuration
void renderSettings() {
    drawHeaderBar("TOYOTA DASHVIEW - SETTINGS");

    char buf[64];

    // Card 1: Display Orientation (180-deg Flip)
    // Box: x=12, y=28, w=296, h=52
    canvas.fillRoundRect(12, 28, 296, 52, 6, C_CARD_BG);
    canvas.drawRoundRect(12, 28, 296, 52, 6, C_CARD_BORDER);
    canvas.fillRect(14, 28, 4, 52, C_TRD_RED);

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawString("Display Orientation (180 deg Flip)", 26, 32);

    uint16_t flipBtnBg = isDisplayFlipped ? C_TRD_RED : canvas.color565(25, 35, 50);
    uint16_t flipBtnBorder = isDisplayFlipped ? canvas.color565(255, 100, 100) : canvas.color565(60, 100, 160);
    canvas.fillRoundRect(26, 52, 268, 22, 4, flipBtnBg);
    canvas.drawRoundRect(26, 52, 268, 22, 4, flipBtnBorder);
    canvas.setTextColor(C_TEXT_WHITE);
    canvas.setFont(&fonts::Font2);
    snprintf(buf, sizeof(buf), "MODE: %s (TAP TO FLIP)", isDisplayFlipped ? "FLIPPED 180 (INVERTED)" : "STANDARD 0 (NORMAL)");
    canvas.drawCenterString(buf, 160, 55);

    // Card 2: Backlight Brightness
    // Box: x=12, y=86, w=296, h=56
    canvas.fillRoundRect(12, 86, 296, 56, 6, C_CARD_BG);
    canvas.drawRoundRect(12, 86, 296, 56, 6, C_CARD_BORDER);
    canvas.fillRect(14, 86, 4, 56, C_TRD_ORANGE);

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_TEXT_WHITE);
    snprintf(buf, sizeof(buf), "Backlight Brightness (%d%%)", (int)((userBrightness * 100) / 255));
    canvas.drawString(buf, 26, 90);

    const uint8_t brLevels[4] = {64, 128, 192, 255};
    const char* brLabels[4]   = {"25%", "50%", "75%", "100%"};
    for (int i = 0; i < 4; i++) {
        int bx = 26 + i * 68;
        int by = 112;
        bool isActive = (abs((int)userBrightness - (int)brLevels[i]) <= 32);
        uint16_t bg = isActive ? C_TRD_ORANGE : C_CARD_INNER;
        uint16_t border = isActive ? canvas.color565(255, 180, 50) : C_CARD_BORDER;
        canvas.fillRoundRect(bx, by, 62, 24, 4, bg);
        canvas.drawRoundRect(bx, by, 62, 24, 4, border);
        canvas.setTextColor(isActive ? TFT_BLACK : C_TEXT_MUTED);
        canvas.setFont(&fonts::Font2);
        canvas.drawCenterString(brLabels[i], bx + 31, by + 4);
    }

    // Card 3: Reboot Controller
    // Box: x=12, y=148, w=296, h=44
    canvas.fillRoundRect(12, 148, 296, 44, 6, C_CARD_BG);
    canvas.drawRoundRect(12, 148, 296, 44, 6, C_CARD_BORDER);
    canvas.fillRect(14, 148, 4, 44, C_TRD_BURGUNDY);

    canvas.fillRoundRect(26, 154, 268, 30, 4, canvas.color565(45, 18, 22));
    canvas.drawRoundRect(26, 154, 268, 30, 4, C_TRD_BURGUNDY);
    canvas.setTextColor(canvas.color565(255, 120, 120));
    canvas.setFont(&fonts::Font2);
    canvas.drawCenterString("REBOOT CONTROLLER", 160, 160);

    // Status Footer
    canvas.fillRoundRect(12, 198, 296, 18, 3, C_CARD_INNER);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawCenterString("Settings auto-saved to persistent flash storage", 160, 202);

    drawBottomNavBar();
}

void updateDisplay() {
    canvas.fillScreen(C_DARK_BG);

    switch (currentScreen) {
        case SCREEN_DASHBOARD: renderDashboard();     break;
        case SCREEN_SNIFFER:   renderSniffer();       break;
        case SCREEN_LOGGER:    renderLoggerControl(); break;
        case SCREEN_WIFI:      renderWiFi();          break;
        case SCREEN_SYSTEM:    renderSystem();        break;
        case SCREEN_SETTINGS:  renderSettings();      break;
        default:               renderDashboard();     break;
    }

    canvas.pushSprite(0, 0);
}

// =========================================================================
// Official TRD Red Boot Splash (Native PNG Decoded)
// =========================================================================
void showToyotaBootSplash() {
    canvas.drawPng(trd_splash_png, TRD_SPLASH_PNG_LEN, 0, 0);
    canvas.pushSprite(0, 0);
    isBootSplashActive = true;
}

// =========================================================================
// Screen Navigation Functions
// =========================================================================
void nextScreen() {
    if (isPidConfigOpen) return;
    currentScreen = static_cast<DisplayScreen>((currentScreen + 1) % SCREEN_COUNT);
    wakeScreen();
    Serial.printf("[SWIPE] Switched to Next Screen -> Page %d\n", currentScreen);
}

void prevScreen() {
    if (isPidConfigOpen) return;
    currentScreen = static_cast<DisplayScreen>((currentScreen - 1 + SCREEN_COUNT) % SCREEN_COUNT);
    wakeScreen();
    Serial.printf("[SWIPE] Switched to Prev Screen <- Page %d\n", currentScreen);
}

// =========================================================================
// Capacitive Touch & Gesture / Button Tap Engine
// =========================================================================
#define SWIPE_MIN_DIST_PX 400  // Must swipe at least half of the 800px screen width

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
        } else {
            touchLastX = touchX;
            touchLastY = touchY;
        }
    } else if (wasTouched) {
        wasTouched = false;
        int deltaX = touchLastX - touchStartX;
        int deltaY = touchLastY - touchStartY;
        unsigned long duration = millis() - touchStartTime;

        Serial.printf("[TOUCH] Release at (%d, %d) | deltaX=%d, deltaY=%d, dur=%lums\n", 
                      touchLastX, touchLastY, deltaX, deltaY, duration);

        // 1. Horizontal Swipe: MUST travel at least half the screen width (>= 160px)
        if (abs(deltaX) >= SWIPE_MIN_DIST_PX && !isPidConfigOpen && !isRawSnifferModalOpen) {
            if (deltaX <= -SWIPE_MIN_DIST_PX) {
                nextScreen(); // Drag right-to-left >= 160px -> Next Page
                Serial.printf("[SWIPE] Left swipe (%d px) -> Next Screen (%d)\n", deltaX, currentScreen);
            } else if (deltaX >= SWIPE_MIN_DIST_PX) {
                prevScreen(); // Drag left-to-right >= 160px -> Prev Page
                Serial.printf("[SWIPE] Right swipe (%d px) -> Prev Screen (%d)\n", deltaX, currentScreen);
            }
            return;
        }

        // 2. Stationary Button / Card Tap (Minimal movement < 30px, duration < 600ms)
        // A regular touch NEVER changes screens!
        if (abs(deltaX) < 30 && abs(deltaY) < 30 && duration < 600) {
            // A. Page 1 (CAN Sniffer Page)
            if (currentScreen == SCREEN_SNIFFER) {
                // When Floating Raw Packet Terminal is Open
                if (isRawSnifferModalOpen) {
                    // Button 1: [ PAUSE / RESUME ] (x: 12..98, y: 204..238)
                    if (touchLastX >= 12 && touchLastX <= 98 && touchLastY >= 204 && touchLastY <= 238) {
                        isSnifferPaused = !isSnifferPaused;
                        Serial.printf("[SNIFFER MODAL] Toggled Pause -> %s\n", isSnifferPaused ? "PAUSED" : "STREAMING");
                        return;
                    }
                    // Button 2: [ CLEAR ] (x: 104..180, y: 204..238)
                    else if (touchLastX >= 104 && touchLastX <= 180 && touchLastY >= 204 && touchLastY <= 238) {
                        for (int i = 0; i < SNIFFER_HISTORY_SIZE; i++) {
                            snifferHistory[i].id = 0;
                            snifferHistory[i].dlc = 0;
                        }
                        snifferHead = 0;
                        Serial.println("[SNIFFER MODAL] Cleared history buffer.");
                        return;
                    }
                    // Button 3: [ BACK / CLOSE ] (x: 186..308, y: 204..238)
                    else if (touchLastX >= 186 && touchLastX <= 308 && touchLastY >= 204 && touchLastY <= 238) {
                        isRawSnifferModalOpen = false;
                        Serial.println("[SNIFFER MODAL] Closed modal -> Returning to Sniffer Page.");
                        return;
                    }
                    return;
                }
                // When Normal Page 1 Sniffer View is Open
                else {
                    // Card 1: CAN Logger Start / Stop (y: 28 - 82)
                    if (touchLastY >= 28 && touchLastY <= 82) {
                        if (currentLogMode == LOG_CANBUS) {
                            stopActiveLogger();
                        } else if (currentLogMode == LOG_IDLE) {
                            startCanbusLogger();
                        }
                        return;
                    }
                    // Card 3: Open Raw Packet Terminal (y: 152 - 204)
                    else if (touchLastY >= 152 && touchLastY <= 204) {
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
                    int startY = 28;
                    int rowHeight = 28;
                    int colWidth = 144;

                    for (size_t i = 0; i < PID_COUNT; i++) {
                        int col = (i % 2);
                        int row = (i / 2);
                        int bx = 12 + col * (colWidth + 8);
                        int by = startY + row * (rowHeight + 2);

                        if (touchLastX >= bx && touchLastX <= bx + colWidth &&
                            touchLastY >= by && touchLastY <= by + rowHeight) {
                            availablePids[i].enabled = !availablePids[i].enabled;
                            Serial.printf("[PID PICKER] Toggled %s -> %s\n", availablePids[i].idStr, availablePids[i].enabled ? "ON" : "OFF");
                            return;
                        }
                    }

                    int botY = 210;
                    if (touchLastY >= botY && touchLastY <= botY + 28) {
                        if (touchLastX >= 12 && touchLastX <= 77) {
                            for (size_t i = 0; i < PID_COUNT; i++) availablePids[i].enabled = true;
                        } else if (touchLastX >= 85 && touchLastX <= 150) {
                            for (size_t i = 0; i < PID_COUNT; i++) availablePids[i].enabled = false;
                        } else if (touchLastX >= 160 && touchLastX <= 310) {
                            isPidConfigOpen = false;
                        }
                        return;
                    }
                }
                // When Normal Page 2 View is Open
                else {
                    // Card 1: PID Datalogger Start / Stop (y: 28 - 82)
                    if (touchLastY >= 28 && touchLastY <= 82) {
                        if (currentLogMode == LOG_DATALOG) {
                            stopActiveLogger();
                        } else if (currentLogMode == LOG_IDLE) {
                            startDataLogger();
                        }
                        return;
                    }
                    // Card 3: Configure PIDs Button (y: 152 - 204)
                    else if (touchLastY >= 152 && touchLastY <= 204) {
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
                // Card 1: 180-deg Display Flip (y: 28 - 80)
                if (touchLastY >= 28 && touchLastY <= 80) {
                    saveDisplayFlipSetting(!isDisplayFlipped);
                    return;
                }
                // Card 2: Brightness Step Buttons (y: 106 - 140)
                else if (touchLastY >= 106 && touchLastY <= 140) {
                    const uint8_t brLevels[4] = {64, 128, 192, 255};
                    for (int i = 0; i < 4; i++) {
                        int bx = 26 + i * 68;
                        if (touchLastX >= bx && touchLastX <= bx + 62) {
                            saveBrightnessSetting(brLevels[i]);
                            return;
                        }
                    }
                    return;
                }
                // Card 3: Reboot Controller (y: 148 - 192)
                else if (touchLastY >= 148 && touchLastY <= 192) {
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
            activeLogFile.printf("%lu,0x%03X,%d,%d,", millis(), message.identifier, message.extd, message.data_length_code);
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
        for (size_t i = 0; i < PID_COUNT; i++) {
            if (!availablePids[i].enabled) continue;
            row += ",";
            if (strcmp(availablePids[i].idStr, "RPM") == 0) {
                row += String(vehicleData.rpm);
            } else if (strcmp(availablePids[i].idStr, "SPEED") == 0) {
                row += String(vehicleData.speedMph);
            } else if (strcmp(availablePids[i].idStr, "THR") == 0) {
                row += String(vehicleData.throttlePct);
            } else if (strcmp(availablePids[i].idStr, "LOAD") == 0) {
                row += String(vehicleData.engineLoadPct);
            } else if (strcmp(availablePids[i].idStr, "AFR") == 0) {
                row += String(vehicleData.commandedAfr, 2);
                row += ",";
                row += String(vehicleData.actualAfr, 2);
            } else if (strcmp(availablePids[i].idStr, "KCLV") == 0) {
                row += String(vehicleData.kclv, 1);
            } else if (strcmp(availablePids[i].idStr, "KFB") == 0) {
                row += String(vehicleData.knockFB, 1);
            } else if (strcmp(availablePids[i].idStr, "ECT") == 0) {
                row += String(vehicleData.coolantTempC);
            } else if (strcmp(availablePids[i].idStr, "GEAR") == 0) {
                row += String(vehicleData.gear);
                row += ",";
                row += vehicleData.tccLocked ? "1" : "0";
            } else if (strcmp(availablePids[i].idStr, "IAT") == 0) {
                row += String(vehicleData.iatC);
            } else if (strcmp(availablePids[i].idStr, "MAF") == 0) {
                row += String(vehicleData.mafGps, 2);
            } else if (strcmp(availablePids[i].idStr, "TIMING") == 0) {
                row += String(vehicleData.timingDeg, 1);
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

    // 4. Display: RGB panel frame buffer lives in the 8MB OPI PSRAM
    tft.init();
    tft.setRotation(isDisplayFlipped ? 2 : 0);
    Serial.printf("[DISPLAY] Panel ready: %dx%d, PSRAM frame buffer: %s\n",
                  tft.width(), tft.height(), psramFound() ? "yes" : "NO (memory_type mismatch!)");

    // Draw into a PSRAM-backed canvas, then push whole frames to the panel.
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    canvas.createSprite(tft.width(), tft.height());

    backlightOn();
    initGt911Touch();
    initRtc();

    // 5. OEM Toyota Boot Splash Screen
    showToyotaBootSplash();

    // 6. Wi-Fi SoftAP & SavvyCAN Streaming Server
    initWiFiStreaming();

    // 7. CAN Bus & MicroSD
    initCAN();
    mountSD();

    lastUserActivityTime = millis();
}

void loop() {
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
    if (millis() - lastCanActivityTime < 3000 && (millis() - lastObdQueryTime >= 250)) {
        lastObdQueryTime = millis();
        sendToyotaObdQueries();
    }

    // MicroSD retry
    if (!sdMounted && (millis() - lastSdRetryTime >= 5000)) {
        lastSdRetryTime = millis();
        mountSD();
    }

    // Update Message Rate calculation
    if (millis() - lastPPSCheck >= 1000) {
        currentPPS = (float)ppsCount * 1000.0f / (millis() - lastPPSCheck);
        ppsCount = 0;
        lastPPSCheck = millis();
    }

    // 30 FPS Display Refresh
    if (millis() - lastDisplayUpdate >= 33) {
        lastDisplayUpdate = millis();
        updateDisplay();
    }
}
