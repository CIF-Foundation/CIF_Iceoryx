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
    tar -cf $tar CMakeLists.txt native scripts VERSION
} finally {
    Pop-Location
}

Write-Host "Uploading to ${Target}:$RemoteRoot ..."
Invoke-Rt "mkdir -p $RemoteRoot"
# scp the tarball then extract so Windows path separators are not an issue
scp -o ConnectTimeout=15 $tar "${Target}:/tmp/iox2_cif_linuxrt.tar"
Invoke-Rt "mkdir -p $RemoteRoot && tar -xf /tmp/iox2_cif_linuxrt.tar -C $RemoteRoot && rm -f /tmp/iox2_cif_linuxrt.tar && find $RemoteRoot/scripts -name '*.sh' -exec sed -i 's/\r$//' {} \; && chmod +x $RemoteRoot/scripts/build-linuxrt.sh"

Write-Host "Building on $Target (core2, no AVX; first iceoryx2/Rust compile can take a while)..."
# Atom cRIO (e.g. 9057 E3805) has no AVX. Force a clean configure so CFLAGS apply.
Invoke-Rt "cd $RemoteRoot && rm -rf build-linuxrt && export CFLAGS='-O2 -fPIC -march=core2' CXXFLAGS='-O2 -fPIC -march=core2' RUSTFLAGS='-C relocation-model=pic -C target-cpu=core2' && bash scripts/build-linuxrt.sh"

$version = (Get-Content (Join-Path $Root "VERSION") -Raw).Trim()
$soName = "libiox2_cif-linuxrt.so.$version"
$distDir = Join-Path $Root "dist\linuxrt"
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Write-Host "Copying $soName back to dist\linuxrt..."
scp -o ConnectTimeout=15 "${Target}:${RemoteRoot}/dist/linuxrt/$soName" (Join-Path $distDir $soName)
scp -o ConnectTimeout=15 "${Target}:${RemoteRoot}/dist/linuxrt/iox2_cif.h" (Join-Path $distDir "iox2_cif.h")

Write-Host "Done. LabVIEW RT library (installer symlinks libiox2_cif.so):"
Write-Host "  $(Join-Path $distDir $soName)"
Write-Host "On the target it is also at:"
Write-Host "  $RemoteRoot/dist/linuxrt/$soName"
