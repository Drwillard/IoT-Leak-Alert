r"""Display an attached ESP32's chip model and unique factory MAC on this PC.

Setup: .\.venv\Scripts\python.exe -m pip install "esptool>=5,<6"
Run:   .\.venv\Scripts\python.exe eps32.py
Choose a port: append --port COM5. List ports: append --list.
Identification briefly resets the board; it does not flash firmware.
"""

import argparse
import importlib.util
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial port, for example COM5")
    parser.add_argument("--list", action="store_true", help="Only list serial ports")
    args = parser.parse_args()

    try:
        from serial.tools import list_ports
    except ImportError:
        print('Install dependencies: python -m pip install "esptool>=5,<6"', file=sys.stderr)
        return 1

    ports = sorted(list_ports.comports(), key=lambda port: port.device)
    print("Serial ports detected:", flush=True)
    for port in ports:
        print(f"  {port.device}: {port.description}", flush=True)
    if not ports:
        print("  None", flush=True)
    if args.list:
        return 0

    if args.port:
        selected = next((p for p in ports if p.device.lower() == args.port.lower()), None)
        if selected is None:
            print(f"Port {args.port} is not connected.", file=sys.stderr)
            return 1
    else:
        # Bluetooth ports are not the board's wired USB connection.
        candidates = [p for p in ports if p.vid is not None]
        if not candidates:
            print("No USB serial board detected. Connect the ESP32 with a USB data cable, "
                  "then run this script again. For a separate serial adapter, use --port COMx.",
                  file=sys.stderr)
            return 1
        if len(candidates) > 1:
            print("Multiple USB serial devices detected. Select the ESP32 with --port COMx.",
                  file=sys.stderr)
            return 1
        selected = candidates[0]

    if importlib.util.find_spec("esptool") is None:
        print('Install dependencies: python -m pip install "esptool>=5,<6"', file=sys.stderr)
        return 1

    print(f"\nReading board identity on {selected.device}...\n"
          "The output below includes the chip model and factory MAC (unique board ID).",
          flush=True)
    try:
        result = subprocess.run(
            [sys.executable, "-m", "esptool", "--port", selected.device,
             "--baud", "115200", "--no-stub", "read-mac"],
            timeout=45,
        )
    except subprocess.TimeoutExpired:
        print("Timed out waiting for the board.", file=sys.stderr)
        return 1
    if result.returncode:
        print("Close any serial monitor using this port. If connection fails, hold BOOT, "
              "tap RESET, run again, then release BOOT once connected.", file=sys.stderr)
    return result.returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
