# Toyota Tacoma CAN Logger & SavvyCAN Wi-Fi Streamer 🚙📡

An automotive CAN bus logger, live dashboard, and wireless SavvyCAN streamer designed for 3rd Gen Toyota Tacomas (2016–2023). Built on the **LILYGO T-Beam SUPREME ESP32-S3** with a **Waveshare SN65HVD230 3.3V CAN Transceiver**.

Originally developed to reverse-engineer and map the factory Toyota 500 kbps CAN bus system for a **BMW M57 3.0L Twin-Turbo Diesel + ZF 6HP28 Transmission swap** using an **ESP32-CAN-X2** dual-channel translator gateway.

---

## Features

* **5 Dedicated OLED Pages (1.3" SH1106 Display)**:
  * **Screen 0 (Dashboard)**: Real-time RPM bar, Large Current Gear with Torque Converter Lockup (`6L`), Commanded & Actual AFR/Lambda, Toyota Learned & Live Knock (`KCLV` / `KFB`), Throttle %, Calculated Engine Load %, and 3-column status footer.
  * **Screen 1 (CAN Sniffer)**: Live message rate (`msg/s`), total packets, and rolling raw hex packet buffer.
  * **Screen 2 (GPS GNSS & Compass)**: Latitude/Longitude, Heading & Cardinal direction, Satellites in view, Altitude, and GPS Speed in MPH.
  * **Screen 3 (Power & Charger)**: Battery voltage, 18650 charge state (`TRICKLE`, `PRE-CHG`, `FAST-CC 1.0A`, `TAPER-CV`, `DONE`), mA current flow, power in Watts, and USB VBUS voltage.
  * **Screen 4 (Wi-Fi Streaming)**: SoftAP status, IP address, connected clients, and live stream packet count.
* **Wireless SavvyCAN Streaming (GVRET over Wi-Fi)**:
  * Broadcasts a dedicated Wi-Fi Access Point (`Tacoma-CAN-Logger`).
  * Runs a TCP GVRET streaming server on Port `23` for real-time, cable-free packet sniffing in **SavvyCAN**.
* **Pure MicroSD CSV Logging**:
  * Automatically creates geostamped CSV trip logs (`/tac_YYYYMMDD_HHMMSS.csv`) upon detecting CAN traffic.
  * Captures timestamp, GPS time, Lat/Lon, GPS speed, altitude, raw standard/extended CAN IDs, DLC, and payload bytes.
* **OEM Toyota Boot Splash**:
  * Centered 72x42 monochrome 1-bit Toyota 3-Oval emblem displayed on boot.
* **Auto-Dimming Power Management**:
  * Dims display brightness down after 60 seconds of inactivity without turning off, instantly waking upon button press or CAN bus activity.
* **LoRa Hardware Shutdown**:
  * SX1262 LoRa radio is held in permanent hardware reset/sleep, allowing safe removal of the bulky 915MHz antenna.
* **18650 Battery Management (AXP2101 PMIC)**:
  * JEITA and TS thermal throttling disabled for unmonitored standard 18650 cells.
  * Blue charging LED control (Long press `BOOT` button to toggle `OFF` $\rightarrow$ `AUTO` $\rightarrow$ `BLINK`).

---

## Hardware Pinout & Wiring

### 1. LilyGO T-Beam Supreme $\longleftrightarrow$ Waveshare SN65HVD230

| LilyGO Header Pin (Label) | Waveshare CAN Board Pin | Description |
| :--- | :--- | :--- |
| **`dc1`** *(or `bldd2`)* | **`3V3` / `VCC`** | 3.3V Regulated Power from PMU |
| **`gnd`** | **`GND`** | Ground |
| **`io2`** | **`CTX` / `TXD`** | ESP32-S3 TWAI CAN Transmit (TX) |
| **`io3`** | **`CRX` / `RXD`** | ESP32-S3 TWAI CAN Receive (RX) |

### 2. Waveshare SN65HVD230 $\longleftrightarrow$ 2016 Tacoma CAN Bus

Connect either to the **CAN Junction Block behind the steering wheel** or to the **OBD-II Port**:

| Waveshare Screw Terminal | Tacoma CAN Junction Block | OBD-II Port | Signal |
| :--- | :--- | :--- | :--- |
| **`CAN_H`** | **White** wire | **Pin 6** | HS-CAN High (500 kbps) |
| **`CAN_L`** | **Black** wire | **Pin 14** | HS-CAN Low (500 kbps) |
| **`GND`** | Chassis Ground Bolt | **Pin 4 or 5** | Ground |

> **Note**: For mid-bus junction block taps, remove the `120R` / `R_EN` termination jumper on the Waveshare board so it acts as a high-impedance listening tap.

---

## Wi-Fi & SavvyCAN Configuration

1. Connect your laptop to the Wi-Fi network:
   * **SSID**: `Tacoma-CAN-Logger`
   * **Password**: `tacoma123`
2. Open **SavvyCAN** (`/usr/bin/SavvyCAN`).
3. Navigate to **`Connection` $\longrightarrow$ `Open Connection Window` $\longrightarrow$ `Add New Device Connection`**.
4. Select **`Network Connection`** (or `TCP/IP` / `GVRET-WiFi`):
   * **IP Address**: `192.168.4.1`
   * **Port**: `23`
5. Click **Connect**. Live CAN packets will stream directly into SavvyCAN.

---

## Build & Flash (PlatformIO)

```bash
# Clone the repository
git clone git@github.com:th3cavalry/tacoma-can-logger.git
cd tacoma-can-logger

# Build and upload firmware
pio run --target upload
```

---

## License

MIT License. Designed for automotive diagnostic research and custom powertrain engine swaps.
