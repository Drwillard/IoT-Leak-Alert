"""Calibrate the water probe and configure autonomous Mailjet water alerts."""
import argparse
import json
import sys
import time

import serial
from email_test import email_address, read_key
from wifi_setup import Board, authorize, check_mode, read_status


def thresholds(dry_values, wet_values):
    dry_max = max(dry_values)
    wet_min = min(wet_values)
    gap = wet_min - dry_max
    if min(dry_values + wet_values) < 0 or gap < 100:
        raise RuntimeError("Dry and wet readings overlap or are too close. Check wiring, dry the probe, and recalibrate.")
    return dry_max + gap // 3, dry_max + 2 * gap // 3


def sample(board, count=16):
    values = []
    for _ in range(count):
        reading = board.request("water_status")["water"]["raw"]
        if reading < 0:
            raise RuntimeError("No valid sensor reading. Check GPIO34 signal and GPIO25 power wiring.")
        values.append(reading)
        print(f"  Sensor: {reading:4d}/4095", flush=True)
        time.sleep(0.55)
    return values


def cooldown():
    while True:
        value = input("Minimum minutes between email attempts [30]: ").strip() or "30"
        try:
            minutes = int(value)
            if 1 <= minutes <= 1440:
                return minutes * 60
        except ValueError:
            pass
        print("Enter a whole number from 1 to 1440.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--development", action="store_true")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--status", action="store_true")
    group.add_argument("--watch", action="store_true")
    group.add_argument("--disable", action="store_true")
    args = parser.parse_args()
    from wifi_setup import select_port
    board = Board(select_port(args.port))
    api_key = secret_key = None
    try:
        status = read_status(board)
        if not status.get("water_alarm"):
            raise RuntimeError("Install the water-alarm firmware first.")
        if args.status:
            print(json.dumps(board.request("water_status")["water"], indent=2))
            return 0
        check_mode(status, args.development)
        if args.disable:
            authorize(board, status)
            board.request("water_disable")
            print("Automatic alerts disabled; sensor configuration removed. Wi-Fi and the separate Mailjet profile are retained.")
            return 0
        print("Wiring with USB power unplugged: sensor S -> GPIO34, + -> GPIO25, - -> GND.")
        print("Use the printed GPIO numbers. Do not connect this sensor to 5V. Keep the board and connector dry.")
        if args.watch:
            authorize(board, status)
            board.request("water_start")
            print("Monitoring; Ctrl+C exits. Existing armed alerts remain armed.")
            while True:
                print(json.dumps(board.request("water_status")["water"]), flush=True)
                time.sleep(1)
        input("After wiring and reconnecting USB, press Enter to calibrate (disables any existing alarm): ")
        authorize(board, status)
        board.request("water_disable")
        board.request("water_start")
        time.sleep(1)
        input("Keep the sensing traces completely DRY, then press Enter: ")
        dry_values = sample(board)
        input("Wet the exposed traces with a few drops of tap water (not the connector), then press Enter: ")
        wet_values = sample(board)
        dry, wet = thresholds(dry_values, wet_values)
        print(f"Calibrated: wet >= {wet}, dry <= {dry}. Alerts are still disabled.")
        label = input("Sensor location [Water sensor]: ").strip() or "Water sensor"
        if len(label) > 48 or any(not 32 <= ord(c) <= 126 for c in label):
            raise RuntimeError("Location must use 1-48 printable ASCII characters.")
        saved_mail = None
        if status.get("mailjet_setup"):
            authorize(board, status)
            profile = board.request("mailjet_status")["mailjet"]
            if profile.get("saved_profile"):
                saved_mail = profile
        if saved_mail:
            seconds = saved_mail['cooldown_seconds']
            sender, recipient = saved_mail['sender'], saved_mail['recipient']
            print("Using saved Mailjet settings. Run mailjet_setup.py to update them.")
        else:
            seconds = cooldown()
            sender = email_address("Verified Mailjet sender address: ")
            recipient = email_address("Alert recipient (Enter = sender): ", sender)
            api_key = read_key("API key", username=True)
            secret_key = read_key("secret key")
        print(f"Location: {label}\nFrom: {sender}\nTo: {recipient}")
        print(f"Confirm water for 2 seconds. Send at most once every {seconds // 60} minutes, including failures.")
        print("Repeat alerts while wet. A brief wet event can be reported later after the cooldown or Wi-Fi returns.")
        storage = "encrypted" if status.get("secure_storage") else "UNENCRYPTED development"
        print(f"Autonomous operation saves BOTH MAILJET KEYS in {storage} storage on the ESP32.")
        print("Dry the sensor now if you do not want an immediate water alert after arming.")
        if input("Type ARM to save and enable automatic emails, or Enter to cancel: ").strip() != "ARM":
            print("Cancelled. Alerts remain disabled.")
            return 0
        authorize(board, status)
        board.request("water_configure", arm=True, label=label, sender=sender, recipient=recipient,
                      use_saved_mail=bool(saved_mail),
                      api_key=api_key, secret_key=secret_key, dry_threshold=dry, wet_threshold=wet,
                      cooldown_seconds=seconds)
        print("Armed. The ESP32 monitors and emails independently, including after power-up.")
        print("Keep it powered. Use --watch for readings or --disable to stop alerts.")
        return 0
    finally:
        api_key = secret_key = None
        try:
            board.request("lock", timeout=1)
        except (RuntimeError, serial.SerialException):
            pass
        board.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, serial.SerialException, EOFError) as exc:
        print(f"Water setup stopped: {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nTerminal closed. Any saved/armed alarm continues on the ESP32; --disable stops it.")
        sys.exit(130)
