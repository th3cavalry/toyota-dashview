# Toyota DashView

In-cabin digital gauge cluster and CAN datalogger for the third-generation Toyota Tacoma (2016+). Reads the factory OBD-II/CAN bus and displays live engine and transmission data, logs PIDs to a microSD card with real-time-clock timestamps, and streams raw CAN traffic over Wi-Fi to SavvyCAN.

## Supported hardware

**Waveshare ESP32-S3-Touch-LCD-4.3B** — the only supported board.

| Component | Specification |
|---|---|
| Display | 4.3 in 800

...
 |
| Touch | GT911 capacitive (I2C) |
| CAN | Onboard transceiver, screw terminals, selectable 120R termination |
| RTC | PCF85063 with supercap backup |
| Storage | microSD (SPI) |
| Power | 7–36 V DC input |
| Wireless | Wi-Fi 2.4 GHz + BLE (ESP32-S3) |

## Wiring

Connect the DashView screw terminals to the Tacoma OBD-II port (driver's side, below the steering wheel):

| DashView terminal | OBD-II pin |
|---|---|
| CAN-H | Pin 6 |
| CAN-L | Pin 14 |
| GND | Pin 4 or 5 (chassis ground) |
| V+ | Ignition-switched +12 V, inline 5 A fuse |

> Route power from a fused ignition-switched circuit so the display sleeps with the engine. Do not connect V+ directly to the battery.

Set the onboard CAN termination switch to **ON** (the Tacoma bus has one termination already; leave **OFF** if you experience bus errors on a daisy-chained harness).

## Features

- Multi-page gauge cluster: RPM, MPH, throttle position, commanded/actual AFR, knock retard, engine coolant temp, gear, and torque-converter lockup
- OBD-II Mode 01/21 queries with ISO 15765-2 multi-frame response reassembly (Toyota `21 A2` knock/learned-value support)
- PID datalogging to microSD with PCF85063 RTC timestamps (FAT32, CSV)
- Raw CAN sniffer page with live traffic terminal
- SavvyCAN-compatible TCP streaming over Wi-Fi (port 23)
- Auto-dim after 60 s of inactivity; tap or CAN traffic to wake
- 180° display flip for inverted mounts (persistent NVS); backlight auto-dim (digital on/off)

## Build & flash

Requires [PlatformIO](https://platformio.org):

```bash
git clone https://github.com/th3cavalry/toyota-dashview.git
cd toyota-dashview
pio run                 # builds the waveshare-touch-43b environment
pio run -t upload       # flash over USB-C
```

## SD card prep (datalogging)

```bash
sudo ./format_sd.sh /dev/sdX    # DESTROYS all data on that device
```

Formats FAT32 and creates the `LOGS` directory the firmware writes to.

## License

MIT — see [LICENSE](LICENSE).

## Disclaimer

This device displays data read from your vehicle's diagnostic bus. It is not a replacement for factory instrumentation. Use at your own risk; driver distraction can cause serious injury or death. The author accepts no liability for vehicle damage, data loss, or accidents arising from use of this software or hardware.