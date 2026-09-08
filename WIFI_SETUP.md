# ESP32 Wi-Fi and Mailjet development test

For the autonomous water detector, follow [WATER_SENSOR.md](WATER_SENSOR.md)
and run `water_setup.py --development`. Alerts remain disabled until calibration
and explicit arming. Water-alarm setup saves Mailjet keys on the board in
unencrypted development storage so it can operate without the PC.

The reversible development firmware is installed on the ESP32, currently COM6.
It replaces the camera application for testing.
Secure boot and flash encryption are **off**. All 31 eFuse read-register values
matched before and after installation. Ordinary firmware updates remain possible.

The original camera firmware has a verified 4 MB backup in the ignored `private/`
directory. Permanent security activation is deferred at the owner's request.

The installer reads `private/board.json` for the local `expected_mac` and
`backup_file` (a filename relative to `private/`). Set these to the backed-up
board's actual MAC and verified backup file before installing. This ignored file
keeps device identity out of Git while retaining the installer's board check.

## 1. Connect Wi-Fi

In PowerShell, from `C:\code\arduino`:

```powershell
.\.venv\Scripts\python.exe wifi_setup.py --development
```

Select a network and enter its password at the hidden prompt. You can rescan
or enter a hidden SSID. The board tries to obtain an IP address for 25 seconds
and saves the profile after success. A failed Wi-Fi attempt retains the previous
saved network. Only the latest successful profile is remembered.

Run the same program whenever you change networks. This does not reflash
firmware or change eFuses. The board reconnects on power-up and after
 disconnections without a PC program running; it still needs power.

The development build does not require holding BOOT. The `--development` flag
explicitly acknowledges the weaker test mode. Supported: 2.4 GHz WPA2/WPA3-Personal
with 8-63 printable ASCII passphrases. Open, WEP, WPA-only, enterprise, 5 GHz and
captive-portal networks are unsupported. Scanning briefly interrupts an existing
connection and then restores it.

**Saved Wi-Fi credentials are unencrypted in this test build.** Use a temporary
test network/password when practical. Password entry is hidden, but USB/UART
traffic is also unencrypted and requires a trusted PC and cable.

## 2. Send one Mailjet test email from the ESP32

```powershell
.\.venv\Scripts\python.exe email_test.py --development
```

Enter your verified Mailjet sender address, recipient (defaults to yourself),
API key, and secret key. Both key prompts are hidden. The terminal previews the
sender, recipient and subject and sends only when you type `SEND`.

The ESP32 makes an HTTPS POST to `https://api.mailjet.com/v3.1/send`, using Basic
authentication with the API key and secret key. It sends a `Messages` array
containing `From`, `To`, `Subject` and `TextPart`. The sender must be verified and
active in your Mailjet account.
[Mailjet Send API documentation](https://dev.mailjet.com/docs/email-api/send-api-v31/send-basic-email).

To validate with Mailjet without delivering an email:

```powershell
.\.venv\Scripts\python.exe email_test.py --development --sandbox
```

This sets `SandboxMode` to true and asks you to type `TEST`. A successful sandbox
request is reported as validation, never as delivery. The normal command
explicitly sets `SandboxMode` to false and submits a real message.

The ESP32 synchronizes its clock using NTP and verifies the HTTPS server's
certificate chain and hostname. There is no insecure fallback or redirect to
another host. The PC supplies parameters over USB; the ESP32 makes the API call.

HTTP success alone is not enough: the firmware also requires Mailjet's message
status to be `success`. Acceptance does not prove final inbox delivery. Check
Inbox, Spam and Mailjet activity. No request is automatically retried; if the
connection drops after sending begins, check Mailjet activity before retrying.

Both keys are used for the current request only and are not saved in NVS or a
PC file. Enter them again for each test, locally rather than in chat. Python and
network libraries cannot guarantee wiping every temporary RAM copy. USB/UART
traffic is unencrypted, so use a trusted PC and cable. Unattended email on future
events is now configured separately using `water_setup.py`; this one-off test
does not save keys or arm the water alarm.

## Status, scans and forgetting a test network

```powershell
.\.venv\Scripts\python.exe wifi_setup.py --status
.\.venv\Scripts\python.exe wifi_setup.py --development --scan
.\.venv\Scripts\python.exe wifi_setup.py --development --forget
```

Use `--port COM6` if needed; otherwise the USB port is detected. Close Arduino
Serial Monitor or any other program holding the same port.

`--forget` removes the active saved profile and disconnects. It is not a forensic
secure erase: old plaintext NVS entries can remain in flash until overwritten.
Use temporary test credentials and keep your Mailjet keys private.

## Validation

- Development firmware compiled successfully and written-flash hashes verified.
- All 31 eFuse read-register values were unchanged across installation and boot.
- Live firmware reports development mode, ready storage, secure boot off and
  flash encryption off.
- Wi-Fi scanning succeeded on the physical board.
- Nineteen PC tests passed, including calibration, development opt-in, hidden input, retries,
  cancellation, header-injection rejection and email failures not reported as success.
- The board reconnected to your saved Wi-Fi after the firmware update, and you
  confirmed the Mailjet test worked. Physical water calibration and automatic
  water-alert delivery still need your local setup. No automatic water email
  has been triggered by the agent.

## Rebuild / reinstall when developing firmware

```powershell
.\build_wifi.ps1 -Development
.\.venv\Scripts\python.exe flash_development.py --port COM6 --app-only
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

Docker Desktop must be running. The build uses `espressif/idf:v5.5.2`.
The installer only accepts the development build and this backed-up board;
it refuses builds enabling secure boot, flash encryption, encrypted NVS or
anti-rollback. The --app-only update preserves Wi-Fi settings and requires a matching partition layout. A full installation without that flag resets test Wi-Fi settings. It checks eFuses
before and after boot. Use the interactive setup script, not the installer,
for ordinary Wi-Fi changes.

The earlier secure-build sources and restricted signing key remain available
for later review, but that firmware is not installed. Do not install a secure
bootloader or burn eFuses during this development phase. The new email changes
have been built and validated in development mode only.

