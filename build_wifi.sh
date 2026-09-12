#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
mode=default
for arg in "$@"; do
    case "$arg" in
        --security|--development)
            if [[ "$mode" != default ]]; then
                echo 'Choose only one build mode.' >&2
                exit 2
            fi
            mode="${arg#--}"
            ;;
        -h|--help)
            echo 'Usage: ./build_wifi.sh [--security | --development]'
            exit 0
            ;;
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if ! command -v docker >/dev/null 2>&1; then
    echo 'Docker is required to build the firmware.' >&2
    exit 1
fi

build_args=(idf.py -B build reconfigure build)
if [[ "$mode" == security ]]; then
    # Building is safe. Booting the resulting bootloader changes permanent eFuses.
    python_exe="$project_root/.venv/bin/python"
    key_dir="$project_root/private"
    key_path="$key_dir/secure_boot_signing_key.pem"
    if [[ ! -x "$python_exe" ]]; then
        echo 'Create .venv and install requirements.txt first (see WIFI_SETUP.md).' >&2
        exit 1
    fi
    umask 077
    mkdir -p -- "$key_dir"
    chmod 700 "$key_dir"
    if [[ ! -e "$key_path" ]]; then
        "$python_exe" -m espsecure generate-signing-key --version 2 "$key_path"
    fi
    chmod 600 "$key_path"
    build_args=(idf.py -B build-secure -D SDKCONFIG=sdkconfig.secure
        -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.security' reconfigure build)
elif [[ "$mode" == development ]]; then
    build_args=(idf.py -B build-development -D SDKCONFIG=sdkconfig.development.generated
        -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.development' reconfigure build)
fi

docker run --rm --mount "type=bind,source=$project_root,target=/project" \
    -w /project/esp32_wifi espressif/idf:v5.5.2 "${build_args[@]}"
echo 'Build complete. No firmware was flashed and no eFuses were changed.'
