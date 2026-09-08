"""
ESP32-S3 Upload Helper with Watchdog Reset
==========================================

Problem: On ESP32-S3 boards using USB-Serial-JTAG (like T-Dongle S3), esptool's
default "hard reset via RTS" produces a USB_UART_CHIP_RESET, which ALWAYS causes
the ROM bootloader to re-enter download mode. The application never runs.

Solution: esptool v5.1+ supports --after watchdog-reset, which triggers an RTC
watchdog reset instead. This produces a different reset reason (RTC_WDT_SYS_RESET)
that makes the ROM bootloader boot the application normally.

Usage:
    python upload_watchdog.py                    # Auto-detect port
    python upload_watchdog.py --port COMx        # Specific port
    python upload_watchdog.py --env serial-test  # Specific PIO env

Requirements:
    pip install esptool>=5.1
"""
import subprocess
import sys
import os
import argparse


def find_firmware(env_name):
    """Find firmware files for the given PIO environment."""
    build_dir = os.path.join(".pio", "build", env_name)
    files = {
        "bootloader": os.path.join(build_dir, "bootloader.bin"),
        "partitions": os.path.join(build_dir, "partitions.bin"),
        "firmware": os.path.join(build_dir, "firmware.bin"),
    }
    for name, path in files.items():
        if not os.path.exists(path):
            print(f"ERROR: {name} not found at {path}")
            print(f"Run: pio run -e {env_name}")
            sys.exit(1)
    return files


def detect_port():
    """Best-effort serial port detection for ESP32-S3 boards."""
    try:
        from serial.tools import list_ports
    except ImportError:
        print("ERROR: pyserial is required for auto-detect. Install pyserial or pass --port.")
        sys.exit(1)

    ports = list(list_ports.comports())
    if not ports:
        print("ERROR: No serial ports found. Connect the board or pass --port explicitly.")
        sys.exit(1)

    if len(ports) == 1:
        return ports[0].device

    preferred = [
        p.device for p in ports
        if any(marker in (p.description or "").lower()
               for marker in ("usb", "uart", "jtag", "cp210", "ch340", "serial"))
    ]
    if len(preferred) == 1:
        return preferred[0]

    print("ERROR: Multiple serial ports detected. Pass --port explicitly.")
    for port in ports:
        desc = port.description or "Unknown device"
        print(f"  - {port.device}: {desc}")
    sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="Upload to ESP32-S3 with watchdog reset")
    parser.add_argument("--port", help="Serial port, e.g. COMx or /dev/ttyACMx")
    parser.add_argument("--env", default="serial-test", help="PIO environment name")
    parser.add_argument("--baud", default="921600", help="Upload baud rate")
    args = parser.parse_args()

    files = find_firmware(args.env)
    port = args.port or detect_port()

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", port,
        "--baud", args.baud,
        "--after", "watchdog-reset",
        "write-flash",
        "-z",
        "--flash_mode", "qio",
        "--flash_size", "16MB",
        "0x0", files["bootloader"],
        "0x8000", files["partitions"],
        "0x10000", files["firmware"],
    ]

    print(f"Uploading {args.env} to {port} with watchdog-reset...")
    print(f"Command: {' '.join(cmd)}")
    print()

    result = subprocess.run(cmd)

    if result.returncode == 0:
        print()
        print("=" * 50)
        print("Upload complete with watchdog-reset!")
        print("The board should boot into the application.")
        print(f"Monitor: pio device monitor -b 115200 -p {port}")
        print("=" * 50)
    else:
        print()
        print("Upload failed! Check connection and port.")
        sys.exit(1)


if __name__ == "__main__":
    main()
