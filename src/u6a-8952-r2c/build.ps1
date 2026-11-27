param([Parameter(Mandatory=$true)][string]$Zig,[string]$Python='python')
$ErrorActionPreference='Stop'
$target=@('-target','arm-linux-musleabihf','-mcpu=cortex_a7-neon','-marm')
$include=Join-Path (Split-Path -Parent $Zig) 'lib/include'
$frontend=@('-cc1','-triple','armv7-unknown-linux-musleabihf','-target-cpu','cortex-a7',
  '-mfloat-abi','hard','-target-feature','+strict-align','-target-feature','-neon','-emit-obj','-fno-temp-file')
Push-Location -LiteralPath $PSScriptRoot
try {
  foreach($name in @('probe','cp_probe')){
    & $Zig clang @frontend -ffreestanding -fno-builtin -mrelocation-model pic -pic-level 2 -Oz -isystem $include -o "$name.o" "$name.c"
    if($LASTEXITCODE){throw "Payload compilation failed: $name"}
  }
  foreach($name in @('hooks','cp_hooks')){
    & $Zig clang -cc1as -triple armv7-unknown-linux-musleabihf -target-cpu cortex-a7 -filetype obj -o "$name.o" "$name.S"
    if($LASTEXITCODE){throw "Payload assembly failed: $name"}
  }
  & $Zig ld.lld -m armelf_linux_eabi -T payload.ld -o payload.elf hooks.o probe.o
  if($LASTEXITCODE){throw 'iAP2 payload link failed'}
  & $Zig ld.lld -m armelf_linux_eabi -T cp_payload.ld -o cp_payload.elf cp_hooks.o cp_probe.o
  if($LASTEXITCODE){throw 'CarPlay payload link failed'}
  & $Zig cc @target -static -Os -s -ffunction-sections -fdata-sections '-Wl,--gc-sections' -o u6diag.cgi u6diag.c
  if($LASTEXITCODE){throw 'Diagnostic CGI build failed'}
  foreach($binary in @(@('u6hud_ble','u6hud_ble.c'),@('gate_test','test_gate.c'))){
    & $Zig cc @target -static -Oz -ffunction-sections -fdata-sections '-Wl,--gc-sections' -o $binary[0] $binary[1] ../ble-r18/core_adapter.c ../ble-r18/core/vendor/rgd/rgd_tlv.c
    if($LASTEXITCODE){throw "ARM build failed: $($binary[0])"}
  }
  & $Python -B pack.py
  if($LASTEXITCODE){throw 'Native ELF patch validation failed'}
} finally {Pop-Location}
