param([switch]$Security, [switch]$Development)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$pythonExe = Join-Path $projectRoot '.venv\Scripts\python.exe'
if ($Security -and $Development) { throw 'Choose only one build mode.' }
$buildArgs = @('idf.py', '-B', 'build', 'reconfigure', 'build')
if ($Security) {
    # Building is safe. Booting the resulting bootloader changes permanent eFuses.
    $keyDir = Join-Path $projectRoot 'private'
    $keyPath = Join-Path $keyDir 'secure_boot_signing_key.pem'
    New-Item -ItemType Directory -Path $keyDir -Force | Out-Null
    $accountSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    & icacls $keyDir /inheritance:r /grant:r "*${accountSid}:(OI)(CI)F" '*S-1-5-18:(OI)(CI)F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not protect the private key directory.' }
    if (-not (Test-Path -LiteralPath $keyPath)) {
        & $pythonExe -m espsecure generate-signing-key --version 2 $keyPath
        if ($LASTEXITCODE -ne 0) { throw 'Signing key generation failed.' }
    }
    $buildArgs = @('idf.py', '-B', 'build-secure', '-D', 'SDKCONFIG=sdkconfig.secure',
                   '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.security', 'reconfigure', 'build')
}
if ($Development) {
    $buildArgs = @('idf.py', '-B', 'build-development', '-D', 'SDKCONFIG=sdkconfig.development.generated',
                   '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.development', 'reconfigure', 'build')
}
# Windows PowerShell treats redirected native stderr (including IDF progress)
# as error records. Use the actual process exit code to detect build failures.
$ErrorActionPreference = 'Continue'
& docker run --rm --mount "type=bind,source=$projectRoot,target=/project" `
    -w /project/esp32_wifi espressif/idf:v5.5.2 @buildArgs
$buildExitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($buildExitCode -ne 0) { throw 'Firmware build failed.' }
Write-Host 'Build complete. No firmware was flashed and no eFuses were changed.'
