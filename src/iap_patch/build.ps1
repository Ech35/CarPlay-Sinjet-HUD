param(
    [Parameter(Mandatory = $true)][string]$Zig,
    [Parameter(Mandatory = $true)][string]$Python
)
$ErrorActionPreference = 'Stop'
$target = @('-target', 'arm-linux-musleabihf', '-mcpu=cortex_a7-neon', '-marm', '-g')
$cflags = @(
    '-fno-vectorize', '-fno-slp-vectorize', '-mno-unaligned-access', '-fPIC',
    '-ffreestanding', '-fno-stack-protector', '-fno-unwind-tables',
    '-fno-asynchronous-unwind-tables', '-fno-builtin', '-Oz'
)
Push-Location -LiteralPath $PSScriptRoot
try {
    # Compile separately so Zig retains -fPIC for the payload object.
    & $Zig cc @target @cflags -c -o nav_probe.o nav_probe.c
    if ($LASTEXITCODE) { throw 'Probe compilation failed' }
    & $Zig cc @target -c -o entry.o entry.S
    if ($LASTEXITCODE) { throw 'Entry assembly failed' }
    $linker = '-Wl,-e,probe_outbound,--no-gc-sections,-T,payload.ld'
    & $Zig cc @target -nostdlib -static $linker -o payload.elf entry.o nav_probe.o
    if ($LASTEXITCODE) { throw 'Payload link failed' }
    & $Python patch.py
    if ($LASTEXITCODE) { throw 'ELF patch failed' }
} finally {
    Pop-Location
}
