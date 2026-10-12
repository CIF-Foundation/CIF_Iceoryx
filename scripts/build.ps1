# Build iox2_cif.dll (x64) and run the C smoke test.
# Requires: CMake, git, Rust (MSVC), Visual Studio C++ tools.

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

function Find-VcVars64 {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($install) {
            $vcvars = Join-Path $install "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $vcvars) { return $vcvars }
        }
    }
    $candidates = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    throw "Could not find vcvars64.bat. Install Visual Studio C++ tools."
}

$iox2 = Join-Path $Root "third_party\iceoryx2"
if (-not (Test-Path (Join-Path $iox2 "CMakeLists.txt"))) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "third_party") | Out-Null
    git clone --depth 1 https://github.com/eclipse-iceoryx/iceoryx2.git $iox2
}

$vcvars = Find-VcVars64
Write-Host "Using $vcvars"

$version = (Get-Content (Join-Path $Root "VERSION") -Raw).Trim()
$dllName = "iox2_cif-windows-$version.dll"
$buildDir = Join-Path $Root "build"
$distDir = Join-Path $Root "dist\windows"
New-Item -ItemType Directory -Force -Path $buildDir, $distDir | Out-Null

$bat = Join-Path $buildDir "_build.bat"
@(
    "@echo on"
    "call `"$vcvars`""
    "if errorlevel 1 exit /b 1"
    "cmake -S `"$Root`" -B `"$buildDir`" -A x64"
    "if errorlevel 1 exit /b 1"
    "cmake --build `"$buildDir`" --config Release --target iox2_cif --target iox2_cif_smoke"
    "if errorlevel 1 exit /b 1"
) | Set-Content -Path $bat -Encoding ASCII

& cmd.exe /c $bat
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit $LASTEXITCODE" }

$dll = Get-ChildItem -Path $buildDir -Recurse -Filter iox2_cif.dll | Where-Object { $_.FullName -match "Release|bin" } | Select-Object -First 1
if (-not $dll) {
    $dll = Get-ChildItem -Path $buildDir -Recurse -Filter iox2_cif.dll | Select-Object -First 1
}
$smoke = Get-ChildItem -Path $buildDir -Recurse -Filter iox2_cif_smoke.exe | Select-Object -First 1
if (-not $dll) { throw "iox2_cif.dll not found under $buildDir" }

Copy-Item $dll.FullName (Join-Path $distDir $dllName) -Force
$staleFfi = Join-Path $distDir "iceoryx2_ffi_c.dll"
if (Test-Path $staleFfi) {
    Remove-Item $staleFfi -Force -ErrorAction SilentlyContinue
}
Copy-Item (Join-Path $Root "native\include\iox2_cif.h") (Join-Path $distDir "iox2_cif.h") -Force

Write-Host "DLL: $($dll.FullName)"
Write-Host "Copied to: $distDir\$dllName"

if ($smoke) {
    Write-Host "Running smoke test..."
    Push-Location $smoke.DirectoryName
    try {
        & $smoke.FullName
        if ($LASTEXITCODE -ne 0) { throw "Smoke test failed with exit $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
    Write-Host "Smoke test passed."
}

Write-Host "Done. Installer will symlink iox2_cif.dll -> $dllName"
Write-Host "  $distDir\$dllName"
