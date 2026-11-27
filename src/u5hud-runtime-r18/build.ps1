param(
    [Parameter(Mandatory = $true)][string]$Zig,
    [Parameter(Mandatory = $true)][string]$Python
)
$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
    & $Zig cc -target arm-linux-musleabihf -mcpu=cortex_a7-neon -marm -Oz -static -s `
        -ffunction-sections -fdata-sections '-Wl,--gc-sections' -o u5hud.cgi u5hud.cgi.c
    if ($LASTEXITCODE) { throw 'Runtime CGI compilation failed' }
} finally {
    Pop-Location
}
