r"""Interactive USB Wi-Fi setup for the companion ESP32 firmware.

Run: .venv\Scripts\python.exe wifi_setup.py [--port COM6]
This program never flashes the board or enables hardware security.
"""

import argparse
import getpass
import json
import sys
import time
import warnings

import serial
from serial.tools import list_ports


class BoardTimeout(RuntimeError):
    pass


def safe_text(value):
    """Do not allow nearby SSIDs to inject terminal control sequences."""
    return str(value).encode("unicode_escape").decode("ascii")


class Board:
    def __init__(self, port):
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = 115200
        self.serial.timeout = 0.3
        self.serial.write_timeout = 3
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.sequence = 0

    def close(self):
        self.serial.close()

    def request(self, command, timeout=40, **fields):
        self.sequence += 1
        request_id = self.sequence
        payload = json.dumps(dict(id=request_id, cmd=command, **fields),
                             ensure_ascii=True, separators=(",", ":")).encode() + b"\n"
        self.serial.write(payload)
        deadline = time.monotonic() + timeout
        buffer = bytearray()
        while time.monotonic() < deadline:
            chunk = self.serial.read(1)
            if not chunk:
                continue
            if chunk != b"\n":
                buffer.extend(chunk)
                if len(buffer) > 16384:
                    raise RuntimeError("Oversized response from board.")
                continue
            try:
                reply = json.loads(buffer)
            except (ValueError, UnicodeError):
                buffer.clear()  # Ignore ROM boot messages.
                continue
            buffer.clear()
            if not isinstance(reply, dict) or reply.get("id") != request_id:
                continue
            if not reply.get("ok"):
                raise RuntimeError(safe_text(reply.get("error", "Board rejected request.")))
            return reply
        raise BoardTimeout("No response from Wi-Fi setup firmware. If it has not been installed, "
                           "see WIFI_SETUP.md. Otherwise close other serial monitors and tap RESET.")


def select_port(explicit):
    if explicit:
        return explicit
    ports = [p for p in list_ports.comports() if p.vid is not None]
    if not ports:
        raise RuntimeError("No USB serial device found. Check the USB cable and driver.")
    if len(ports) == 1:
        return ports[0].device
    for index, port in enumerate(ports, 1):
        print(f"{index}. {port.device}: {safe_text(port.description)}")
    while True:
        value = input("Select the ESP32 port: ").strip()
        if value.isdigit() and 1 <= int(value) <= len(ports):
            return ports[int(value) - 1].device


def choose_network(networks):
    networks = [n for n in networks if n.get("supported") and n.get("ssid")]
    seen = set()
    networks = [n for n in networks if not (n["ssid"] in seen or seen.add(n["ssid"]))]
    for index, network in enumerate(networks, 1):
        print(f"{index}. {safe_text(network['ssid'])}  ({network['rssi']} dBm, WPA2/WPA3)")
    print("H. Enter a hidden network   R. Scan again   Q. Quit")
    while True:
        choice = input("Choose a network: ").strip().lower()
        if choice in ("q", "r"):
            return choice, None
        if choice == "h":
            ssid = input("Network name (SSID, exact spelling): ")
            if 1 <= len(ssid.encode("utf-8")) <= 32 and "\x00" not in ssid:
                return "connect", ssid
            print("SSID must be 1–32 UTF-8 bytes and contain no NUL character.")
        elif choice.isdigit() and 1 <= int(choice) <= len(networks):
            return "connect", networks[int(choice) - 1]["ssid"]


def read_password():
    while True:
        # Refuse getpass's fallback that would echo the password.
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            try:
                password = getpass.getpass("Wi-Fi password (hidden): ")
            except getpass.GetPassWarning as exc:
                raise RuntimeError("Use a real terminal that supports hidden password input.") from exc
        if 8 <= len(password) <= 63 and all(32 <= ord(c) <= 126 for c in password):
            return password
        print("Use a WPA2/WPA3 personal passphrase of 8–63 printable ASCII characters.")


def read_status(board):
    for attempt in range(3):
        try:
            status = board.request("status", timeout=2)
            break
        except BoardTimeout:
            if attempt == 2:
                raise
    if status.get("protocol") != 1:
        raise RuntimeError("Unsupported board firmware protocol.")
    return status


def check_mode(status, development=False):
    if status.get("secure_storage"):
        return
    if development and status.get("development") and status.get("storage_ready"):
        print("DEVELOPMENT MODE: saved Wi-Fi credentials are unencrypted on the board.")
        print("Use test credentials and a trusted USB connection. No permanent security settings change.")
        return
    raise RuntimeError("Hardware security is not enabled. No password will be requested "
                       "or sent. For the test firmware, rerun with --development.")


def authorize(board, status):
    if status.get("development"):
        board.request("unlock", timeout=5, development_ack=True)
    else:
        print("Use a trusted PC and USB cable. Hold BOOT on the board while pressing Enter below.")
        input("Press Enter to allow changes: ")
        board.request("unlock", timeout=5)
        print("You can release BOOT. Setup is unlocked for five minutes.")


def main():
    parser = argparse.ArgumentParser(description="Configure ESP32 Wi-Fi repeatedly over USB.")
    parser.add_argument("--port", help="ESP32 serial port (otherwise detected automatically)")
    parser.add_argument("--development", action="store_true", help="Allow the reversible test firmware's unencrypted Wi-Fi storage")
    parser.add_argument("--status", action="store_true", help="Show board status without changing settings")
    parser.add_argument("--scan", action="store_true", help="List networks without entering credentials")
    parser.add_argument("--forget", action="store_true", help="Forget the saved network and disconnect")
    args = parser.parse_args()
    board = Board(select_port(args.port))
    try:
        status = read_status(board)
        print(f"ESP32 {safe_text(status['mac'])}")
        if args.status:
            print(json.dumps(status, indent=2, ensure_ascii=True))
            return 0
        check_mode(status, args.development)
        if status.get("connected"):
            print(f"Currently connected; IP address: {safe_text(status.get('ip', ''))}")
        if args.scan:
            for network in board.request("scan")["networks"]:
                print(f"{safe_text(network['ssid'])}: {network['rssi']} dBm")
            return 0
        authorize(board, status)
        if args.forget:
            board.request("forget")
            print("Saved network forgotten; board disconnected.")
            return 0
        while True:
            print("Scanning the ESP32's nearby 2.4 GHz networks...")
            networks = board.request("scan")["networks"]
            action, ssid = choose_network(networks)
            if action == "q":
                return 0
            if action == "r":
                continue
            password = read_password()
            try:
                result = board.request("connect", ssid=ssid, password=password)
            except RuntimeError as exc:
                print(f"Setup attempt was not confirmed: {exc}")
                continue
            finally:
                password = None  # Python cannot guarantee wiping all temporary string copies.
            print(f"Connected to {safe_text(ssid)}. IP address: {safe_text(result['ip'])}")
            storage_label = "encrypted" if status.get("secure_storage") else "unencrypted test"
            print(f"Credentials saved in {storage_label} device storage. The ESP32 will reconnect on power-up.")
            print("Run this program again whenever you want to change networks.")
            print("Wi-Fi connection confirmed; internet/email access has not been tested.")
            return 0
    finally:
        try:
            board.request("lock", timeout=1)
        except (RuntimeError, serial.SerialException):
            pass
        board.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, serial.SerialException, EOFError) as exc:
        print(f"Setup stopped: {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nSetup cancelled.")
        sys.exit(130)
