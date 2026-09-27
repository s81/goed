# Builds goed.exe: unit tests -> tcc -> embed icon. Stops at the first failure.
#   pwsh -File build.ps1
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function Step($name, [scriptblock]$cmd) {
    Write-Host "== $name" -ForegroundColor Cyan
    & $cmd
    if ($LASTEXITCODE) { Write-Host "$name failed" -ForegroundColor Red; exit $LASTEXITCODE }
}

Step 'test'  { tcc -DUNICODE -luser32 -lgdi32 -L. -lcomdlg32 -run test_goed.c }
Step 'build' { tcc -DUNICODE -Wall goed.c -luser32 -lgdi32 -L. -lcomdlg32 '-Wl,-subsystem=windows' -o goed.exe }
Step 'icon'  { & "$PSScriptRoot\icon.ps1" }
Write-Host "ok: goed.exe $((Get-Item goed.exe).Length) bytes" -ForegroundColor Green
