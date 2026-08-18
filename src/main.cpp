#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include "driver/twai.h"
#include <TinyGPSPlus.h>
#include <U8g2lib.h>
#include <XPowersLib.h>

// =========================================================================
// Pin Definitions for LILYGO T-Beam SUPREME ESP32-S3 (Header Pinout)
// =========================================================================

// Waveshare SN65HVD230 CAN Transceiver (Connected to Header IO2 & IO3)
#define CAN_TX_PIN         GPIO_NUM_2
#define CAN_RX_PIN         GPIO_NUM_3

// AXP2101 PMU Dedicated I2C1 Bus
#define PMU_I2C_SDA        42
#define PMU_I2C_SCL        41

// Primary Onboard I2C0 Bus (OLED Display, BME280, QMC6310)
#define BOARD_I2C_SDA      17
#define BOARD_I2C_SCL      18

// u-blox MAX-M10S GNSS Hardware Pins
#define GPS_RX_PIN         9   // ESP32-S3 RX <- GPS TX
#define GPS_TX_PIN         8   // ESP32-S3 TX -> GPS RX
#define GPS_EN_PIN         7   // GPS Power Enable (Active HIGH)
#define GPS_PPS_PIN        6   // GPS Pulse-Per-Second
#define GPS_BAUD           38400

// T-Beam SUPREME ESP32-S3 Dedicated SPI Bus
#define SDCARD_MOSI_PIN    35
#define SDCARD_MISO_PIN    37
#define SDCARD_SCK_PIN     36
#define SDCARD_CS_PIN      47

// LoRa SX1262 Radio Hardware Shutdown Pins
#define LORA_CS_PIN        10
#define LORA_RST_PIN       5
#define LORA_BUSY_PIN      4
#define LORA_DIO1_PIN      1
#define IMU_CS_PIN         34

// Onboard User / Boot Button (Short press = Page, Long press = Toggle Blue LED)
#define USER_BUTTON_PIN    0

// Screen Auto-Dim Timeout (60 Seconds)
#define SCREEN_TIMEOUT_MS          60000 
#define CAN_INACTIVITY_TIMEOUT_MS  10000 

// =========================================================================
// Toyota Emblem 72x42 1-Bit Monochrome Bitmap (Centered on 128x64)
// =========================================================================
static const unsigned char toyota_logo_72x42[] U8X8_PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xE0, 0xFF, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xF0, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x00,
    0x00, 0x00, 0xFC, 0x1F, 0x7E, 0xF8, 0x3F, 0x00, 0x00,
    0x00, 0x80, 0xFF, 0x00, 0xC3, 0x00, 0xFF, 0x01, 0x00,
    0x00, 0xC0, 0x0F, 0xC0, 0xFF, 0x03, 0xF0, 0x03, 0x00,
    0x00, 0xF0, 0x03, 0xFE, 0xFF, 0x7F, 0xC0, 0x0F, 0x00,
    0x00, 0xF8, 0xC0, 0xDF, 0x00, 0xFB, 0x03, 0x1F, 0x00,
    0x00, 0x3C, 0xF0, 0xC1, 0x00, 0x83, 0x0F, 0x3C, 0x00,
    0x00, 0x1F, 0x38, 0x60, 0x00, 0x06, 0x1C, 0xF8, 0x00,
    0x00, 0x07, 0x0C, 0x60, 0x00, 0x06, 0x30, 0xE0, 0x00,
    0x80, 0x03, 0x06, 0x60, 0x00, 0x06, 0x60, 0xC0, 0x01,
    0xC0, 0x01, 0x03, 0x60, 0x00, 0x06, 0xC0, 0x80, 0x03,
    0xE0, 0x01, 0x03, 0x30, 0x00, 0x0C, 0xC0, 0x80, 0x07,
    0xE0, 0x00, 0x03, 0x30, 0x00, 0x0C, 0xC0, 0x00, 0x07,
    0xE0, 0x00, 0x03, 0x30, 0x00, 0x0C, 0xC0, 0x00, 0x07,
    0x70, 0x00, 0x06, 0x30, 0x00, 0x0C, 0x60, 0x00, 0x0E,
    0x70, 0x00, 0x0C, 0x30, 0x00, 0x0C, 0x30, 0x00, 0x0E,
    0x70, 0x00, 0x38, 0x30, 0x00, 0x0C, 0x1C, 0x00, 0x0E,
    0x70, 0x00, 0xF0, 0x31, 0x00, 0x8C, 0x0F, 0x00, 0x0E,
    0x70, 0x00, 0xC0, 0x3F, 0x00, 0xFC, 0x03, 0x00, 0x0E,
    0x70, 0x00, 0x00, 0xFE, 0xFF, 0x7F, 0x00, 0x00, 0x0E,
    0xE0, 0x00, 0x00, 0xF0, 0xFF, 0x0F, 0x00, 0x00, 0x07,
    0xE0, 0x00, 0x00, 0x30, 0x00, 0x0C, 0x00, 0x00, 0x07,
    0xE0, 0x01, 0x00, 0x30, 0x00, 0x0C, 0x00, 0x80, 0x07,
    0xC0, 0x01, 0x00, 0x60, 0x00, 0x06, 0x00, 0x80, 0x03,
    0x80, 0x03, 0x00, 0x60, 0x00, 0x06, 0x00, 0xC0, 0x01,
    0x00, 0x07, 0x00, 0x60, 0x00, 0x06, 0x00, 0xE0, 0x00,
    0x00, 0x1F, 0x00, 0x60, 0x00, 0x06, 0x00, 0xF8, 0x00,
    0x00, 0x3C, 0x00, 0xC0, 0x00, 0x03, 0x00, 0x3C, 0x00,
    0x00, 0xF8, 0x00, 0xC0, 0x00, 0x03, 0x00, 0x1F, 0x00,
    0x00, 0xF0, 0x03, 0x80, 0x81, 0x01, 0xC0, 0x0F, 0x00,
    0x00, 0xC0, 0x0F, 0x80, 0x81, 0x01, 0xF0, 0x03, 0x00,
    0x00, 0x80, 0xFF, 0x00, 0xC3, 0x00, 0xFF, 0x01, 0x00,
    0x00, 0x00, 0xFC, 0x1F, 0x7E, 0xF8, 0x3F, 0x00, 0x00,
    0x00, 0x00, 0xF0, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xE0, 0xFF, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

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
// Hardware Instances
// =========================================================================
TwoWire PMUWire = TwoWire(1);
XPowersAXP2101 PMU;
TinyGPSPlus gps;
HardwareSerial GPSSerial(1);
SPIClass sdSPI(HSPI);

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

File logFile;
bool sdMounted = false;
bool isLoggingActive = false;
char currentLogFileName[36] = "None";
unsigned long packetCount = 0;
unsigned long tripPacketCount = 0;
unsigned long lastCanActivityTime = 0;
unsigned long lastSdRetryTime = 0;
unsigned long lastLogFlushTime = 0;
unsigned long lastPowerPrintTime = 0;
unsigned long lastGpsDebugTime = 0;
unsigned long lastObdQueryTime = 0;
unsigned long lastUserActivityTime = 0;
bool isScreenDimmed = false;
uint8_t obdQueryIndex = 0;
unsigned long ppsCount = 0;
float currentPPS = 0;
unsigned long lastPPSCheck = 0;
unsigned long lastDisplayUpdate = 0;
uint8_t oledI2CAddress = 0x3C;
bool displayFound = false;

// Blue LED Management (AXP2101 CHGLED)
enum BlueLedMode {
    LED_MODE_OFF = 0,
    LED_MODE_AUTO = 1,
    LED_MODE_BLINK = 2,
    LED_MODE_COUNT = 3
};
BlueLedMode currentLedMode = LED_MODE_OFF;
unsigned long ledBannerUntil = 0;
const char* ledBannerText = "";

// 5 Dedicated UI Screens
enum DisplayScreen {
    SCREEN_DASHBOARD = 0,
    SCREEN_SNIFFER   = 1,
    SCREEN_GPS       = 2,
    SCREEN_POWER     = 3,
    SCREEN_WIFI      = 4,
    SCREEN_COUNT     = 5
};
DisplayScreen currentScreen = SCREEN_DASHBOARD;
unsigned long buttonDownTime = 0;
bool buttonIsPressed = false;

// Live Vehicle State for Display View
struct TacomaTelemetry {
    char gear[4] = "P";         // P, R, N, 1, 2, 3, 4, 5, 6
    bool tccLocked = false;     // Torque Converter Lockup (TCC)
    int rpm = 0;
    float commandedAfr = 14.7f; // Target / Commanded AFR
    float actualAfr = 14.7f;    // Live Wideband A/F Sensor AFR
    float kclv = 20.0f;         // Knock Correct Learn Value
    float knockFB = 0.0f;       // Knock Feedback (deg)
    int throttlePct = 0;        // Throttle %
    int engineLoadPct = 0;      // Calculated Engine Load %
} vehicleData;

// Ring buffer for CAN Sniffer View
struct RecentFrame {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
    unsigned long timestamp;
};
#define SNIFFER_HISTORY_SIZE 4
RecentFrame snifferHistory[SNIFFER_HISTORY_SIZE];
int snifferHead = 0;

// Helper for battery charging state
const char* getChargePhaseString(int &approxCurrentMa) {
    xpowers_chg_status_t status = PMU.getChargerStatus();
    switch (status) {
        case XPOWERS_AXP2101_CHG_TRI_STATE:
            approxCurrentMa = 50;
            return "TRICKLE (50mA)";
        case XPOWERS_AXP2101_CHG_PRE_STATE:
            approxCurrentMa = 200;
            return "PRE-CHG (200mA)";
        case XPOWERS_AXP2101_CHG_CC_STATE:
            approxCurrentMa = 1000;
            return "FAST-CC (1.0A)";
        case XPOWERS_AXP2101_CHG_CV_STATE:
            approxCurrentMa = 400;
            return "TAPER-CV (4.2V)";
        case XPOWERS_AXP2101_CHG_DONE_STATE:
            approxCurrentMa = 0;
            return "DONE (100%)";
        case XPOWERS_AXP2101_CHG_STOP_STATE:
        default:
            approxCurrentMa = 0;
            return PMU.isVbusIn() ? "STANDBY" : "DISCHARGING";
    }
}

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
// LoRa SX1262 Hardware Shutdown
// =========================================================================
void shutdownLoRa() {
    pinMode(LORA_CS_PIN, OUTPUT);
    digitalWrite(LORA_CS_PIN, HIGH);
    
    pinMode(LORA_RST_PIN, OUTPUT);
    digitalWrite(LORA_RST_PIN, LOW);
    
    pinMode(LORA_BUSY_PIN, INPUT);
    pinMode(LORA_DIO1_PIN, INPUT);
    Serial.println("[LORA] SX1262 placed in permanent hardware reset. Safe to remove antenna.");
}

// =========================================================================
// Power Management & Battery Charger
// =========================================================================
void applyLedMode() {
    switch (currentLedMode) {
        case LED_MODE_OFF:
            PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);
            ledBannerText = "BLUE LED: OFF";
            break;
        case LED_MODE_AUTO:
            PMU.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
            ledBannerText = "BLUE LED: AUTO";
            break;
        case LED_MODE_BLINK:
            PMU.setChargingLedMode(XPOWERS_CHG_LED_BLINK_1HZ);
            ledBannerText = "BLUE LED: BLINK";
            break;
        default:
            break;
    }
    ledBannerUntil = millis() + 2000;
    Serial.printf("[PMU] %s\n", ledBannerText);
}

void initPMU() {
    PMUWire.begin(PMU_I2C_SDA, PMU_I2C_SCL);
    if (!PMU.begin(PMUWire, AXP2101_SLAVE_ADDRESS, PMU_I2C_SDA, PMU_I2C_SCL)) {
        Serial.println("[PMU] ERROR: AXP2101 not detected on I2C1!");
        return;
    }

    Serial.println("[PMU] AXP2101 detected. Enabling DC1 & Power rails...");

    PMU.setDC1Voltage(3300); PMU.enableDC1();
    PMU.setALDO1Voltage(3300); PMU.enableALDO1();
    PMU.setALDO2Voltage(3300); PMU.enableALDO2();
    PMU.setALDO3Voltage(3300); PMU.enableALDO3();
    PMU.setALDO4Voltage(3300); PMU.enableALDO4();
    PMU.setBLDO1Voltage(3300); PMU.enableBLDO1();
    PMU.setBLDO2Voltage(3300); PMU.enableBLDO2();
    PMU.setDLDO1Voltage(3300); PMU.enableDLDO1();
    PMU.setDLDO2Voltage(3300); PMU.enableDLDO2();

    PMU.enableBattDetection();
    PMU.enableVbusVoltageMeasure();
    PMU.enableBattVoltageMeasure();
    PMU.enableSystemVoltageMeasure();
    PMU.enableTemperatureMeasure();

    PMU.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_200MA);
    PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_1000MA);
    PMU.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    PMU.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
    PMU.setThermaThreshold(XPOWERS_AXP2101_THREMAL_120DEG);
    PMU.fuelGaugeControl(true, true);

    PMU.writeRegister(0x50, 0x00);
    PMU.writeRegister(0x58, 0x00);

    applyLedMode();
}

void initGPS() {
    pinMode(GPS_EN_PIN, OUTPUT);
    digitalWrite(GPS_EN_PIN, HIGH);
    delay(50);

    GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[GPS] MAX-M10S UART initialized at %d baud (RX: IO%d, TX: IO%d, EN: IO%d)\n",
                  GPS_BAUD, GPS_RX_PIN, GPS_TX_PIN, GPS_EN_PIN);
}

// =========================================================================
// OLED Initialization & Dimming Control + Clean Centered Logo Boot Splash
// =========================================================================
void wakeScreen() {
    lastUserActivityTime = millis();
    if (isScreenDimmed) {
        isScreenDimmed = false;
        u8g2.setContrast(255);
        Serial.println("[OLED] Screen Brightness Restored to 100%.");
    }
}

void dimScreen() {
    if (!isScreenDimmed) {
        isScreenDimmed = true;
        u8g2.setContrast(1);
        Serial.println("[OLED] Screen Dimmed to Low Brightness (1 min timeout).");
    }
}

void initOLED() {
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    delay(100);

    oledI2CAddress = 0x3C;
    displayFound = false;

    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            if (addr == 0x3C || addr == 0x3D) {
                oledI2CAddress = addr;
                displayFound = true;
            }
        }
    }

    u8g2.setI2CAddress(oledI2CAddress * 2);
    u8g2.begin();
    u8g2.setPowerSave(0);
    u8g2.setContrast(255);

    // Boot Splash: Clean, Centered Toyota 3-Oval Emblem
    u8g2.clearBuffer();
    u8g2.drawXBMP(28, 11, 72, 42, toyota_logo_72x42);
    u8g2.sendBuffer();
    delay(2000);

    lastUserActivityTime = millis();
}

// =========================================================================
// CAN Frame Decoding & Live Querying
// =========================================================================
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
        queryMsg.data[2] = 0xA2;
    } else if (obdQueryIndex == 1) {
        queryMsg.data[0] = 0x02;
        queryMsg.data[1] = 0x01;
        queryMsg.data[2] = 0x24;
    } else {
        queryMsg.data[0] = 0x02;
        queryMsg.data[1] = 0x01;
        queryMsg.data[2] = 0x44;
    }

    twai_transmit(&queryMsg, 0);
    obdQueryIndex = (obdQueryIndex + 1) % 3;
}

void decodeTacomaFrame(const twai_message_t &msg) {
    if (msg.identifier == 0x3BC && msg.data_length_code >= 5) {
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
        Serial.println("[CAN] TWAI started.");
    } else {
        Serial.println("[CAN] Failed to start TWAI.");
    }
}

bool mountSD() {
    pinMode(LORA_CS_PIN, OUTPUT);
    digitalWrite(LORA_CS_PIN, HIGH);

    pinMode(IMU_CS_PIN, OUTPUT);
    digitalWrite(IMU_CS_PIN, HIGH);

    pinMode(SDCARD_CS_PIN, OUTPUT);
    digitalWrite(SDCARD_CS_PIN, HIGH);

    sdSPI.begin(SDCARD_SCK_PIN, SDCARD_MISO_PIN, SDCARD_MOSI_PIN, SDCARD_CS_PIN);
    
    if (SD.begin(SDCARD_CS_PIN, sdSPI, 20000000)) {
        sdMounted = true;
        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Serial.printf("[SD] MicroSD Mounted! Card size: %llu MB\n", cardSize);
        return true;
    }

    if (SD.begin(SDCARD_CS_PIN, sdSPI, 10000000)) {
        sdMounted = true;
        Serial.println("[SD] MicroSD Mounted (10MHz).");
        return true;
    }

    sdMounted = false;
    return false;
}

void startTripLogging() {
    if (!sdMounted && !mountSD()) {
        return;
    }

    if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024) {
        snprintf(currentLogFileName, sizeof(currentLogFileName), "/tac_%04d%02d%02d_%02d%02d%02d.csv",
                 gps.date.year(), gps.date.month(), gps.date.day(),
                 gps.time.hour(), gps.time.minute(), gps.time.second());
    } else {
        snprintf(currentLogFileName, sizeof(currentLogFileName), "/tacoma_trip_%lu.csv", millis() / 1000);
    }

    logFile = SD.open(currentLogFileName, FILE_WRITE);
    if (logFile) {
        logFile.println("Timestamp_ms,GPS_Time,Latitude,Longitude,Speed_kmh,Altitude_m,CAN_ID,Ext,DLC,Data");
        logFile.flush();
        isLoggingActive = true;
        tripPacketCount = 0;
        lastLogFlushTime = millis();
        Serial.printf("[SD] >>> Auto-Trip Started: %s\n", currentLogFileName);
    } else {
        isLoggingActive = false;
    }
}

void stopTripLogging() {
    if (isLoggingActive && logFile) {
        logFile.flush();
        logFile.close();
        isLoggingActive = false;
        Serial.printf("[SD] <<< Auto-Trip Finalized: %s (Packets: %lu)\n", currentLogFileName, tripPacketCount);
    }
}

// =========================================================================
// UI Screen Renderers (5 Distinct Pages)
// =========================================================================

// Screen 0: Live Vehicle Dashboard
void renderDashboard() {
    char buf[32];

    // Top RPM Bar (0 - 6000 RPM)
    u8g2.drawFrame(0, 0, 128, 6);
    int rpmWidth = map(constrain(vehicleData.rpm, 0, 6000), 0, 6000, 0, 124);
    if (rpmWidth > 0) {
        u8g2.drawBox(2, 2, rpmWidth, 2);
    }

    // Large Current Gear with Lockup 'L' indicator
    u8g2.setFont(u8g2_font_logisoso22_tr);
    if (vehicleData.tccLocked && vehicleData.gear[0] >= '1' && vehicleData.gear[0] <= '6') {
        snprintf(buf, sizeof(buf), "%sL", vehicleData.gear);
    } else {
        snprintf(buf, sizeof(buf), "%s", vehicleData.gear);
    }
    u8g2.drawStr(2, 30, buf);

    // Commanded & Actual AFR / Lambda
    u8g2.setFont(u8g2_font_6x10_tr);
    snprintf(buf, sizeof(buf), "Cmd:%.1f (%.2f)", vehicleData.commandedAfr, vehicleData.commandedAfr / 14.7f);
    u8g2.drawStr(46, 18, buf);

    snprintf(buf, sizeof(buf), "Act:%.1f (%.2f)", vehicleData.actualAfr, vehicleData.actualAfr / 14.7f);
    u8g2.drawStr(46, 30, buf);

    // Line 3: KCLV & KFB
    snprintf(buf, sizeof(buf), "KCLV: %.1f", vehicleData.kclv);
    u8g2.drawStr(0, 43, buf);

    snprintf(buf, sizeof(buf), "KFB: %+2.1f\xb0", vehicleData.knockFB);
    u8g2.drawStr(66, 43, buf);

    // Line 4: Throttle % & Calculated Engine Load %
    snprintf(buf, sizeof(buf), "Thr: %d%%", vehicleData.throttlePct);
    u8g2.drawStr(0, 53, buf);

    snprintf(buf, sizeof(buf), "Load: %d%%", vehicleData.engineLoadPct);
    u8g2.drawStr(66, 53, buf);

    // Divider Line
    u8g2.drawHLine(0, 55, 128);

    // 3-Column Footer
    u8g2.setFont(u8g2_font_5x8_tr);

    const char* logStatus = "NO SD";
    if (isLoggingActive) {
        logStatus = ((millis() / 500) % 2 == 0) ? "[REC]" : " REC ";
    } else if (sdMounted) {
        logStatus = "STBY";
    }
    u8g2.drawStr(0, 63, logStatus);

    snprintf(buf, sizeof(buf), "%.0f msg/s", currentPPS);
    u8g2.drawStr(44, 63, buf);

    int battPct = PMU.getBatteryPercent();
    bool chg = PMU.isCharging();
    snprintf(buf, sizeof(buf), "%s%d%%", chg ? "+" : "", battPct >= 0 ? battPct : 0);
    u8g2.drawStr(98, 63, buf);
}

// Screen 1: CAN Sniffer View
void renderSniffer() {
    char buf[32];
    u8g2.setFont(u8g2_font_6x10_tr);

    snprintf(buf, sizeof(buf), "CAN: %.0f msg/s  (%lu)", currentPPS, packetCount);
    u8g2.drawStr(0, 8, buf);
    u8g2.drawHLine(0, 11, 128);

    for (int i = 0; i < SNIFFER_HISTORY_SIZE; i++) {
        int idx = (snifferHead - 1 - i + SNIFFER_HISTORY_SIZE) % SNIFFER_HISTORY_SIZE;
        if (snifferHistory[idx].id == 0 && snifferHistory[idx].dlc == 0) continue;

        snprintf(buf, sizeof(buf), "0x%03X:%02X%02X%02X%02X%02X",
                 snifferHistory[idx].id,
                 snifferHistory[idx].data[0],
                 snifferHistory[idx].data[1],
                 snifferHistory[idx].data[2],
                 snifferHistory[idx].data[3],
                 snifferHistory[idx].data[4]);
        u8g2.drawStr(0, 24 + (i * 13), buf);
    }
}

// Screen 2: GPS GNSS & Compass
void renderGPS() {
    char buf[32];
    u8g2.setFont(u8g2_font_6x10_tr);

    const char* cardinals[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int headingIdx = ((int)(gps.course.deg() + 22.5) % 360) / 45;
    const char* headingCard = gps.course.isValid() ? cardinals[headingIdx] : "--";

    snprintf(buf, sizeof(buf), "HDG: %03.0f\xb0 (%s)", gps.course.isValid() ? gps.course.deg() : 0.0, headingCard);
    u8g2.drawStr(0, 9, buf);

    snprintf(buf, sizeof(buf), "Sats: %d | Alt: %.0fft", gps.satellites.value(), gps.altitude.feet());
    u8g2.drawStr(0, 22, buf);

    snprintf(buf, sizeof(buf), "GPS Spd: %.1f MPH", gps.speed.mph());
    u8g2.drawStr(0, 35, buf);

    if (gps.location.isValid()) {
        snprintf(buf, sizeof(buf), "Lat: %.5f", gps.location.lat());
        u8g2.drawStr(0, 48, buf);
        snprintf(buf, sizeof(buf), "Lon: %.5f", gps.location.lng());
        u8g2.drawStr(0, 61, buf);
    } else {
        snprintf(buf, sizeof(buf), "Fix: %lu chars", gps.charsProcessed());
        u8g2.drawStr(0, 48, buf);
        u8g2.drawStr(0, 61, "Searching sky...");
    }
}

// Screen 3: Dedicated Power & Charger Screen
void renderPower() {
    char buf[32];
    u8g2.setFont(u8g2_font_6x10_tr);

    u8g2.drawStr(0, 8, "[ POWER & BATTERY ]");
    u8g2.drawHLine(0, 10, 128);

    int battPct = PMU.getBatteryPercent();
    float battVolt = PMU.getBattVoltage() / 1000.0f;
    bool isChg = PMU.isCharging();
    int currentMa = 0;
    const char* phase = getChargePhaseString(currentMa);
    float powerWatts = battVolt * (currentMa / 1000.0f);

    snprintf(buf, sizeof(buf), "Batt: %.3fV (%d%%)", battVolt, battPct >= 0 ? battPct : 0);
    u8g2.drawStr(0, 22, buf);

    snprintf(buf, sizeof(buf), "Mode: %s", phase);
    u8g2.drawStr(0, 34, buf);

    if (isChg && currentMa > 0) {
        snprintf(buf, sizeof(buf), "Flow: +%dmA | %.2fW", currentMa, powerWatts);
    } else {
        snprintf(buf, sizeof(buf), "Flow: 0 mA (Idle)");
    }
    u8g2.drawStr(0, 46, buf);

    float vbusVolt = PMU.getVbusVoltage() / 1000.0f;
    const char* ledStr = (currentLedMode == LED_MODE_OFF) ? "OFF" : ((currentLedMode == LED_MODE_AUTO) ? "AUTO" : "BLINK");
    snprintf(buf, sizeof(buf), "USB:%.2fV  LED:%s", vbusVolt, ledStr);
    u8g2.drawStr(0, 58, buf);

    u8g2.drawHLine(0, 60, 128);
}

// Screen 4: Dedicated Wi-Fi & SavvyCAN Streaming Screen
void renderWiFi() {
    char buf[32];
    u8g2.setFont(u8g2_font_6x10_tr);

    u8g2.drawStr(0, 8, "[ WI-FI STREAMING ]");
    u8g2.drawHLine(0, 10, 128);

    snprintf(buf, sizeof(buf), "SSID: %s", WIFI_SSID);
    u8g2.drawStr(0, 22, buf);

    snprintf(buf, sizeof(buf), "Pass: %s", WIFI_PASS);
    u8g2.drawStr(0, 33, buf);

    snprintf(buf, sizeof(buf), "IP: 192.168.4.1:%d", SAVVYCAN_PORT);
    u8g2.drawStr(0, 44, buf);

    if (savvyClient && savvyClient.connected()) {
        snprintf(buf, sizeof(buf), "Status: LIVE (%lu)", wifiStreamedCount);
    } else {
        snprintf(buf, sizeof(buf), "Status: Waiting...");
    }
    u8g2.drawStr(0, 56, buf);

    snprintf(buf, sizeof(buf), "SD Log: %s", isLoggingActive ? "RECORDING" : (sdMounted ? "STANDBY" : "NO SD"));
    u8g2.drawStr(0, 64, buf);
}

void updateDisplay() {
    u8g2.clearBuffer();

    switch (currentScreen) {
        case SCREEN_DASHBOARD: renderDashboard(); break;
        case SCREEN_SNIFFER:   renderSniffer();   break;
        case SCREEN_GPS:       renderGPS();       break;
        case SCREEN_POWER:     renderPower();     break;
        case SCREEN_WIFI:      renderWiFi();      break;
        default:               renderDashboard(); break;
    }

    if (millis() < ledBannerUntil) {
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.drawBox(10, 20, 108, 22);
        u8g2.setDrawColor(0);
        u8g2.drawStr(16, 35, ledBannerText);
        u8g2.setDrawColor(1);
    }

    u8g2.sendBuffer();
}

// =========================================================================
// Main Loop & Handlers
// =========================================================================
void handleButton() {
    int btnState = digitalRead(USER_BUTTON_PIN);

    if (btnState == LOW && !buttonIsPressed) {
        buttonIsPressed = true;
        buttonDownTime = millis();
        wakeScreen();
    } else if (btnState == HIGH && buttonIsPressed) {
        buttonIsPressed = false;
        unsigned long duration = millis() - buttonDownTime;

        if (duration >= 1000) {
            currentLedMode = static_cast<BlueLedMode>((currentLedMode + 1) % LED_MODE_COUNT);
            applyLedMode();
            wakeScreen();
        } else if (duration >= 50) {
            currentScreen = static_cast<DisplayScreen>((currentScreen + 1) % SCREEN_COUNT);
            wakeScreen();
            Serial.printf("[UI] Switched to Screen Page %d\n", currentScreen);
        }
    }
}

void processCAN() {
    twai_message_t message;
    while (twai_receive(&message, 0) == ESP_OK) {
        packetCount++;
        tripPacketCount++;
        ppsCount++;
        lastCanActivityTime = millis();
        wakeScreen();

        if (!isLoggingActive) {
            startTripLogging();
        }

        streamFrameToSavvyCAN(message);
        decodeTacomaFrame(message);
        recordSnifferFrame(message);

        if (isLoggingActive && logFile) {
            logFile.printf("%lu,", millis());

            if (gps.time.isValid()) {
                logFile.printf("%02d:%02d:%02d.%02d,", gps.time.hour(), gps.time.minute(), gps.time.second(), gps.time.centisecond());
            } else {
                logFile.print("NO_FIX,");
            }

            if (gps.location.isValid()) {
                logFile.printf("%.6f,%.6f,", gps.location.lat(), gps.location.lng());
            } else {
                logFile.print("0.0,0.0,");
            }

            logFile.printf("%.2f,%.1f,", gps.speed.kmph(), gps.altitude.meters());

            logFile.printf("0x%03X,%d,%d,", message.identifier, message.extd, message.data_length_code);
            for (int i = 0; i < message.data_length_code; i++) {
                logFile.printf("%02X", message.data[i]);
                if (i < message.data_length_code - 1) logFile.print(" ");
            }
            logFile.println();

            if (tripPacketCount % 50 == 0 || (millis() - lastLogFlushTime >= 1000)) {
                logFile.flush();
                lastLogFlushTime = millis();
            }
        }
    }

    if (isLoggingActive && (millis() - lastCanActivityTime > CAN_INACTIVITY_TIMEOUT_MS)) {
        stopTripLogging();
    }
}

void processGPS() {
    while (GPSSerial.available() > 0) {
        char c = GPSSerial.read();
        gps.encode(c);
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
    delay(200);
    Serial.println("\n=== 2016 Toyota Tacoma CAN Logger (Pure Toyota Emblem Boot) ===");

    shutdownLoRa();
    initPMU();
    initWiFiStreaming();
    initGPS();
    initOLED();
    initCAN();
    mountSD();

    delay(200);
}

void loop() {
    handleButton();
    handleWiFiClients();
    processCAN();
    processGPS();

    if (!isScreenDimmed && (millis() - lastUserActivityTime >= SCREEN_TIMEOUT_MS) && (millis() - lastCanActivityTime >= SCREEN_TIMEOUT_MS)) {
        dimScreen();
    }

    if (millis() - lastCanActivityTime < 3000 && (millis() - lastObdQueryTime >= 300)) {
        lastObdQueryTime = millis();
        sendToyotaObdQueries();
    }

    if (!sdMounted && (millis() - lastSdRetryTime >= 5000)) {
        lastSdRetryTime = millis();
        mountSD();
    }

    if (millis() - lastGpsDebugTime >= 3000) {
        lastGpsDebugTime = millis();
        Serial.printf("[GPS] Chars: %lu | Fix: %s | Sats: %d | Lat: %.5f | Lon: %.5f\n",
                      gps.charsProcessed(),
                      gps.location.isValid() ? "YES" : "NO",
                      gps.satellites.value(),
                      gps.location.lat(),
                      gps.location.lng());
    }

    if (millis() - lastPowerPrintTime >= 2000) {
        lastPowerPrintTime = millis();
        int currentMa = 0;
        const char* phase = getChargePhaseString(currentMa);
        uint16_t battMv = PMU.getBattVoltage();
        uint16_t vbusMv = PMU.getVbusVoltage();
        float watts = (battMv / 1000.0f) * (currentMa / 1000.0f);
        Serial.printf("[POWER] Batt: %u mV | VBUS: %u mV | Phase: %s | Current: +%d mA | Power: %.3f W | WiFi: %s\n",
                      battMv, vbusMv, phase, currentMa, watts, wifiClientConnected ? "CLIENT CONNECTED" : "LISTENING");
    }

    if (millis() - lastPPSCheck >= 1000) {
        currentPPS = (float)ppsCount * 1000.0f / (millis() - lastPPSCheck);
        ppsCount = 0;
        lastPPSCheck = millis();
    }

    if (millis() - lastDisplayUpdate >= 150) {
        lastDisplayUpdate = millis();
        updateDisplay();
    }
}
