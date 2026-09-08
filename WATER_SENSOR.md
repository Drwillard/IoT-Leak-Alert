# Water sensor and autonomous Mailjet alerts

For email testing with the onboard BOOT button, see [button behavior](TEST_BUTTON.md).

To save or update Mailjet keys, addresses, and the alert interval independently
of calibration, use [Mailjet setup](MAILJET_SETUP.md). Calibration reuses that
profile when present; disabling the sensor retains the separate mail profile.

This firmware targets your classic ESP32 and the DIYables three-pin analog water
sensor, ASIN B0BXKMLB4D. Confirm the board headers actually expose GPIO34 and
GPIO25; use the printed GPIO numbers, not physical header positions. If either
is missing, stop and identify the board before wiring.

## Wiring

Unplug USB and any other power before connecting these three wires:

| Sensor label | ESP32 connection | Purpose |
| --- | --- | --- |
| `S` | `34` / `GPIO34` | Analog signal, ADC1 channel 6 |
| `+` | `25` / `GPIO25` | Switched 3.3 V sensor power |
| `-` | `GND` | Ground |

Do not connect the sensor to 5V. GPIO25 powers this one low-current sensor for
about 20 ms every half second, then switches it off. This reduces electrode
corrosion compared with continuous power. GPIO34 belongs to ADC1, which can be
used while Wi-Fi is running. In ESP-IDF 5.5 the attenuation setting is named
`ADC_ATTEN_DB_12` (the current name for the old 11 dB setting).

Use this direct GPIO power connection only for the specified low-current sensor,
not pumps, relays or an arbitrary sensor module. Keep the ESP32, USB connector,
sensor connector and electronics dry; only wet the exposed sensing traces.

References: [DIYables specifications and linked ESP32 tutorial](https://diyables.io/products/water-sensor-detector),
[manufacturer-linked wiring/power guidance](https://esp32io.com/tutorials/esp32-water-sensor),
[Espressif ADC documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32/api-reference/peripherals/adc_oneshot.html).

## Calibrate and arm

Wire the sensor, reconnect USB, then run from `C:\code\arduino`:

```powershell
.\.venv\Scripts\python.exe water_setup.py --development
```

The interactive program:

1. Disables any existing water alarm while you calibrate. Cancelling setup leaves
   it disabled; it does not silently restore old alert settings.
2. Measures the fully dry sensor for about nine seconds.
3. Measures it again after you apply a few drops of tap water to the traces.
4. Rejects calibration if the measurements overlap or have too little separation.
5. Asks for a location label, throttle interval (default 30 minutes), verified
   Mailjet sender, recipient, API key and secret key.
6. Shows the settings and requires you to type `ARM` before saving and enabling
   automatic emails. Dry the sensor before arming if you do not want an immediate
   alert from the calibration water.

For autonomous operation, BOTH Mailjet keys must now remain on the board.
They are saved along with the alarm configuration in **unencrypted development
NVS**, because hardware encryption remains disabled at your request. No keys are
stored in a PC file or returned by status commands. USB/UART remains unencrypted;
enter keys locally on a trusted PC and cable. The one-off `email_test.py` still
uses its supplied keys only for that request.

Once armed, the ESP32 reconnects to the saved Wi-Fi and resumes monitoring after
power-up. The PC program can be closed and the board can run from a suitable USB
power supply. No permanent security settings or eFuses are changed.

## Alert behavior

- Read the powered sensor approximately twice a second, averaging 16 ADC samples.
- Confirm a new wet condition for 2 seconds; confirm dry for 5 seconds.
- Use separate calibrated wet/dry thresholds so readings near the boundary do
  not repeatedly toggle the state.
- First confirmed wet event is eligible immediately unless an earlier alert
  attempt is still in its cooldown.
- While wet, send reminders no more frequently than the chosen interval.
- Rapid dry/wet cycles do not bypass the throttle. Multiple events in a cooldown
  are coalesced, not turned into a burst of emails.
- A wet event stays pending in RAM if Wi-Fi is unavailable or the cooldown is
  active. It can be reported later even if the sensor is dry by then; the message
  explains that it is now dry. A power failure can lose a transient pending event;
  water still present at startup will be detected again.
- Reserve and persist the next eligible time BEFORE contacting Mailjet. Failed
  or ambiguous requests also consume the cooldown, reducing duplicate emails.
- A reboot restores the cooldown using network time. Clock-sync failures defer
  sending; the board needs a valid clock to check HTTPS certificates. Clock sync
  may be retried once a minute, but those retries do not send email.
- Updating/disabling/rearming the alarm does not clear the previous cooldown.
  The configured minimum is one minute; use a short interval for bench testing
  and a longer one for ordinary monitoring.
- Email submission and time sync can temporarily delay serial commands, while
  a separate task keeps sampling the sensor. Mailjet acceptance does not guarantee
  final delivery; inspect your inbox and Mailjet activity during testing.

## Observe or stop

```powershell
.\.venv\Scripts\python.exe water_setup.py --status
.\.venv\Scripts\python.exe water_setup.py --development --watch
.\.venv\Scripts\python.exe water_setup.py --development --disable
```

`--watch` starts sampling if needed but does not arm a disabled alarm. It also
does not pause an already armed alarm. Ctrl+C closes the monitor; the saved
alarm continues. `--disable` stops automatic alerts and removes the active alarm
configuration/keys, leaving Wi-Fi intact. Removal is not a forensic secure erase
of old NVS flash entries. Revoke a Mailjet key if it needs to become unusable.

Status shows raw readings, wet/dry state, pending notification, cooldown, attempt
and acceptance counts for this boot, and the last error. It never displays keys.

## Test the complete path

After arming, apply water to the sensing traces and wait a few seconds. Expect a
Mailjet message identifying the sensor location and ESP32 MAC. Leave it wet and
verify that no second message arrives before the configured interval. Dry it,
wait for dry confirmation, and wet it again to check that the cooldown still
applies. Power-cycle it to verify automatic Wi-Fi reconnection and retained
settings. No simulated-water command sends real emails.

This is an experimental leak notification device, not a sole flood-protection
system. It needs power, internet and Mailjet service. The resistive probe can
corrode, low-conductivity water may respond differently, and this three-wire
sensor cannot reliably distinguish a disconnected cable from a dry probe.
Inspect and retest it periodically; recalibrate after changing the probe, wiring
or placement.

## Implementation and validation

- `esp32_wifi/main/water_alarm.c`: power pulses, ADC sampling, persisted settings,
  background monitoring and Mailjet alert scheduling.
- `esp32_wifi/main/water_logic.c`: hysteresis, confirmation and due-event logic.
- `water_setup.py`: local calibration and explicit arming.
- `tests/water_logic_test.c`: runs the actual C logic against spikes, confirmation
  times, hysteresis, deferred events, wet/dry cycling and cooldown behavior.
- Nineteen Python tests cover setup, calibration and existing Wi-Fi/Mailjet flows.

Firmware compilation and software tests are complete. Physical probe calibration
and receipt of an automatic water email must still be tested with your wiring
and locally entered keys. The first installation leaves sensing and alerts off.

For future firmware edits, use `build_wifi.ps1 -Development` and
`flash_development.py --port COM6 --app-only` to preserve Wi-Fi and alarm settings.
An already armed alarm resumes after an app update; disable it first if you are
bench-testing code changes.
