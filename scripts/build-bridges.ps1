# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
param(
    [switch]$Strict
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$offline = Join-Path $root "build-offline"
$dist = Join-Path $offline "PS5Link-Win"
if (-not (Test-Path $dist)) {
    New-Item -ItemType Directory -Path $dist -Force | Out-Null
}

function Find-VcVars64 {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($installPath) {
            $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    $fallbacks = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($path in $fallbacks) {
        if (Test-Path $path) { return $path }
    }
    return $null
}

$vcvars = Find-VcVars64
if (-not $vcvars) {
    $msg = "Visual Studio C++ x64 build tools (vcvars64.bat) not found."
    if ($Strict) { throw $msg }
    Write-Warning $msg
    exit 0
}

function Invoke-ClBridge {
    param(
        [string]$Name,
        [string]$Command
    )
    Write-Host "  Building $Name..." -ForegroundColor DarkCyan
    $fullCmd = "call `"$vcvars`" >nul && $Command"
    & cmd.exe /d /c $fullCmd
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to build $Name (exit code $LASTEXITCODE)"
    }
}

$rtxInc = Join-Path $offline "rtx-video-sdk\include"
$rtxLib = Join-Path $offline "rtx-video-sdk\lib\Windows\x64\nvsdk_ngx_d.lib"
$dlssInc = Join-Path $offline "dlss-sdk\include"
$dlssLib = Join-Path $offline "dlss-sdk\nvsdk_ngx_d.lib"
$ffxApiInc = Join-Path $offline "ffx-sdk\api\include"
$ffxFgInc = Join-Path $offline "ffx-sdk\framegeneration\include"

# 1. RTX VSR bridge
if ((Test-Path $rtxInc) -and (Test-Path $rtxLib)) {
    $src = Join-Path $root "gui\src\rtxvideo_bridge.cpp"
    $obj = Join-Path $offline "rtxvideo_bridge.obj"
    $out = Join-Path $dist "PS5LinkVsr.dll"
    $imp = Join-Path $offline "PS5LinkVsr.lib"
    Invoke-ClBridge "PS5LinkVsr.dll" "cl /nologo /std:c++17 /O2 /MD /EHsc /LD /I`"$rtxInc`" /Fo`"$obj`" `"$src`" `"$rtxLib`" /link advapi32.lib ole32.lib user32.lib /OUT:`"$out`" /IMPLIB:`"$imp`""
    Copy-Item $out (Join-Path $offline "PS5LinkVsr.dll") -Force
    $rtxBin = Join-Path $offline "rtx-video-sdk\bin\Windows\x64\rel"
    foreach ($dll in @("nvngx_vsr.dll", "nvngx_truehdr.dll")) {
        $dllPath = Join-Path $rtxBin $dll
        if (Test-Path $dllPath) {
            Copy-Item $dllPath (Join-Path $dist $dll) -Force
        }
    }
} else {
    Write-Warning "Skipping PS5LinkVsr.dll (RTX Video SDK not found under build-offline\rtx-video-sdk)."
}

# 2. DLSS Super Resolution bridge
if ((Test-Path $dlssInc) -and (Test-Path $dlssLib)) {
    $src = Join-Path $root "gui\src\dlsssr_bridge.cpp"
    $obj = Join-Path $offline "dlsssr_bridge.obj"
    $out = Join-Path $dist "PS5LinkDlssSr.dll"
    $imp = Join-Path $offline "PS5LinkDlssSr.lib"
    Invoke-ClBridge "PS5LinkDlssSr.dll" "cl /nologo /std:c++17 /O2 /MD /EHsc /LD /I`"$dlssInc`" /Fo`"$obj`" `"$src`" `"$dlssLib`" /link advapi32.lib ole32.lib user32.lib /OUT:`"$out`" /IMPLIB:`"$imp`""
} else {
    Write-Warning "Skipping PS5LinkDlssSr.dll (DLSS SDK not found under build-offline\dlss-sdk)."
}

# 3. DLSS Frame Generation bridge
if ((Test-Path $dlssInc) -and (Test-Path $rtxLib)) {
    $src = Join-Path $root "gui\src\dlssfg_bridge.cpp"
    $obj = Join-Path $offline "dlssfg_bridge.obj"
    $out = Join-Path $dist "PS5LinkDlssFg.dll"
    $imp = Join-Path $offline "PS5LinkDlssFg.lib"
    Invoke-ClBridge "PS5LinkDlssFg.dll" "cl /nologo /std:c++17 /O2 /MD /EHsc /LD /I`"$dlssInc`" /Fo`"$obj`" `"$src`" `"$rtxLib`" /link advapi32.lib ole32.lib user32.lib /OUT:`"$out`" /IMPLIB:`"$imp`""
} else {
    Write-Warning "Skipping PS5LinkDlssFg.dll (DLSS/RTX NGX SDK not found)."
}

# 4. AMD FidelityFX FSR 3.1 Frame Generation bridge
if ((Test-Path $ffxApiInc) -and (Test-Path $ffxFgInc)) {
    $src = Join-Path $root "gui\src\fsrfg_bridge.cpp"
    $obj = Join-Path $offline "fsrfg_bridge.obj"
    $out = Join-Path $dist "PS5LinkFsrFg.dll"
    $imp = Join-Path $offline "PS5LinkFsrFg.lib"
    Invoke-ClBridge "PS5LinkFsrFg.dll" "cl /nologo /std:c++17 /O2 /MD /EHsc /LD /I`"$ffxApiInc`" /I`"$ffxFgInc`" /Fo`"$obj`" `"$src`" /link d3d12.lib /OUT:`"$out`" /IMPLIB:`"$imp`""
    $ffxSignedBin = Join-Path $offline "ffx-sdk\signedbin"
    $fsrFgDist = Join-Path $dist "fsrfg"
    New-Item -ItemType Directory -Path $fsrFgDist -Force | Out-Null
    foreach ($dll in @("amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll")) {
        $dllPath = Join-Path $ffxSignedBin $dll
        if (Test-Path $dllPath) {
            Copy-Item $dllPath (Join-Path $fsrFgDist $dll) -Force
        }
    }
} else {
    Write-Warning "Skipping PS5LinkFsrFg.dll (AMD FidelityFX SDK not found under build-offline\ffx-sdk)."
}
