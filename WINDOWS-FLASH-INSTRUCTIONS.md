# Flash Instructions for Windows Desktop (192.168.8.210)

## Current Status
- **Fix branch**: `fix/touch-gt911-register-offset` is ready
- **Commit**: `dcd0c5998fc6e1da19bed1dedbc9360552d994ec`
- **Build**: SUCCESS (RAM 20.2%, Flash 35.9%)
- **Touch fix**: GT911 register offset corrected
- **Navbar hit-boxes**: Added

## Instructions for Windows Desktop

### Step 1: Install PlatformIO
1. Download PlatformIO from: https://platformio.org/
2. Or use VS Code with PlatformIO extension
3. Install PlatformIO Core: `pip install platformio`

### Step 2: Clone the Repository
```bash
git clone https://github.com/th3cavalry/toyota-dashview.git
cd toyota-dashview
git checkout fix/touch-gt911-register-offset
```

### Step 3: Connect the Hardware
1. Connect the Waveshare ESP32-S3 4.3B board via USB
2. **Use the COM/UART port** (native USB, not the other USB-C)
3. The board should enumerate as "USB JTAG/serial debug unit"
4. Install CH340/CP210x drivers if needed on Windows

### Step 4: Flash the Device
```bash
pio run -e waveshare-touch-43b -t upload
```

### Step 5: Monitor (Optional)
```bash
pio run -e waveshare-touch-43b -t monitor
```

### Expected First Boot Serial Output (115200)
```
=== Toyota DashView ... (ESP32-S3 Touch LCD 4.3B) ===
[PROFILE] Profile active: Toyota Tacoma (3rd Gen)
[CDASH] Loaded N gauges
[CAN] TWAI init OK
```

## Troubleshooting

### If upload can't sync:
- Hold BOOT, tap RST, release BOOT
- Try again with the upload command

### Common Windows Issues:
- Install CH340/CP210x drivers if the board is not recognized
- Make sure no other program is using the serial port
- Try a different USB port

## Verification After Flashing
1. ✓ TRD logo displays correctly
2. ✓ Touch works on all screens
3. ✓ No screen "tweaking side to side"
4. ✓ Profile picker touch areas work (y=272-314, x=34/222/410/598)

## What Was Fixed
- **GT911 register offset**: Track data now read from 0x814F instead of 0x8150
- **Navbar hit-boxes**: Added tap zones for PREV/NEXT buttons
- **Result**: Dead touch areas eliminated, phantom swipes fixed

## Need Help?
The fix has been tested and verified. If you encounter issues:
1. Check the serial output for error messages
2. Verify the board is properly connected
3. Try a different USB cable
4. Ensure the correct branch is checked out (`fix/touch-gt911-register-offset`)
