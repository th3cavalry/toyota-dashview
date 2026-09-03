# Toyota DashView 🚙📊

**Toyota DashView** is an open-source, in-cabin automotive digital gauge cluster, PID datalogger, raw CAN bus analyzer, and wireless SavvyCAN streamer for 3rd-gen Toyota Tacoma (2016–2023) and other Toyota CAN vehicles.

Originally developed to reverse-engineer and map the factory Toyota 500 kbps CAN bus for a **BMW M57 3.0L Twin-Turbo Diesel + ZF 6HP28 swap**, and used daily as a tuning companion on a supercharged 2GR-FKS build.

## Hardware (current build)

**Waveshare ESP32-S3-Touch-LCD-2.8 V2** (320×240 ST7789 IPS, CST3530 capacitive touch) + external **SN65HVD230** CAN transceiver on the 12-pin header.

> 🚚 Migrating to the **Waveshare ESP32-S3-Touch-LCD-4.3B** (800×480 RGB, *onboard* CAN + RS485 + RTC + 7–36V input) — see issue #1 for the port plan. The `waveshare-touch-28` PlatformIO env stays supported meanwhile.

## Features

* **6 swipeable 320×240 pages** (TRD dark-motorsport UI, 30 fps double-buffered):
  * **Cluster** — RPM bar with redline bands, gear + TCC lockup (`6L`), Commanded/Actual AFR + lambda, KCLV/KFB knock health, throttle %, engine load %.
  * **CAN Sniffer** — live msg/s, frame totals, raw-frame logger to SD, and a floating raw-packet terminal modal (pause / clear / inspect).
  * **PID Datalogger** — 10 Hz selected-PID CSV logging with an interactive PID picker (ALL/NONE presets).
  * **Wi-Fi Streaming** — SoftAP status + live stream counter.
  * **System** — firmware version, heap, PSRAM, SD and CAN interface health.
  * **Settings** — 180° display flip, 4-step backlight, reboot (persisted to NVS flash).
* **Toyota-aware decode**: passive `0x0B4` (speed), `0x3BC` (gear/TCC), `0x2C4` (RPM/throttle/load) + active OBD-II queries on `0x7E0`/`0x7E8` incl. Toyota Mode $21 `A2` (KCLV/KFB), with ISO 15765-2 multi-frame reassembly.
* **Wireless SavvyCAN streaming (GVRET)**: join the AP, connect SavvyCAN to `192.168.4.1:23` (one client at a time).
* **Pure MicroSD CSV logging**: `canbus_XXXX.csv` raw frames (ID/ext/DLC/hex payload) or `datalog_XXXX.csv` decoded PIDs — mutually exclusive sessions, unique filenames, periodic flush.
* **Boot splash** until first tap or engine start (RPM > 0), then straight to the cluster.
* **Auto-dim** backlight after 60 s idle; instant wake on touch or CAN traffic.

## Wiring (SN65HVD230 ↔ board ↔ truck)

| SN65HVD230 | ESP32-S3 (2.8 V2) | Signal |
| :--- | :--- | :--- |
| `TXD` | GPIO43 | TWAI TX |
| `RXD` | GPIO44 | TWAI RX |
| `VCC` | 3V3 | 3.3 V |
| `GND` | GND | Ground |

| SN65HVD230 | Tacoma CAN Junction Block | OBD-II | Signal |
| :--- | :--- | :--- | :--- |
| `CAN_H` | White wire | Pin 6 | HS-CAN High (500 kbps) |
| `CAN_L` | Black wire | Pin 14 | HS-CAN Low |
| `GND` | Chassis bolt | Pin 4/5 | Ground |

> For mid-bus taps, remove the 120 Ω termination jumper on the transceiver board.

## SavvyCAN over Wi-Fi

1. Join SSID **`Toyota-DashView`** (password on the device's Wi-Fi page).
2. SavvyCAN → *Connection* → *New Connection* → **Network (GVRET)** → `192.168.4.1`, port `23`.

⚠️ The AP uses a fixed default passphrase and streams live CAN frames — treat it as a bench/vehicle-service network, not internet-connected.

## Build & Flash (PlatformIO)

```bash
git clone https://github.com/th3cavalry/toyota-dashview.git
cd toyota-dashview
pio run -e waveshare-touch-28 --target upload
```

SD cards should be FAT32 (`./format_sd.sh /dev/sdX`).

## Disclaimer

Designed for automotive diagnostic research and custom powertrain swaps. Tap your own CAN bus at your own risk; verify every decode against a known-good reference before trusting it on the road.

## License

MIT — see [LICENSE](LICENSE).
