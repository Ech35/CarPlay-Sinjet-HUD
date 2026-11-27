param([Parameter(Mandatory=$true)][string]$Zig,[string]$Python='python')
$ErrorActionPreference='Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
  & $Zig cc -O2 -shared -o adapter-test.dll core_adapter.c core/vendor/rgd/rgd_tlv.c
  if($LASTEXITCODE){throw 'Core compilation failed'}
  & $Python -B test_adapter.py
  if($LASTEXITCODE){throw 'Synthetic core checks failed'}
} finally { Pop-Location }
