# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
param(
    [switch]$SkipTests,
    [switch]$SkipBridges,
    [switch]$SkipLauncher,
    [string]$MsysRoot = "C:\msys64"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$offline = Join-Path $root "build-offline"
$dist = Join-Path $offline "PS5Link-Win"
$buildDir = Join-Path $offline "client"

$running = Get-Process -Name "PS5Link", "PS5LinkLauncher" -ErrorAction SilentlyContinue
if ($running) {
    $ids = ($running | ForEach-Object { "$($_.ProcessName) (PID $($_.Id))" }) -join ", "
    throw "Close running processes before rebuilding portable files: $ids"
}

Write-Host "[1/4] Building PS5Link client and tests (MSYS2 MINGW64)..." -ForegroundColor Cyan
$env:MSYSTEM = "MINGW64"
$env:CHERE_INVOKING = "1"
$env:PS5LINK_BUILD_ROOT = $root
$bash = Join-Path $MsysRoot "usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
    throw "MSYS2 bash not found at $bash"
}

$targets = if ($SkipTests) { "chiaki" } else { "chiaki chiaki-unit ps5link-receiver-test ps5link-controller-idle-test" }
try {
    & $bash -lc "cd `"`$(cygpath -u `"`$PS5LINK_BUILD_ROOT`")`" && cmake --build build-offline/client --target $targets"
    if ($LASTEXITCODE -ne 0) { throw "Client build failed ($LASTEXITCODE)" }

    if (-not $SkipTests) {
        Write-Host "[2/4] Running CTest..." -ForegroundColor Cyan
        & $bash -lc "cd `"`$(cygpath -u `"`$PS5LINK_BUILD_ROOT`")`" && ctest --test-dir build-offline/client --output-on-failure"
        if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
    }
} finally {
    Remove-Item Env:PS5LINK_BUILD_ROOT -ErrorAction SilentlyContinue
}

if (-not $SkipBridges) {
    Write-Host "[3/4] Building MSVC NVIDIA NGX & AMD FidelityFX bridges..." -ForegroundColor Cyan
    $bridgeScript = Join-Path $PSScriptRoot "build-bridges.ps1"
    if (Test-Path $bridgeScript) {
        & $bridgeScript
        if ($LASTEXITCODE -ne 0) { throw "build-bridges.ps1 failed ($LASTEXITCODE)" }
    } else {
        foreach ($script in @("build-vsr.cmd", "build-dlsssr-bridge.cmd", "build-dlssfg-bridge.cmd", "build-fsrfg-bridge.cmd")) {
            $path = Join-Path $offline $script
            if (Test-Path $path) {
                cmd /c $path
                if ($LASTEXITCODE -ne 0) { throw "$script failed ($LASTEXITCODE)" }
            }
        }
        if (Test-Path (Join-Path $offline "PS5LinkVsr.dll")) {
            Copy-Item (Join-Path $offline "PS5LinkVsr.dll") (Join-Path $dist "PS5LinkVsr.dll") -Force
        }
    }
}

if (-not $SkipLauncher) {
    Write-Host "[4/4] Publishing the Avalonia launcher..." -ForegroundColor Cyan
    $launcherProject = Join-Path $root "gui\launcher\PS5Link.Launcher.csproj"
    $launcherPublish = Join-Path $buildDir "launcher-publish"
    if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
        throw "The .NET 10 SDK is required to build PS5LinkLauncher.exe."
    }
    & dotnet publish $launcherProject --configuration Release --output $launcherPublish --nologo
    if ($LASTEXITCODE -ne 0) { throw "Launcher publish failed ($LASTEXITCODE)" }

    $notices = Join-Path $dist "LICENSES\AvaloniaLauncher"
    New-Item -ItemType Directory -Path $notices -Force | Out-Null
    Copy-Item (Join-Path $launcherPublish "licenses\*.txt") -Destination $notices -Force
    Copy-Item (Join-Path $launcherPublish "PS5LinkLauncher.exe") -Destination $dist -Force
}

New-Item -ItemType Directory -Path $dist -Force | Out-Null
Copy-Item (Join-Path $buildDir "gui\PS5Link.exe") (Join-Path $dist "PS5Link.exe") -Force
Write-Host "Done! Portable build updated in $dist" -ForegroundColor Green
