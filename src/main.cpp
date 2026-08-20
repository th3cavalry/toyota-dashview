#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <Wire.h>
#include "driver/twai.h"
#include <LovyanGFX.hpp>
#include "version.h"
#include "toyota_splash.h"

// =========================================================================
// Waveshare ESP32-S3-Touch-LCD-2.8 V2 Hardware Configuration
// =========================================================================
class LGFX_Waveshare28 : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789      _panel_instance;
    lgfx::Bus_SPI           _bus_instance;
    lgfx::Light_PWM         _light_instance;

public:
    LGFX_Waveshare28(void) {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read  = 16000000;
            cfg.spi_3wire  = false;
            cfg.use_lock   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 40; // LCD_SCLK
            cfg.pin_mosi = 45; // LCD_MOSI
            cfg.pin_miso = -1;
            cfg.pin_dc   = 41; // LCD_DC
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = 42; // LCD_CS
            cfg.pin_rst          = 39; // LCD_RST
            cfg.pin_busy         = -1;
            cfg.panel_width      = 240;
            cfg.panel_height     = 320;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = true; // IPS panel color inversion
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel_instance.config(cfg);
        }

        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = 5; // LCD_BL (Backlight PWM)
            cfg.invert = false;
            cfg.freq   = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        setPanel(&_panel_instance);
    }
};

LGFX_Waveshare28 tft;
LGFX_Sprite canvas(&tft); // Double-buffer sprite for 60FPS flicker-free rendering

// =========================================================================
// Pin Definitions (Waveshare ESP32-S3-Touch-LCD-2.8 V2)
// =========================================================================

// Waveshare SN65HVD230 CAN Transceiver (Connected to 12-PIN Header TXD & RXD)
#define CAN_TX_PIN         GPIO_NUM_43
#define CAN_RX_PIN         GPIO_NUM_44

// MicroSD (TF Card Slot) SPI Pins
#define SD_MOSI_PIN        17
#define SD_MISO_PIN        16
#define SD_SCK_PIN         14
#define SD_CS_PIN          21

// Capacitive Touch I2C Pins (Hynitron CST3530 V2 Controller)
#define TP_SDA_PIN         1
#define TP_SCL_PIN         3
#define TP_INT_PIN         4
#define TP_RST_PIN         2
#define CST3530_I2C_ADDR   0x58

const uint8_t CST3530_DATA_REG[4]     = {0xD0, 0x07, 0x00, 0x00};
const uint8_t CST3530_END_READ_REG[4] = {0xD0, 0x00, 0x02, 0xAB};

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

bool isPidConfigOpen = false; // Is the PID selection modal/view open
bool isBootSplashActive = true; // Keep boot splash until screen tapped or engine starts (RPM > 0)

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

// 5 Dedicated Full-Color UI Screens
enum DisplayScreen {
    SCREEN_DASHBOARD = 0,
    SCREEN_SNIFFER   = 1,
    SCREEN_LOGGER    = 2, // Dedicated Datalog & CAN Logger Control Page
    SCREEN_WIFI      = 3,
    SCREEN_SYSTEM    = 4,
    SCREEN_COUNT     = 5
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
#define SNIFFER_HISTORY_SIZE 7
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

    if (twai_start() == ESP_OK) {
        Serial.println("[CAN] TWAI started successfully.");
    } else {
        Serial.println("[CAN] Failed to start TWAI.");
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
    queryMsg.identifier = 0x7E0;
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

void decodeTacomaFrame(const twai_message_t &msg) {
    if (msg.identifier == 0x0B4 && msg.data_length_code >= 8) {
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
    else if (msg.identifier == 0x7E8 && msg.data_length_code >= 4) {
        if (msg.data[1] == 0x61 && msg.data[2] == 0xA2 && msg.data_length_code >= 6) {
            float rawKclv = msg.data[3] * 0.1f;
            if (rawKclv >= 10.0f && rawKclv <= 30.0f) {
                vehicleData.kclv = rawKclv;
            }
            int8_t rawKnock = (int8_t)msg.data[4];
            vehicleData.knockFB = rawKnock * 0.1f;
        }
        else if (msg.data[1] == 0x41) {
            if (msg.data[2] == 0x04 && msg.data_length_code >= 4) {
                vehicleData.engineLoadPct = (msg.data[3] * 100) / 255;
            }
            else if (msg.data[2] == 0x05 && msg.data_length_code >= 4) {
                vehicleData.coolantTempC = (int)msg.data[3] - 40;
            }
            else if (msg.data[2] == 0x0F && msg.data_length_code >= 4) {
                vehicleData.iatC = (int)msg.data[3] - 40;
            }
            else if (msg.data[2] == 0x10 && msg.data_length_code >= 5) {
                vehicleData.mafGps = ((msg.data[3] << 8) | msg.data[4]) / 100.0f;
            }
            else if (msg.data[2] == 0x0E && msg.data_length_code >= 4) {
                vehicleData.timingDeg = ((float)msg.data[3] / 2.0f) - 64.0f;
            }
            else if (msg.data[2] == 0x24 && msg.data_length_code >= 6) {
                uint16_t rawLambda = (msg.data[3] << 8) | msg.data[4];
                float lambda = (float)rawLambda / 32768.0f;
                if (lambda > 0.5f && lambda < 2.0f) {
                    vehicleData.actualAfr = lambda * 14.7f;
                }
            }
            else if (msg.data[2] == 0x44 && msg.data_length_code >= 5) {
                uint16_t rawCmd = (msg.data[3] << 8) | msg.data[4];
                float lambdaCmd = (float)rawCmd / 32768.0f;
                if (lambdaCmd > 0.5f && lambdaCmd < 2.0f) {
                    vehicleData.commandedAfr = lambdaCmd * 14.7f;
                }
            }
        }
    }
}

void recordSnifferFrame(const twai_message_t &msg) {
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
    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (SD.begin(SD_CS_PIN, sdSPI, 20000000)) {
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
    for (int i = 1; i <= 9999; i++) {
        snprintf(filename, sizeof(filename), "/%s_%04d.csv", prefix, i);
        if (!SD.exists(filename)) {
            return String(filename);
        }
    }
    snprintf(filename, sizeof(filename), "/%s_%lu.csv", prefix, millis() / 1000);
    return String(filename);
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
// =========================================================================
void wakeScreen() {
    lastUserActivityTime = millis();
    if (isScreenDimmed) {
        isScreenDimmed = false;
        tft.setBrightness(255);
        Serial.println("[DISPLAY] Screen Brightness Restored to 100%.");
    }
}

void dimScreen() {
    if (!isScreenDimmed) {
        isScreenDimmed = true;
        tft.setBrightness(35);
        Serial.println("[DISPLAY] Screen Dimmed to 15% (1 min timeout).");
    }
}

// =========================================================================
// Native Waveshare V2 CST3530 Capacitive Touch Driver
// =========================================================================
void initCst3530Touch() {
    pinMode(TP_INT_PIN, INPUT_PULLUP);
    pinMode(TP_RST_PIN, OUTPUT);
    digitalWrite(TP_RST_PIN, LOW);
    delay(20);
    digitalWrite(TP_RST_PIN, HIGH);
    delay(100);

    Wire1.begin(TP_SDA_PIN, TP_SCL_PIN, 400000);
    Serial.println("[TOUCH] Initialized CST3530 V2 Touch Controller on Wire1 (SDA:1, SCL:3, addr:0x58)");
}

bool pollCst3530Touch(int &screenX, int &screenY) {
    Wire1.beginTransmission(CST3530_I2C_ADDR);
    Wire1.write(CST3530_DATA_REG, 4);
    if (Wire1.endTransmission(false) != 0) {
        return false;
    }

    if (Wire1.requestFrom((uint8_t)CST3530_I2C_ADDR, (uint8_t)9) != 9) {
        return false;
    }

    uint8_t buf[9];
    Wire1.readBytes(buf, 9);

    Wire1.beginTransmission(CST3530_I2C_ADDR);
    Wire1.write(CST3530_END_READ_REG, 4);
    Wire1.endTransmission(true);

    uint8_t count = buf[3] & 0x0F;
    if (count == 0 || (buf[8] & 0xF0) == 0x00) {
        return false;
    }

    uint16_t rawX = ((uint16_t)(buf[7] & 0x0F) << 8) | buf[4];
    uint16_t rawY = ((uint16_t)(buf[7] & 0xF0) << 4) | buf[5];

    screenX = rawY;
    screenY = 240 - rawX;

    if (screenX < 0) screenX = 0;
    if (screenX > 320) screenX = 320;
    if (screenY < 0) screenY = 0;
    if (screenY > 240) screenY = 240;

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

// Page 1: Live CAN Sniffer (Dark Terminal)
void renderSniffer() {
    char buf[64];
    snprintf(buf, sizeof(buf), "CAN SNIFFER (Total: %lu)", packetCount);
    drawHeaderBar(buf);

    int rowY = 30;
    for (int i = 0; i < SNIFFER_HISTORY_SIZE; i++) {
        int idx = (snifferHead - 1 - i + SNIFFER_HISTORY_SIZE) % SNIFFER_HISTORY_SIZE;
        if (snifferHistory[idx].id == 0 && snifferHistory[idx].dlc == 0) continue;

        uint16_t rowBg = (i % 2 == 0) ? C_CARD_BG : canvas.color565(22, 26, 36);
        canvas.fillRoundRect(8, rowY, 304, 23, 4, rowBg);
        canvas.drawRoundRect(8, rowY, 304, 23, 4, C_CARD_BORDER);
        
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

        rowY += 26;
    }

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

// Page 2: Datalog & CAN Logger Control Center (Motorsport Control Deck)
void renderLoggerControl() {
    if (isPidConfigOpen) {
        renderPidSelector();
        return;
    }

    drawHeaderBar("DATALOG & CAN LOGGER CONTROL");

    char buf[64];
    unsigned long elapsedSec = (currentLogMode != LOG_IDLE) ? ((millis() - logStartTime) / 1000) : 0;

    // 1. BUTTON 1: Raw CAN Bus Logger (canbus_XXXX.csv)
    bool isCanActive = (currentLogMode == LOG_CANBUS);
    bool canDisabled = (currentLogMode == LOG_DATALOG);

    uint16_t canBgColor = isCanActive ? canvas.color565(55, 14, 20) : (canDisabled ? canvas.color565(16, 18, 24) : C_CARD_BG);
    uint16_t canBorderColor = isCanActive ? C_TRD_RED : (canDisabled ? canvas.color565(35, 40, 52) : C_CARD_BORDER);

    canvas.fillRoundRect(12, 28, 296, 48, 6, canBgColor);
    canvas.drawRoundRect(12, 28, 296, 48, 6, canBorderColor);
    canvas.fillRect(14, 28, 4, 48, isCanActive ? C_TRD_RED : C_TRD_ORANGE);

    canvas.setFont(&fonts::Font4);
    if (isCanActive) {
        canvas.setTextColor(C_TRD_RED);
        canvas.drawString("[STOP CAN LOGGER]", 26, 32);
        canvas.setFont(&fonts::Font2);
        snprintf(buf, sizeof(buf), "REC: %s (%lu frames, %lum%02lus)", currentLogFileName, logEntryCount, elapsedSec / 60, elapsedSec % 60);
        canvas.setTextColor(canvas.color565(255, 200, 200));
        canvas.drawString(buf, 26, 54);
    } else {
        canvas.setTextColor(canDisabled ? C_TEXT_MUTED : C_TEXT_WHITE);
        canvas.drawString("[START CAN LOG]", 26, 32);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(canDisabled ? canvas.color565(80, 85, 100) : C_TEXT_MUTED);
        canvas.drawString(canDisabled ? "Locked (Stop PID Datalogger first)" : "Raw CAN frames -> canbus_XXXX.csv", 26, 54);
    }

    // 2. BUTTON 2: PID Datalogger (datalog_XXXX.csv)
    bool isDatalogActive = (currentLogMode == LOG_DATALOG);
    bool datalogDisabled = (currentLogMode == LOG_CANBUS);

    uint16_t dlBgColor = isDatalogActive ? canvas.color565(55, 30, 10) : (datalogDisabled ? canvas.color565(16, 18, 24) : C_CARD_BG);
    uint16_t dlBorderColor = isDatalogActive ? C_TRD_ORANGE : (datalogDisabled ? canvas.color565(35, 40, 52) : C_CARD_BORDER);

    canvas.fillRoundRect(12, 82, 296, 48, 6, dlBgColor);
    canvas.drawRoundRect(12, 82, 296, 48, 6, dlBorderColor);
    canvas.fillRect(14, 82, 4, 48, isDatalogActive ? C_TRD_ORANGE : C_TEXT_CYAN);

    canvas.setFont(&fonts::Font4);
    if (isDatalogActive) {
        canvas.setTextColor(C_TRD_ORANGE);
        canvas.drawString("[STOP PID DATALOG]", 26, 86);
        canvas.setFont(&fonts::Font2);
        snprintf(buf, sizeof(buf), "REC: %s (%lu samples, %lum%02lus)", currentLogFileName, logEntryCount, elapsedSec / 60, elapsedSec % 60);
        canvas.setTextColor(canvas.color565(255, 230, 180));
        canvas.drawString(buf, 26, 108);
    } else {
        canvas.setTextColor(datalogDisabled ? C_TEXT_MUTED : C_TEXT_WHITE);
        canvas.drawString("[START PID DATALOG]", 26, 86);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(datalogDisabled ? canvas.color565(80, 85, 100) : C_TEXT_MUTED);
        snprintf(buf, sizeof(buf), "Logs %d Selected PIDs -> datalog_XXXX.csv", getActivePidCount());
        canvas.drawString(canDisabled ? "Locked (Stop CAN Logger first)" : buf, 26, 108);
    }

    // 3. BUTTON 3: Configure Selectable PIDs Button
    canvas.fillRoundRect(12, 136, 296, 40, 6, C_CARD_BG);
    canvas.drawRoundRect(12, 136, 296, 40, 6, C_CARD_BORDER);
    canvas.fillRect(14, 136, 4, 40, C_TRD_BURGUNDY);

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_TEXT_WHITE);
    snprintf(buf, sizeof(buf), "[+] CONFIGURE PIDs (%d of %d Active)", getActivePidCount(), (int)PID_COUNT);
    canvas.drawString(buf, 26, 142);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("Tap here to customize parameters recorded to SD", 26, 160);

    // 4. Status Footer
    canvas.fillRoundRect(12, 182, 296, 30, 4, C_CARD_INNER);
    canvas.drawRoundRect(12, 182, 296, 30, 4, C_CARD_BORDER);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT_CYAN);
    canvas.drawString("Sampling Rate: 10 Hz (100ms) | Storage: MicroSD FAT32", 20, 192);

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

void updateDisplay() {
    canvas.fillScreen(C_DARK_BG);

    switch (currentScreen) {
        case SCREEN_DASHBOARD: renderDashboard();     break;
        case SCREEN_SNIFFER:   renderSniffer();       break;
        case SCREEN_LOGGER:    renderLoggerControl(); break;
        case SCREEN_WIFI:      renderWiFi();          break;
        case SCREEN_SYSTEM:    renderSystem();        break;
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
void handleTouch() {
    int touchX = 0, touchY = 0;
    bool touched = pollCst3530Touch(touchX, touchY);

    if (touched) {
        wakeScreen();
        if (!wasTouched) {
            wasTouched = true;
            touchStartX = touchX;
            touchStartY = touchY;
            touchLastX  = touchX;
            touchLastY  = touchY;
            touchStartTime = millis();
            Serial.printf("[TOUCH V2] Press at (%d, %d)\n", touchX, touchY);
        } else {
            touchLastX = touchX;
            touchLastY = touchY;
        }
    } else if (wasTouched) {
        wasTouched = false;
        int deltaX = touchLastX - touchStartX;
        int deltaY = touchLastY - touchStartY;
        unsigned long duration = millis() - touchStartTime;

        Serial.printf("[TOUCH V2] Release at (%d, %d) | deltaX=%d, deltaY=%d, dur=%lums\n", 
                      touchLastX, touchLastY, deltaX, deltaY, duration);

        // 1. Horizontal Swipe Gesture (Drag >= 20px)
        if (abs(deltaX) >= 20 && duration < 1000 && !isPidConfigOpen) {
            if (deltaX < -20) {
                nextScreen(); // Drag right-to-left -> Next Page
            } else if (deltaX > 20) {
                prevScreen(); // Drag left-to-right -> Prev Page
            }
        } 
        // 2. Button / Screen Tap Detection (duration < 500ms and minimal drag)
        else if (duration < 500 && abs(deltaX) < 20 && abs(deltaY) < 20) {
            // A. PID Selector Modal Tap Handling
            if (currentScreen == SCREEN_LOGGER && isPidConfigOpen) {
                // Check Grid Taps
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

                // Check Bottom Buttons in PID Picker
                int botY = 210;
                if (touchLastY >= botY && touchLastY <= botY + 28) {
                    if (touchLastX >= 12 && touchLastX <= 77) {
                        // [ ALL ]
                        for (size_t i = 0; i < PID_COUNT; i++) availablePids[i].enabled = true;
                    } else if (touchLastX >= 85 && touchLastX <= 150) {
                        // [ NONE ]
                        for (size_t i = 0; i < PID_COUNT; i++) availablePids[i].enabled = false;
                    } else if (touchLastX >= 160 && touchLastX <= 310) {
                        // [ SAVE & RETURN ]
                        isPidConfigOpen = false;
                    }
                    return;
                }
            }
            // B. Page 2 (Logger Control Screen) Normal View
            else if (currentScreen == SCREEN_LOGGER && !isPidConfigOpen) {
                // Button 1: CAN Bus Logger (y: 28 - 76)
                if (touchLastY >= 28 && touchLastY <= 76) {
                    if (currentLogMode == LOG_CANBUS) {
                        stopActiveLogger();
                    } else if (currentLogMode == LOG_IDLE) {
                        startCanbusLogger();
                    }
                    return;
                }
                // Button 2: PID Datalogger (y: 82 - 130)
                else if (touchLastY >= 82 && touchLastY <= 130) {
                    if (currentLogMode == LOG_DATALOG) {
                        stopActiveLogger();
                    } else if (currentLogMode == LOG_IDLE) {
                        startDataLogger();
                    }
                    return;
                }
                // Button 3: Configure PIDs (y: 136 - 176)
                else if (touchLastY >= 136 && touchLastY <= 176) {
                    if (currentLogMode == LOG_IDLE) {
                        isPidConfigOpen = true;
                        Serial.println("[PID PICKER] Opened PID Config Screen.");
                    }
                    return;
                }
            }

            // Bottom Navigation Bar Tap
            if (touchLastY >= 210 && !isPidConfigOpen) {
                if (touchLastX > 160) {
                    nextScreen();
                } else {
                    prevScreen();
                }
            } 
            // General Screen Tap on other pages
            else if (currentScreen != SCREEN_LOGGER) {
                if (touchLastX > 160) {
                    nextScreen();
                } else {
                    prevScreen();
                }
            }
        }
    }
}

// =========================================================================
// Main Loop & CAN Processing
// =========================================================================
void processCAN() {
    twai_message_t message;
    while (twai_receive(&message, 0) == ESP_OK) {
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
    Serial.printf("\n=== %s %s (ESP32-S3 Touch 2.8 V2) ===\n", APP_NAME, APP_VERSION_STR);

    // 1. Initialize Display & Backlight
    tft.init();
    tft.setRotation(1); // Landscape 320x240
    tft.setBrightness(255);

    // Create 320x240 Canvas Sprite
    canvas.setColorDepth(16);
    canvas.createSprite(320, 240);

    // 2. OEM Toyota Boot Splash Screen
    showToyotaBootSplash();

    // 3. Capacitive Touch Controller (Native CST3530 V2 Driver)
    initCst3530Touch();

    // 4. Wi-Fi SoftAP & SavvyCAN Streaming Server
    initWiFiStreaming();

    // 5. CAN Bus & MicroSD
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
        if (pollCst3530Touch(touchX, touchY)) {
            isBootSplashActive = false;
            wakeScreen();
            lastUserActivityTime = millis();
            Serial.println("[SPLASH] Screen tapped -> Exiting splash to Dashboard.");
        }

        // 2. Check if engine started (RPM > 0 or Speed > 0)
        if (vehicleData.rpm > 0 || vehicleData.speedMph > 0) {
            isBootSplashActive = false;
            wakeScreen();
            lastUserActivityTime = millis();
            Serial.printf("[SPLASH] Engine started (RPM: %d) -> Exiting splash to Dashboard.\n", vehicleData.rpm);
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
