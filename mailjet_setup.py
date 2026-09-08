"""Save or update the ESP32's autonomous Mailjet alert settings over USB."""
import argparse
import json
import sys

import serial
from email_test import email_address, read_key
from wifi_setup import Board, authorize, check_mode, read_status, select_port


def alert_interval(default=1800):
    while True:
        value = input(f"Minimum minutes between email attempts [{default // 60}]: ").strip()
        try:
            minutes = int(value) if value else default // 60
            if 1 <= minutes <= 1440:
                return minutes * 60
        except ValueError:
            pass
        print("Enter a whole number from 1 to 1440.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--development", action="store_true")
    parser.add_argument("--status", action="store_true", help="Show saved addresses and interval; never keys")
    args = parser.parse_args()
    board = Board(select_port(args.port))
    api_key = secret_key = None
    try:
        status = read_status(board)
        check_mode(status, args.development)
        if not status.get("mailjet_setup"):
            raise RuntimeError("Install the updated Mailjet setup firmware first.")
        authorize(board, status)
        current = board.request("mailjet_status")["mailjet"]
        if args.status:
            print(json.dumps(current, indent=2))
            return 0
        print("Save Mailjet settings on the ESP32. Wi-Fi is not required for setup.")
        print("Both keys use hidden input and must be entered each time; stored keys are never returned.")
        if not status.get("secure_storage"):
            print("Development mode: keys and addresses are saved UNENCRYPTED on the device.")
        sender = email_address(f"Verified Mailjet sender [{current['sender']}]: ", current['sender'])
        recipient_default = current['recipient'] or sender
        recipient = email_address(f"Alert recipient [{recipient_default}]: ", recipient_default)
        seconds = alert_interval(current['cooldown_seconds'])
        api_key = read_key("API key", username=True)
        secret_key = read_key("secret key")
        print(f"From: {sender}\nTo: {recipient}\nMinimum interval: {seconds // 60} minutes")
        print("The existing water-alert message template will be used.")
        print("An armed alarm will use these settings; a disabled alarm stays disabled.")
        print("Any current cooldown finishes first; the new interval applies to subsequent attempts.")
        if input("Type SAVE to store these settings, or Enter to cancel: ").strip() != "SAVE":
            print("Cancelled. Saved settings were not changed.")
            return 0
        authorize(board, status)
        board.request("mailjet_save", sender=sender, recipient=recipient,
                      api_key=api_key, secret_key=secret_key, cooldown_seconds=seconds)
        print("Saved on the ESP32. No test email requested; Mailjet credentials have not been validated online.")
        if not current['armed']:
            print("Run water_setup.py --development to calibrate and arm the water alarm.")
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
        print(f"Mailjet setup stopped: {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nSetup interrupted. Any existing armed alarm continues.")
        sys.exit(130)
