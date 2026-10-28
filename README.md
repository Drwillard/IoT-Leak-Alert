# ESP32 Water Alert

ESP-IDF firmware and Python tools for an ESP32 water detector that sends email
alerts through Mailjet. The board saves its Wi-Fi and alarm settings, reconnects
after startup, and monitors the sensor without a PC once configured and powered.

Features include calibrated wet/dry detection, persistent alert cooldowns,
USB setup tools, and manual email tests using the onboard BOOT button. Earlier
camera capture and board identification utilities are also included.

## Requirements

- Classic ESP32 with 4 MB flash and revision 3 or newer, matching the firmware
  configuration, plus a USB data cable.
- Python 3.10 or newer.
- Docker installed and running to build firmware. The build scripts use
  `espressif/idf:v5.5.2`.
- A supported 2.4 GHz WPA2/WPA3-Personal network with internet access.
- A Mailjet account with a verified sender and API credentials for email alerts.
- For water detection: the DIYables three-pin analog water sensor described in
  [the sensor guide](WATER_SENSOR.md).

## Set up the Python tools

Run commands from the repository root.

### Windows (PowerShell)

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

### macOS / Linux

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --only-binary=cryptography,cffi -r requirements.txt
```

The remaining Python examples use the Windows executable. On macOS/Linux,
replace `.\.venv\Scripts\python.exe` with `.venv/bin/python`.
Serial tools detect the USB port automatically; append `--port COM6` or your
board's `/dev/cu.*` or `/dev/ttyUSB*` path when needed. Close other serial monitors
before connecting.

## Build and install development firmware

On Windows:

```powershell
.\build_wifi.ps1 -Development
```

On macOS/Linux:

```bash
./build_wifi.sh --development
```

Build output goes into `esp32_wifi/build-development/`. Building does not flash
the board or change eFuses.

The installer is restricted to a backed-up board. Before first installation,
place a verified, original 4 MB firmware backup in the ignored `private/`
directory and create `private/board.json` with that board's actual MAC address
and backup filename. For example, replace these placeholders with local values:

```json
{
  "expected_mac": "AA:BB:CC:DD:EE:FF",
  "backup_file": "original-firmware.bin"
}
```

Install on that board:

```powershell
.\.venv\Scripts\python.exe flash_development.py --port COM6
```

A full installation resets saved device settings. For subsequent updates with
the same partition layout, preserve settings with:

```powershell
.\.venv\Scripts\python.exe flash_development.py --port COM6 --app-only
```

An armed alarm resumes after an app update. Disable it before bench testing if
you want to stop automatic emails. The installer checks board identity, build
settings, and eFuse values; see [WIFI_SETUP.md](WIFI_SETUP.md) for details.

## Configure the device

With development firmware installed, connect Wi-Fi:

```powershell
.\.venv\Scripts\python.exe wifi_setup.py --development
```

Save Mailjet sender, recipient, keys, and alert interval:

```powershell
.\.venv\Scripts\python.exe mailjet_setup.py --development
```

Setup requires typing `SAVE` and does not send an email. Once connected, press
and release BOOT for about half a second to request a test using the saved
profile. See [TEST_BUTTON.md](TEST_BUTTON.md) for button behavior.

For water detection, unplug power and wire the specified sensor's `S` to
GPIO34, `+` to GPIO25, and `-` to GND. Confirm these GPIOs are exposed on your
board and follow [WATER_SENSOR.md](WATER_SENSOR.md) before powering it again.
Do not connect the sensor to 5 V.

Calibrate dry and wet readings, then explicitly arm the alarm:

```powershell
.\.venv\Scripts\python.exe water_setup.py --development
```

Calibration disables any existing alarm; automatic alerts remain disabled until
you type `ARM`. Once armed, monitoring continues after the setup tool closes.

Useful commands:

```powershell
.\.venv\Scripts\python.exe wifi_setup.py --status
.\.venv\Scripts\python.exe water_setup.py --status
.\.venv\Scripts\python.exe water_setup.py --development --watch
.\.venv\Scripts\python.exe water_setup.py --development --disable
.\.venv\Scripts\python.exe email_test.py --development --sandbox
```

`--watch` does not pause an armed alarm. The sandbox command validates a one-off
Mailjet request without delivery; omit `--sandbox` to preview and confirm a real
test email. One-off email tests do not save their supplied credentials.

## Development storage and limitations

The development build stores Wi-Fi and saved Mailjet credentials **unencrypted
in device flash**, and USB/UART setup traffic is unencrypted. Enter credentials
locally using a trusted PC and cable. Keep keys, backups, and board metadata in
the ignored `private/` directory; do not commit them.

Development firmware leaves secure boot and flash encryption disabled. Separate
default and security build modes exist, but the documented installation path
uses development mode. Booting a security-enabled bootloader can permanently
change eFuses; it is outside this development workflow.

Alerts require power, internet access, valid network time, and Mailjet service.
Failed or ambiguous automatic email attempts consume the configured cooldown.
Mailjet acceptance does not guarantee inbox delivery. This is an experimental
notification device; periodically inspect the probe and test the complete alert
path. See the sensor guide for calibration and hardware limitations.

## Repository guide

| Path | Purpose |
| --- | --- |
| `esp32_wifi/` | ESP-IDF firmware, configuration, and partition table |
| `wifi_setup.py` | Wi-Fi provisioning, scans, status, and profile removal |
| `mailjet_setup.py` | Saved email profile configuration |
| `water_setup.py` | Sensor calibration, arming, monitoring, and disabling |
| `email_test.py` | One-off real or sandbox Mailjet request from the ESP32 |
| `build_wifi.ps1`, `build_wifi.sh` | Docker-based firmware builds |
| `flash_development.py` | Board-restricted development installer |
| `eps32.py` | Serial port listing and chip/MAC identification |
| `main.py` | Earlier camera firmware's serial JPEG capture client; uses COM5 |
| `tests/` | Python tool tests and standalone C logic tests |

Detailed guides: [Wi-Fi and firmware](WIFI_SETUP.md),
[Mailjet profile](MAILJET_SETUP.md), [water sensor](WATER_SENSOR.md), and
[BOOT button tests](TEST_BUTTON.md).

Run the Python tests without connected hardware:

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

The standalone C tests in `tests/water_logic_test.c` and
`tests/button_logic_test.c` exercise firmware detection and button logic.
Physical calibration and end-to-end email delivery require testing on the board.
