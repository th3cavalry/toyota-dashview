#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <Wire.h>
#include "driver/twai.h"
#include <LovyanGFX.hpp>

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
const char* WIFI_SSID = "Tacoma-CAN-Logger";
const char* WIFI_PASS = "tacoma123";
#define SAVVYCAN_PORT 23

WiFiServer tcpServer(SAVVYCAN_PORT);
WiFiClient savvyClient;
bool wifiClientConnected = false;
unsigned long wifiStreamedCount = 0;

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
    SCREEN_LOGGER    = 2, // New Dedicated Datalog & CAN Logger Control Page
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

void sendToyotaObdQueries() {
    twai_message_t queryMsg;
    queryMsg.identifier = 0x7E0;
    queryMsg.extd = 0;
    queryMsg.rtr = 0;
    queryMsg.data_length_code = 8;
    memset(queryMsg.data, 0, 8);

    if (obdQueryIndex == 0) {
        queryMsg.data[0] = 0x02;
        queryMsg.data[1] = 0x21;
        queryMsg.data[2] = 0xA2; // Mode 21 PID A2: KCLV & KnockFB
    } else if (obdQueryIndex == 1) {
        queryMsg.data[0] = 0x02;
        queryMsg.data[1] = 0x01;
        queryMsg.data[2] = 0x24; // Mode 1 PID 24: Actual A/F Sensor Lambda
    } else if (obdQueryIndex == 2) {
        queryMsg.data[0] = 0x02;
        queryMsg.data[1] = 0x01;
        queryMsg.data[2] = 0x44; // Mode 1 PID 44: Commanded Equivalence Ratio
    } else {
        queryMsg.data[0] = 0x02;
        queryMsg.data[1] = 0x01;
        queryMsg.data[2] = 0x05; // Mode 1 PID 05: Engine Coolant Temp
    }

    twai_transmit(&queryMsg, 0);
    obdQueryIndex = (obdQueryIndex + 1) % 4;
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

// Find next available non-overwriting file: /prefix_0001.csv, /prefix_0002.csv ...
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
        activeLogFile.println("Timestamp_ms,RPM,Speed_MPH,Throttle_Pct,Engine_Load_Pct,Cmd_AFR,Act_AFR,Cmd_Lambda,Act_Lambda,KCLV,Knock_FB_deg,Coolant_C");
        activeLogFile.flush();
        currentLogMode = LOG_DATALOG;
        strncpy(currentLogFileName, path.c_str(), sizeof(currentLogFileName));
        logStartTime = millis();
        logEntryCount = 0;
        lastLogFlushTime = millis();
        lastDatalogSampleTime = millis();
        Serial.printf("[LOGGER] >>> Started PID Datalogger: %s\n", currentLogFileName);
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

    // Map native 240x320 portrait to 320x240 landscape (Rotation 1)
    screenX = rawY;
    screenY = 240 - rawX;

    if (screenX < 0) screenX = 0;
    if (screenX > 320) screenX = 320;
    if (screenY < 0) screenY = 0;
    if (screenY > 240) screenY = 240;

    return true;
}

// =========================================================================
// Full-Color 320x240 UI Page Renderers
// =========================================================================

void drawHeaderBar(const char* title) {
    canvas.fillRect(0, 0, 320, 24, canvas.color565(18, 18, 24));
    canvas.fillRect(0, 0, 4, 24, canvas.color565(235, 10, 30));
    canvas.setTextColor(TFT_WHITE, canvas.color565(18, 18, 24));
    canvas.setFont(&fonts::Font2);
    canvas.drawString(title, 10, 4);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f msg/s", currentPPS);
    canvas.setTextColor(canvas.color565(0, 220, 255), canvas.color565(18, 18, 24));
    canvas.drawRightString(buf, 240, 4);

    const char* sdTag = (currentLogMode == LOG_CANBUS) ? "[CAN REC]" : ((currentLogMode == LOG_DATALOG) ? "[PID REC]" : (sdMounted ? "SD:OK" : "NO SD"));
    uint16_t sdColor = (currentLogMode != LOG_IDLE) ? canvas.color565(255, 60, 60) : (sdMounted ? canvas.color565(80, 220, 80) : canvas.color565(120, 120, 120));
    canvas.setTextColor(sdColor, canvas.color565(18, 18, 24));
    canvas.drawRightString(sdTag, 315, 4);

    canvas.drawFastHLine(0, 24, 320, canvas.color565(40, 40, 50));
}

void drawBottomNavBar() {
    canvas.fillRect(0, 218, 320, 22, canvas.color565(15, 15, 20));
    canvas.drawFastHLine(0, 218, 320, canvas.color565(40, 40, 50));

    canvas.setTextColor(canvas.color565(140, 140, 160));
    canvas.setFont(&fonts::Font2);
    canvas.drawString("< PREV", 10, 222);

    int dotSpacing = 16;
    int startDotX = 160 - (((SCREEN_COUNT - 1) * dotSpacing) / 2);
    for (int i = 0; i < SCREEN_COUNT; i++) {
        int dx = startDotX + (i * dotSpacing);
        if (i == currentScreen) {
            canvas.fillCircle(dx, 228, 4, canvas.color565(0, 220, 255));
        } else {
            canvas.fillCircle(dx, 228, 2, canvas.color565(70, 70, 90));
        }
    }

    canvas.drawRightString("NEXT >", 310, 222);
}

// Page 0: Live Vehicle Cluster
void renderDashboard() {
    drawHeaderBar("TOYOTA TACOMA DASHBOARD");

    int rpmY = 30;
    canvas.drawRoundRect(10, rpmY, 300, 16, 4, canvas.color565(60, 60, 75));
    int rpmWidth = map(constrain(vehicleData.rpm, 0, 6000), 0, 6000, 0, 296);
    
    if (rpmWidth > 0) {
        for (int i = 0; i < rpmWidth; i++) {
            uint16_t barColor;
            if (i < 170) {
                barColor = canvas.color565(0, 220, 100);
            } else if (i < 240) {
                barColor = canvas.color565(255, 200, 0);
            } else {
                barColor = canvas.color565(255, 40, 40);
            }
            canvas.drawFastVLine(12 + i, rpmY + 2, 12, barColor);
        }
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "%d RPM", vehicleData.rpm);
    canvas.setTextColor(TFT_WHITE);
    canvas.setFont(&fonts::Font2);
    canvas.drawRightString(buf, 305, rpmY + 18);

    canvas.fillRoundRect(10, 56, 90, 95, 6, canvas.color565(25, 28, 38));
    canvas.drawRoundRect(10, 56, 90, 95, 6, canvas.color565(60, 65, 85));
    
    canvas.setTextColor(canvas.color565(140, 145, 165));
    canvas.setFont(&fonts::Font0);
    canvas.drawCenterString("GEAR", 55, 62);

    canvas.setFont(&fonts::Font7);
    if (vehicleData.tccLocked && vehicleData.gear[0] >= '1' && vehicleData.gear[0] <= '6') {
        snprintf(buf, sizeof(buf), "%sL", vehicleData.gear);
        canvas.setTextColor(canvas.color565(255, 215, 0));
    } else {
        snprintf(buf, sizeof(buf), "%s", vehicleData.gear);
        canvas.setTextColor(TFT_WHITE);
    }
    canvas.drawCenterString(buf, 55, 78);

    if (vehicleData.tccLocked) {
        canvas.fillRoundRect(18, 130, 74, 16, 3, canvas.color565(200, 160, 0));
        canvas.setTextColor(TFT_BLACK);
        canvas.setFont(&fonts::Font2);
        canvas.drawCenterString("LOCKED", 55, 131);
    }

    canvas.fillRoundRect(110, 56, 200, 45, 6, canvas.color565(25, 28, 38));
    canvas.drawRoundRect(110, 56, 200, 45, 6, canvas.color565(60, 65, 85));

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(canvas.color565(140, 180, 255));
    snprintf(buf, sizeof(buf), "Cmd AFR: %.1f  (%.2f \xce\xbb)", vehicleData.commandedAfr, vehicleData.commandedAfr / 14.7f);
    canvas.drawString(buf, 118, 62);

    uint16_t actAfrColor = (vehicleData.actualAfr > 15.2f) ? canvas.color565(255, 80, 80) : canvas.color565(0, 255, 180);
    canvas.setTextColor(actAfrColor);
    snprintf(buf, sizeof(buf), "Act AFR: %.1f  (%.2f \xce\xbb)", vehicleData.actualAfr, vehicleData.actualAfr / 14.7f);
    canvas.drawString(buf, 118, 80);

    canvas.fillRoundRect(110, 106, 200, 45, 6, canvas.color565(25, 28, 38));
    canvas.drawRoundRect(110, 106, 200, 45, 6, canvas.color565(60, 65, 85));

    uint16_t kclvColor = (vehicleData.kclv >= 19.0f) ? canvas.color565(80, 255, 100) : ((vehicleData.kclv >= 15.0f) ? canvas.color565(255, 200, 0) : canvas.color565(255, 60, 60));
    canvas.setTextColor(kclvColor);
    snprintf(buf, sizeof(buf), "KCLV: %.1f", vehicleData.kclv);
    canvas.drawString(buf, 118, 112);

    uint16_t kfbColor = (vehicleData.knockFB < 0) ? canvas.color565(255, 80, 80) : canvas.color565(0, 220, 255);
    canvas.setTextColor(kfbColor);
    snprintf(buf, sizeof(buf), "KFB: %+2.1f\xb0", vehicleData.knockFB);
    canvas.drawString(buf, 215, 112);

    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(canvas.color565(140, 140, 160));
    canvas.drawString("Learned Knock Value (20.0 = Best)", 118, 133);

    int botY = 160;
    canvas.setTextColor(TFT_WHITE);
    canvas.setFont(&fonts::Font2);
    snprintf(buf, sizeof(buf), "Throttle: %d%%", vehicleData.throttlePct);
    canvas.drawString(buf, 10, botY);
    canvas.drawRoundRect(10, botY + 18, 145, 14, 3, canvas.color565(60, 60, 75));
    int thrWidth = map(constrain(vehicleData.throttlePct, 0, 100), 0, 100, 0, 141);
    if (thrWidth > 0) {
        canvas.fillRect(12, botY + 20, thrWidth, 10, canvas.color565(0, 180, 255));
    }

    snprintf(buf, sizeof(buf), "Engine Load: %d%%", vehicleData.engineLoadPct);
    canvas.drawString(buf, 165, botY);
    canvas.drawRoundRect(165, botY + 18, 145, 14, 3, canvas.color565(60, 60, 75));
    int loadWidth = map(constrain(vehicleData.engineLoadPct, 0, 100), 0, 100, 0, 141);
    if (loadWidth > 0) {
        canvas.fillRect(167, botY + 20, loadWidth, 10, canvas.color565(255, 140, 0));
    }

    drawBottomNavBar();
}

// Page 1: Live CAN Sniffer
void renderSniffer() {
    char buf[64];
    snprintf(buf, sizeof(buf), "CAN SNIFFER (Total: %lu)", packetCount);
    drawHeaderBar(buf);

    int rowY = 32;
    for (int i = 0; i < SNIFFER_HISTORY_SIZE; i++) {
        int idx = (snifferHead - 1 - i + SNIFFER_HISTORY_SIZE) % SNIFFER_HISTORY_SIZE;
        if (snifferHistory[idx].id == 0 && snifferHistory[idx].dlc == 0) continue;

        canvas.fillRoundRect(8, rowY, 304, 22, 4, (i % 2 == 0) ? canvas.color565(20, 22, 30) : canvas.color565(28, 30, 42));
        
        snprintf(buf, sizeof(buf), "0x%03X", snifferHistory[idx].id);
        canvas.setTextColor(canvas.color565(0, 220, 255));
        canvas.setFont(&fonts::Font2);
        canvas.drawString(buf, 14, rowY + 3);

        snprintf(buf, sizeof(buf), "[%d]", snifferHistory[idx].dlc);
        canvas.setTextColor(canvas.color565(160, 160, 180));
        canvas.drawString(buf, 70, rowY + 3);

        char hexBuf[36] = "";
        for (int b = 0; b < snifferHistory[idx].dlc && b < 8; b++) {
            char bStr[6];
            snprintf(bStr, sizeof(bStr), "%02X ", snifferHistory[idx].data[b]);
            strcat(hexBuf, bStr);
        }
        canvas.setTextColor(TFT_WHITE);
        canvas.drawString(hexBuf, 100, rowY + 3);

        rowY += 26;
    }

    drawBottomNavBar();
}

// Page 2: Dedicated Datalog & CAN Logger Control Center (Interactive Touch Buttons)
void renderLoggerControl() {
    drawHeaderBar("DATALOG & CAN LOGGER CONTROL");

    char buf[64];
    unsigned long elapsedSec = (currentLogMode != LOG_IDLE) ? ((millis() - logStartTime) / 1000) : 0;

    // ==========================================
    // BUTTON 1: CAN Bus Logger (canbus_XXXX.csv)
    // Box: x=12, y=32, w=296, h=56
    // ==========================================
    bool isCanActive = (currentLogMode == LOG_CANBUS);
    bool canDisabled = (currentLogMode == LOG_DATALOG); // Mutually exclusive locked

    uint16_t canBgColor = isCanActive ? canvas.color565(160, 20, 20) : (canDisabled ? canvas.color565(30, 32, 40) : canvas.color565(24, 70, 35));
    uint16_t canBorderColor = isCanActive ? canvas.color565(255, 60, 60) : (canDisabled ? canvas.color565(60, 60, 70) : canvas.color565(60, 200, 80));

    canvas.fillRoundRect(12, 32, 296, 56, 6, canBgColor);
    canvas.drawRoundRect(12, 32, 296, 56, 6, canBorderColor);

    canvas.setFont(&fonts::Font4);
    if (isCanActive) {
        canvas.setTextColor(TFT_WHITE);
        canvas.drawString("[STOP CAN LOGGER]", 22, 38);
        canvas.setFont(&fonts::Font2);
        snprintf(buf, sizeof(buf), "REC: %s (%lu frames, %lum%02lus)", currentLogFileName, logEntryCount, elapsedSec / 60, elapsedSec % 60);
        canvas.setTextColor(canvas.color565(255, 200, 200));
        canvas.drawString(buf, 22, 64);
    } else {
        canvas.setTextColor(canDisabled ? canvas.color565(100, 100, 120) : TFT_WHITE);
        canvas.drawString("[START CAN LOG]", 22, 38);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(canDisabled ? canvas.color565(80, 80, 90) : canvas.color565(180, 240, 180));
        canvas.drawString(canDisabled ? "Locked (Stop PID Datalogger first)" : "Logs all raw frames -> canbus_XXXX.csv", 22, 64);
    }

    // ==========================================
    // BUTTON 2: PID Datalogger (datalog_XXXX.csv)
    // Box: x=12, y=94, w=296, h=56
    // ==========================================
    bool isDatalogActive = (currentLogMode == LOG_DATALOG);
    bool datalogDisabled = (currentLogMode == LOG_CANBUS); // Mutually exclusive locked

    uint16_t dlBgColor = isDatalogActive ? canvas.color565(160, 80, 0) : (datalogDisabled ? canvas.color565(30, 32, 40) : canvas.color565(20, 50, 90));
    uint16_t dlBorderColor = isDatalogActive ? canvas.color565(255, 160, 0) : (datalogDisabled ? canvas.color565(60, 60, 70) : canvas.color565(60, 140, 255));

    canvas.fillRoundRect(12, 94, 296, 56, 6, dlBgColor);
    canvas.drawRoundRect(12, 94, 296, 56, 6, dlBorderColor);

    canvas.setFont(&fonts::Font4);
    if (isDatalogActive) {
        canvas.setTextColor(TFT_WHITE);
        canvas.drawString("[STOP PID DATALOG]", 22, 100);
        canvas.setFont(&fonts::Font2);
        snprintf(buf, sizeof(buf), "REC: %s (%lu samples, %lum%02lus)", currentLogFileName, logEntryCount, elapsedSec / 60, elapsedSec % 60);
        canvas.setTextColor(canvas.color565(255, 230, 180));
        canvas.drawString(buf, 22, 126);
    } else {
        canvas.setTextColor(datalogDisabled ? canvas.color565(100, 100, 120) : TFT_WHITE);
        canvas.drawString("[START PID DATALOG]", 22, 100);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(datalogDisabled ? canvas.color565(80, 80, 90) : canvas.color565(180, 220, 255));
        canvas.drawString(datalogDisabled ? "Locked (Stop CAN Logger first)" : "Logs RPM, Speed, AFR, KCLV, Load, ECT", 22, 126);
    }

    // ==========================================
    // Bottom Info & PID Summary Card
    // Box: x=12, y=156, w=296, h=56
    // ==========================================
    canvas.fillRoundRect(12, 156, 296, 56, 6, canvas.color565(20, 22, 30));
    canvas.drawRoundRect(12, 156, 296, 56, 6, canvas.color565(50, 55, 75));

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(canvas.color565(0, 220, 255));
    canvas.drawString("Logged PIDs (10 Hz Polling):", 20, 162);

    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("RPM | Speed | Throttle% | Engine Load% | ECT (Coolant)", 20, 182);
    canvas.drawString("Cmd/Act AFR | Cmd/Act Lambda | KCLV (Learned) | KFB (Knock)", 20, 196);

    drawBottomNavBar();
}

// Page 3: Wi-Fi & SavvyCAN Streaming
void renderWiFi() {
    drawHeaderBar("WI-FI SAVVYCAN STREAMING");

    canvas.fillRoundRect(15, 36, 290, 165, 6, canvas.color565(25, 28, 38));
    canvas.drawRoundRect(15, 36, 290, 165, 6, canvas.color565(60, 65, 85));

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Hotspot SSID:", 25, 48);
    canvas.setTextColor(canvas.color565(0, 220, 255));
    canvas.drawString(WIFI_SSID, 140, 48);

    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Password:", 25, 75);
    canvas.setTextColor(canvas.color565(255, 220, 100));
    canvas.drawString(WIFI_PASS, 140, 75);

    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("SavvyCAN Server:", 25, 102);
    canvas.setTextColor(canvas.color565(80, 255, 100));
    canvas.drawString("192.168.4.1:23", 160, 102);

    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Client Status:", 25, 130);
    if (savvyClient && savvyClient.connected()) {
        canvas.setTextColor(canvas.color565(0, 255, 180));
        char buf[32];
        snprintf(buf, sizeof(buf), "CONNECTED (%lu pkts)", wifiStreamedCount);
        canvas.drawString(buf, 140, 130);
    } else {
        canvas.setTextColor(canvas.color565(255, 160, 60));
        canvas.drawString("Waiting for Laptop...", 140, 130);
    }

    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(canvas.color565(160, 160, 180));
    canvas.drawString("Connect laptop to Wi-Fi -> In SavvyCAN add Network device (Port 23)", 25, 165);

    drawBottomNavBar();
}

// Page 4: System Diagnostics
void renderSystem() {
    drawHeaderBar("HARDWARE DIAGNOSTICS");

    canvas.fillRoundRect(15, 36, 290, 165, 6, canvas.color565(25, 28, 38));
    canvas.drawRoundRect(15, 36, 290, 165, 6, canvas.color565(60, 65, 85));

    char buf[64];
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(TFT_WHITE);

    snprintf(buf, sizeof(buf), "MCU: ESP32-S3 @ %d MHz", getCpuFrequencyMhz());
    canvas.drawString(buf, 25, 48);

    snprintf(buf, sizeof(buf), "Flash: 16 MB  |  PSRAM: %d MB", ESP.getPsramSize() / (1024 * 1024));
    canvas.drawString(buf, 25, 75);

    snprintf(buf, sizeof(buf), "Free Heap: %u KB", ESP.getFreeHeap() / 1024);
    canvas.drawString(buf, 25, 102);

    snprintf(buf, sizeof(buf), "MicroSD: %s (%s)", sdMounted ? "Mounted" : "None", (currentLogMode != LOG_IDLE) ? "ACTIVE" : "IDLE");
    canvas.drawString(buf, 25, 130);

    snprintf(buf, sizeof(buf), "CAN Pins: TX:IO%d  RX:IO%d (500k)", CAN_TX_PIN, CAN_RX_PIN);
    canvas.drawString(buf, 25, 158);

    drawBottomNavBar();
}

void updateDisplay() {
    canvas.fillScreen(canvas.color565(10, 12, 16));

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
// Pure Toyota 3-Oval Boot Splash
// =========================================================================
void showToyotaBootSplash() {
    tft.fillScreen(TFT_BLACK);

    int cx = 160;
    int cy = 105;

    for (int t = 0; t < 6; t++) {
        tft.drawEllipse(cx, cy, 100 - t, 60 - t, tft.color565(235, 10, 30));
    }

    for (int t = 0; t < 4; t++) {
        tft.drawEllipse(cx, cy - 14, 68 - t, 32 - t, TFT_WHITE);
    }

    for (int t = 0; t < 4; t++) {
        tft.drawEllipse(cx, cy, 34 - t, 56 - t, TFT_WHITE);
    }

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setFont(&fonts::Font4);
    tft.drawCenterString("TOYOTA", cx, 180);

    tft.setFont(&fonts::Font2);
    tft.setTextColor(tft.color565(160, 160, 180), TFT_BLACK);
    tft.drawCenterString("CAN BUS LOGGER & ANALYZER", cx, 212);

    delay(2000);
}

// =========================================================================
// Screen Navigation Functions
// =========================================================================
void nextScreen() {
    currentScreen = static_cast<DisplayScreen>((currentScreen + 1) % SCREEN_COUNT);
    wakeScreen();
    Serial.printf("[SWIPE] Switched to Next Screen -> Page %d\n", currentScreen);
}

void prevScreen() {
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
        if (abs(deltaX) >= 20 && duration < 1000) {
            if (deltaX < -20) {
                nextScreen(); // Drag right-to-left -> Next Page
            } else if (deltaX > 20) {
                prevScreen(); // Drag left-to-right -> Prev Page
            }
        } 
        // 2. Button / Screen Tap Detection (duration < 500ms and minimal drag)
        else if (duration < 500 && abs(deltaX) < 20 && abs(deltaY) < 20) {
            // Check if on Page 2 (Logger Control Screen) for Button Taps
            if (currentScreen == SCREEN_LOGGER) {
                // Button 1: CAN Bus Logger (y: 32 - 88)
                if (touchLastY >= 32 && touchLastY <= 88) {
                    if (currentLogMode == LOG_CANBUS) {
                        stopActiveLogger();
                    } else if (currentLogMode == LOG_IDLE) {
                        startCanbusLogger();
                    }
                    return;
                }
                // Button 2: PID Datalogger (y: 94 - 150)
                else if (touchLastY >= 94 && touchLastY <= 150) {
                    if (currentLogMode == LOG_DATALOG) {
                        stopActiveLogger();
                    } else if (currentLogMode == LOG_IDLE) {
                        startDataLogger();
                    }
                    return;
                }
            }

            // Bottom Navigation Bar Tap
            if (touchLastY >= 210) {
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
    // Mode B: Log Decoded Vehicle PIDs at 10 Hz (every 100ms)
    if (currentLogMode == LOG_DATALOG && activeLogFile && (millis() - lastDatalogSampleTime >= 100)) {
        lastDatalogSampleTime = millis();
        logEntryCount++;

        activeLogFile.printf("%lu,%d,%d,%d,%d,%.2f,%.2f,%.3f,%.3f,%.1f,%.1f,%d\n",
            millis(),
            vehicleData.rpm,
            vehicleData.speedMph,
            vehicleData.throttlePct,
            vehicleData.engineLoadPct,
            vehicleData.commandedAfr,
            vehicleData.actualAfr,
            vehicleData.commandedAfr / 14.7f,
            vehicleData.actualAfr / 14.7f,
            vehicleData.kclv,
            vehicleData.knockFB,
            vehicleData.coolantTempC
        );

        if (logEntryCount % 20 == 0 || (millis() - lastLogFlushTime >= 1000)) {
            activeLogFile.flush();
            lastLogFlushTime = millis();
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== Waveshare ESP32-S3-Touch-LCD-2.8 V2 Tacoma CAN Logger ===");

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
