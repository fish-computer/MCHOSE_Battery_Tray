# Builds MchoseTray.exe with the MSVC toolchain that ships with Visual Studio.
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1
param(
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root
try {
    $vcvars = $null
    $candidates = @()
    if ($env:VSINSTALLDIR) { $candidates += (Join-Path $env:VSINSTALLDIR "VC\Auxiliary\Build\vcvars64.bat") }
    $candidates += Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
    $candidates += Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { $vcvars = $c; break } }
    if (-not $vcvars) { throw "vcvars64.bat not found. Install the 'Desktop development with C++' workload." }

    New-Item -ItemType Directory -Force -Path (Join-Path $root "obj") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $root $OutDir) | Out-Null

    $sources = (Get-ChildItem (Join-Path $root "src") -Filter *.cpp | ForEach-Object { "src\" + $_.Name }) -join " "
    $exe = Join-Path (Join-Path $root $OutDir) "MchoseTray.exe"

    $bat = @"
@echo off
call "$vcvars" >nul 2>&1
rc /nologo /fo "obj\app.res" "src\app.rc" || exit /b 1
cl /nologo /EHsc /std:c++17 /utf-8 /O2 /MT /W3 /DUNICODE /D_UNICODE /Fo:"obj\\" /Fd:"obj\\" $sources /Fe:"$exe" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"src\app.manifest" "obj\app.res" user32.lib gdiplus.lib gdi32.lib shell32.lib setupapi.lib hid.lib ole32.lib advapi32.lib comctl32.lib uuid.lib
"@
    $batPath = Join-Path $root "obj\build.bat"
    Set-Content -Path $batPath -Value $bat -Encoding ASCII

    Write-Host "Building..." -ForegroundColor Cyan
    & cmd.exe /c $batPath
    if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }

    $size = (Get-Item $exe).Length
    Write-Host ("OK  {0}  ({1:N0} bytes)" -f $exe, $size) -ForegroundColor Green
}
finally {
    Pop-Location
}
