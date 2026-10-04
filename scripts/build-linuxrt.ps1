# Sync sources to NI Linux RT and build libiox2_cif.so natively on the target.
# Default host: RT_NUC (192.168.1.160). Override with -Target.
param(
    [string]$Target = "RT_NUC",
    [string]$RemoteHome = "/home/admin",
    [string]$RemoteRoot = "/home/admin/src/iox2_cifwrapper"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

function Invoke-Rt([string]$RemoteCommand) {
    ssh -o ConnectTimeout=15 $Target "export HOME=$RemoteHome USER=admin; $RemoteCommand"
    if ($LASTEXITCODE -ne 0) {
        throw "Remote command failed ($LASTEXITCODE): $RemoteCommand"
    }
}

Write-Host "Packing sources..."
$tar = Join-Path $env:TEMP "iox2_cif_linuxrt.tar"
if (Test-Path $tar) { Remove-Item $tar -Force }
Push-Location $Root
try {
    tar -cf $tar CMakeLists.txt native scripts
} finally {
    Pop-Location
}

Write-Host "Uploading to ${Target}:$RemoteRoot ..."
Invoke-Rt "mkdir -p $RemoteRoot"
# scp the tarball then extract so Windows path separators are not an issue
scp -o ConnectTimeout=15 $tar "${Target}:/tmp/iox2_cif_linuxrt.tar"
Invoke-Rt "mkdir -p $RemoteRoot && tar -xf /tmp/iox2_cif_linuxrt.tar -C $RemoteRoot && rm -f /tmp/iox2_cif_linuxrt.tar && find $RemoteRoot/scripts -name '*.sh' -exec sed -i 's/\r$//' {} \; && chmod +x $RemoteRoot/scripts/build-linuxrt.sh"

Write-Host "Building on $Target (first iceoryx2/Rust compile can take a while)..."
Invoke-Rt "cd $RemoteRoot && bash scripts/build-linuxrt.sh"

$distDir = Join-Path $Root "dist"
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Write-Host "Copying libiox2_cif.so back to dist..."
scp -o ConnectTimeout=15 "${Target}:${RemoteRoot}/dist/libiox2_cif.so" (Join-Path $distDir "libiox2_cif.so")
scp -o ConnectTimeout=15 "${Target}:${RemoteRoot}/dist/iox2_cif.h" (Join-Path $distDir "iox2_cif.h")

Write-Host "Done. LabVIEW RT Call Library Function library:"
Write-Host "  $(Join-Path $distDir 'libiox2_cif.so')"
Write-Host "On the target it is also at:"
Write-Host "  $RemoteRoot/dist/libiox2_cif.so"
