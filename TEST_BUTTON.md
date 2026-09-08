# Onboard email test button

Use the board's **BOOT** button (sometimes labeled FLASH or IO0), connected to
GPIO0. No external button or wiring is needed. GPIO27 is no longer the test input.
Leave the sensor wiring as it is (S: GPIO34, +: GPIO25, -: GND).

The other button, **EN/RESET**, resets the chip and does not request email.
Do not hold BOOT while powering up or pressing RESET: that enters firmware
download mode. If this happens, release BOOT and tap RESET to restart normally.

Allow the board to start and Wi-Fi to connect, then press BOOT for about half a
second and release. The email subject is **TEST - ESP32 water alert**. Its body identifies
the manual button test, sensor location, current state/reading, and board MAC.
It uses the saved Mailjet keys and sender/recipient. Water-alert emails retain
their existing template.

- A separate saved Mailjet profile allows button tests even with automatic
  water detection disabled. An existing armed alarm's settings also work.
- Button tests bypass the water-alert cooldown and never change its deadline or
  clear a pending water event. Automatic water alerts retain their saved interval.
- Presses while offline or another request is running coalesce into one pending
  test, which runs when connected and the current request finishes. There is no
  manual-test cooldown. A failed test attempt is not automatically retried;
  press again to request another. Pending tests are not saved across power loss.
- Debounce is 60 ms. Holding the button sends only one request. A button held at
  startup must be released before a new press is recognized.
- Disabling the water alarm clears the currently queued test; later physical
  presses still work if the separate Mailjet profile exists.

Inspect status without sending email:

```powershell
.venv\Scripts\python.exe water_setup.py --status
```

Look for `test_button_ready`, `test_button_presses`, `test_button_pending`,
`cooldown_remaining`, `attempts_this_boot`, `accepted_this_boot`, and `last_error`.
Mailjet acceptance does not guarantee inbox delivery; check spam as well.

HTTPS failures now include the ESP-IDF error name/code, connection stage, socket
errno, TLS error, mbedTLS code, certificate flags, HTTP status, response byte count,
and overflow flag. These diagnostics contain no credentials or raw response body.
An error after request headers were sent may still mean Mailjet accepted the
message; check activity before pressing again. Old generic errors cannot be
decoded retrospectively; use a new button press with the updated firmware.
