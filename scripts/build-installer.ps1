# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version = '1.10.1',
    [string]$MsysRoot = 'C:\msys64',
    [string]$Iscc
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$release = Join-Path $root 'build-offline\release'
$run = Join-Path $release ([Guid]::NewGuid().ToString('N'))
$stage = Join-Path $run 'payload'
$launcher = Join-Path $run 'launcher'
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'

if (-not $Iscc) {
    $Iscc = @(
        (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
}
if (-not $Iscc) { throw 'Inno Setup 6.6 or newer is required. Pass -Iscc if installed in a custom location.' }
$Iscc = (Resolve-Path -LiteralPath $Iscc).Path
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) { throw "MSYS2 bash not found at $bash" }

New-Item -ItemType Directory -Path $stage, $launcher -Force | Out-Null
Push-Location -LiteralPath $root
try {
    $env:MSYSTEM = 'MINGW64'
    $env:CHERE_INVOKING = '1'
    $env:PS5LINK_RELEASE_STAGE = $stage
    & $bash -lc 'set -e; stage=$(cygpath -u "$PS5LINK_RELEASE_STAGE"); build=build-offline/release/client; cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS5LINK_BUILD=ON -DCHIAKI_ENABLE_CLI=OFF -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF -DCHIAKI_ENABLE_FFMPEG_DECODER=ON -DCHIAKI_USE_SYSTEM_CURL=OFF -DBUILD_SHARED_LIBS=OFF; cmake --build "$build" --target chiaki chiaki-unit ps5link-receiver-test; ctest --test-dir "$build" --output-on-failure; bash scripts/deploy-windows-msys2.sh "$stage" "$build/gui/PS5Link.exe" "$PWD/$build/third-party/cpp-steam-tools" /mingw64 gui/src/qml; bash scripts/collect-windows-notices.sh "$stage"'
    if ($LASTEXITCODE -ne 0) { throw 'Client build, tests, or staging failed.' }

    dotnet publish gui/launcher/PS5Link.Launcher.csproj -c Release -o $launcher --nologo "-p:Version=$Version" -p:DebugType=none -p:DebugSymbols=false
    if ($LASTEXITCODE -ne 0) { throw 'Launcher publish failed.' }

    $launcherNotices = Join-Path $stage 'LICENSES\AvaloniaLauncher'
    New-Item -ItemType Directory -Path $launcherNotices -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $launcher 'PS5LinkLauncher.exe') -Destination $stage
    Copy-Item -Path (Join-Path $launcher 'licenses\*.txt') -Destination $launcherNotices
    Copy-Item -LiteralPath 'COPYING', 'LICENSES\AGPL-3.0-only-OpenSSL.txt' -Destination (Join-Path $stage 'LICENSES')

    & $Iscc "/DPackageDir=$stage" "/DPackageVersion=$Version" "/O$release" (Join-Path $root 'installer\PS5Link.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }

    $file = Join-Path $release "PS5Link-$Version-win-x64-setup.exe"
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $(Split-Path $file -Leaf)" | Set-Content -LiteralPath "$file.sha256" -Encoding ascii
    Write-Output "Installer: $file"
    Write-Output "Staged files: $stage"
} finally {
    Remove-Item Env:PS5LINK_RELEASE_STAGE -ErrorAction SilentlyContinue
    Pop-Location
}
