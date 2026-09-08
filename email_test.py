"""Send one Mailjet test message directly from the connected ESP32 over Wi-Fi."""
import argparse
import getpass
import re
import sys
import warnings

import serial
from wifi_setup import Board, authorize, check_mode, read_status, safe_text, select_port


def email_address(prompt, default=""):
    while True:
        value = input(prompt).strip() or default
        if len(value) <= 254 and re.fullmatch(r"[A-Za-z0-9._+-]+@[A-Za-z0-9._+-]+", value):
            return value
        print("Enter a plain email address, without a display name.")


def read_key(label, username=False):
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        try:
            value = getpass.getpass(f"Mailjet {label} (hidden): ").strip()
        except getpass.GetPassWarning as exc:
            raise RuntimeError("Use a real terminal that supports hidden input.") from exc
    if not 1 <= len(value) <= 128 or any(not 33 <= ord(c) <= 126 for c in value) or (username and ':' in value):
        raise RuntimeError("Use the key from Mailjet (1-128 printable characters, without spaces).")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--development", action="store_true")
    parser.add_argument("--sandbox", action="store_true", help="Ask Mailjet to validate without delivering email")
    args = parser.parse_args()
    board = Board(select_port(args.port))
    api_key = secret_key = None
    try:
        status = read_status(board)
        check_mode(status, args.development)
        if not status.get("email_test") or status.get("email_provider") != "mailjet":
            raise RuntimeError("Install the Mailjet test firmware first. No keys have been requested or sent.")
        if not status.get("connected"):
            raise RuntimeError("Connect the ESP32 first: run wifi_setup.py --development.")
        print(f"ESP32 {safe_text(status['mac'])}, IP {safe_text(status.get('ip', ''))}")
        print("Mailjet test: the ESP32 connects directly to https://api.mailjet.com/v3.1/send.")
        print("Both keys are used once in RAM and are not saved on the PC or board.")
        sender = email_address("Your verified Mailjet sender address: ")
        recipient = email_address("Recipient address (Enter = yourself): ", sender)
        api_key = read_key("API key", username=True)
        secret_key = read_key("secret key")
        print(f"From: {sender}\nTo: {recipient}\nSubject: ESP32 Wi-Fi email test")
        print("Body: a short test message identifying the ESP32 by its MAC address.")
        confirmation = "TEST" if args.sandbox else "SEND"
        action = "validate without delivery" if args.sandbox else "send this one message"
        if input(f"Type {confirmation} to {action}, or Enter to cancel: ").strip() != confirmation:
            print("Cancelled. No email sent.")
            return 0
        authorize(board, status)
        print("ESP32 is synchronizing its clock and contacting Mailjet...")
        result = board.request("send_email", timeout=110, sender=sender, recipient=recipient,
                               api_key=api_key, secret_key=secret_key, sandbox=args.sandbox)
        expected = "validated_by_mailjet" if args.sandbox else "accepted_by_mailjet"
        if result.get("result") != expected:
            raise RuntimeError("Mailjet result was not confirmed. Check Mailjet activity before retrying.")
        if args.sandbox:
            print("Mailjet validated the request in sandbox mode. No email was delivered.")
        else:
            print("Mailjet accepted the ESP32's message. Check your inbox and spam folder for delivery.")
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
        print(f"Email test stopped: {exc}", file=sys.stderr)
        print("If sending had started, check Mailjet activity before retrying; it may already have been accepted.", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nCancelled. If sending had started, check Mailjet activity before retrying.")
        sys.exit(130)
