"""Install only the reversible development build; resets saved test Wi-Fi settings."""
import argparse
import json
import re
from pathlib import Path
import time

import esptool
from esptool import cmds
from wifi_setup import select_port

ROOT = Path(__file__).resolve().parent


def backed_up_board():
    """Keep the board restriction and backup location in ignored local metadata."""
    private = (ROOT / 'private').resolve()
    try:
        settings = json.loads((private / 'board.json').read_text())
        expected_mac = settings['expected_mac'].replace(':', '').lower()
        backup = (private / settings['backup_file']).resolve()
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        raise RuntimeError('Configure private/board.json with expected_mac and backup_file before installing.') from exc
    if not re.fullmatch(r'[0-9a-f]{12}', expected_mac):
        raise RuntimeError('Invalid expected_mac in private/board.json.')
    if not backup.is_relative_to(private):
        raise RuntimeError('The firmware backup must be inside private/.')
    if not backup.is_file() or backup.stat().st_size != 0x400000:
        raise RuntimeError('The original 4 MB firmware backup is required before installing.')
    return expected_mac


def checked_build():
    build = ROOT / 'esp32_wifi' / 'build-development'
    config = json.loads((build / 'config' / 'sdkconfig.json').read_text())
    forbidden = ('SECURE_BOOT', 'SECURE_FLASH_ENC_ENABLED', 'NVS_ENCRYPTION',
                 'BOOTLOADER_APP_ANTI_ROLLBACK')
    if any(config.get(key) for key in forbidden) or not config.get('WIFI_SETUP_DEVELOPMENT'):
        raise RuntimeError('Refusing to flash: this is not the reversible development configuration.')
    files = [(0x1000, build / 'bootloader' / 'bootloader.bin'),
             (0xF000, build / 'partition_table' / 'partition-table.bin'),
             (0x20000, build / 'esp32_wifi.bin')]
    for _, path in files:
        if not path.is_file():
            raise RuntimeError('Build the development firmware first with ./build_wifi.sh --development (Bash) or .\\build_wifi.ps1 -Development (PowerShell).')
    return files


def efuses(esp):
    # ESP32 read-only eFuse block registers 0..30 (not eFuse write registers).
    return [esp.read_reg(esp.EFUSE_RD_REG_BASE + i * 4) for i in range(31)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port')
    parser.add_argument('--app-only', action='store_true', help='Update the app while preserving Wi-Fi settings; partition layout must match')
    args = parser.parse_args()
    files = checked_build()
    expected_mac = backed_up_board()
    port = select_port(args.port)
    esp = esptool.detect_chip(port)
    try:
        if esp.CHIP_NAME != 'ESP32' or bytes(esp.read_mac()).hex() != expected_mac:
            raise RuntimeError('This installer is restricted to the backed-up ESP32 board.')
        if esp.get_secure_boot_enabled() or esp.get_flash_encryption_enabled():
            raise RuntimeError('Security is already enabled on this chip; this installer will not change it.')
        before = efuses(esp)
        (ROOT / 'private' / 'development-efuses-before.json').write_text(json.dumps(before))
        esp = esp.run_stub()
        esp.change_baud(460800)
        cmds.attach_flash(esp)
        if args.app_only:
            expected_table = files[1][1].read_bytes()
            actual_table = cmds.read_flash(esp, 0xF000, len(expected_table), no_progress=True)
            if actual_table != expected_table:
                raise RuntimeError('Partition layout differs; refusing an app-only update.')
            files = files[2:]
        else:
            # Initialize new NVS/keys/PHY area for a first installation.
            cmds.erase_region(esp, 0x10000, 0x8000)
        cmds.write_flash(esp, [(offset, str(path)) for offset, path in files],
                         flash_size='4MB', flash_mode='dio', flash_freq='40m', compress=True)
    finally:
        esp.hard_reset()
        esp._port.close()
    time.sleep(3)
    esp = esptool.detect_chip(port)
    try:
        after = efuses(esp)
        (ROOT / 'private' / 'development-efuses-after.json').write_text(json.dumps(after))
        if before != after:
            raise RuntimeError('The eFuse comparison differed; investigate before further changes.')
        print('Development firmware installed. All 31 eFuse read-register values are unchanged.')
    finally:
        esp.hard_reset()
        esp._port.close()


if __name__ == '__main__':
    main()
