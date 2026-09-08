# Saved Mailjet setup

Run from this folder in a terminal:

```powershell
.venv\Scripts\python.exe mailjet_setup.py --development
```

Choose the verified sender, recipient, and minimum interval (1–1440 minutes).
Enter both Mailjet keys using hidden prompts, then type `SAVE`. Run the same
command whenever you want to replace the settings. Enter keeps the current
address/interval defaults; both keys must be entered on every update.
Neither key is displayed or returned by the device. Setup does not write
credentials to a PC file, contact Mailjet, or request a test email.

The development firmware stores these settings **unencrypted** in device flash.
No permanent security/eFuse changes are made. Setup uses USB and works without
Wi-Fi. Use `--port COM6` if automatic port selection is ambiguous.

```powershell
.venv\Scripts\python.exe mailjet_setup.py --development --status
```

Status shows addresses, interval, and alarm state, never keys. Sensor calibration,
Wi-Fi, and armed state are preserved when saving. Already armed alerts continue
and use the new settings. The current reserved cooldown is preserved; subsequent
attempts use the new interval, including failed attempts. The existing water-alert
subject/body template is unchanged.

If the alarm is disabled, run `water_setup.py --development` to calibrate and arm
it. Calibration automatically uses this saved Mailjet profile. Disabling or
recalibrating the sensor retains the separate Mailjet profile. Existing older
alarm settings remain supported; running Mailjet setup creates the separate profile.

Firmware update (development only, preserves saved settings):

```powershell
.\build_wifi.ps1 -Development
.venv\Scripts\python.exe flash_development.py --port COM6 --app-only
```
