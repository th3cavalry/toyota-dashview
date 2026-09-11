#!/usr/bin/env python3
import sys
import os
import time
import subprocess
import serial

PORT = os.environ.get('DASHVIEW_PORT', '/dev/ttyACM0')
BAUD = 115200

def main():
    screen = None
    mock = False
    output_png = "screenshot.png"

    args = sys.argv[1:]
    idx = 0
    while idx < len(args):
        arg = args[idx]
        if arg in ("--mock", "-m"):
            mock = True
        elif arg in ("--screen", "-s") and idx + 1 < len(args):
            idx += 1
            screen = args[idx]
        elif arg.isdigit():
            screen = arg
        elif not arg.startswith("-"):
            output_png = arg
        idx += 1

    print(f"Connecting to {PORT}...")
    ser = serial.Serial(PORT, BAUD, timeout=3)
    time.sleep(0.3)

    if screen is not None:
        print(f"Switching to screen {screen}...")
        ser.write(f"{screen}\n".encode())
        time.sleep(0.3)

    if mock:
        print("Injecting mock telemetry...")
        ser.write(b"m\n")
        time.sleep(0.3)

    ser.reset_input_buffer()

    print("Requesting screenshot from device...")
    ser.write(b"c\n")

    start_time = time.time()
    marker = b"---SCREENSHOT:START:800:480:RGB565---"
    found = False
    while time.time() - start_time < 5.0:
        line = ser.readline()
        if marker in line:
            found = True
            break

    if not found:
        print("Error: Did not receive screenshot start marker!")
        ser.close()
        sys.exit(1)

    print("Receiving 768,000 bytes of framebuffer data...")
    total_bytes = 800 * 480 * 2
    raw_data = bytearray()
    while len(raw_data) < total_bytes and (time.time() - start_time < 12.0):
        chunk = ser.read(min(total_bytes - len(raw_data), 16384))
        if chunk:
            raw_data.extend(chunk)

    ser.close()

    if len(raw_data) != total_bytes:
        print(f"Error: Received {len(raw_data)} bytes, expected {total_bytes} bytes!")
        sys.exit(1)

    print(f"Received {len(raw_data)} bytes successfully. Converting to PNG: {output_png}...")
    raw_path = "/tmp/screenshot_raw.bin"
    with open(raw_path, "wb") as f:
        f.write(raw_data)

    cmd = [
        "ffmpeg", "-y", "-v", "error",
        "-f", "rawvideo",
        "-pixel_format", "rgb565le",
        "-video_size", "800x480",
        "-i", raw_path,
        output_png
    ]
    subprocess.run(cmd, check=True)
    if os.path.exists(raw_path):
        os.remove(raw_path)
    print(f"Screenshot successfully saved to {output_png}")

if __name__ == "__main__":
    main()
