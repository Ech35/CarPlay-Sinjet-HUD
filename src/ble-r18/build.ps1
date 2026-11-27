param([Parameter(Mandatory=$true)][string]$Zig)
$ErrorActionPreference='Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
    & $Zig cc -target arm-linux-musleabihf -mcpu=cortex_a7-neon -marm -Oz -fno-lto -static -s -Wall -Wextra `
        -ffunction-sections -fdata-sections '-Wl,--gc-sections' -o u5hud_ble u5hud_ble.c core_adapter.c core/vendor/rgd/rgd_tlv.c
    if($LASTEXITCODE){throw 'HUD BLE helper build failed'}
} finally { Pop-Location }
