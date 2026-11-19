# Sync sources to an Ubuntu host and build libiox2_cif.so in Ubuntu 20.04 Docker
# (glibc 2.31). -Target is the SSH destination (host, or user@host).
param(
    [Parameter(Mandatory = $true)]
    [string]$Target,
    [string]$RemoteUser,
    [string]$RemoteRoot = "~/src/iox2_cifwrapper",
    [string]$ImageName = "iox2-cif-ubuntu2004"
)

if ([string]::IsNullOrWhiteSpace($Target)) {
    throw "Pass -Target <ssh-host>. Example: .\scripts\build-ubuntu2004.ps1 -Target user@build-host"
}
$Remote = if ([string]::IsNullOrWhiteSpace($RemoteUser)) { $Target } else { "${RemoteUser}@${Target}" }

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

function Invoke-Ubuntu([string]$RemoteCommand) {
    ssh -o ConnectTimeout=15 $Remote $RemoteCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Remote command failed ($LASTEXITCODE): $RemoteCommand"
    }
}

Write-Host "Packing sources..."
$tar = Join-Path $env:TEMP "iox2_cif_ubuntu2004.tar"
if (Test-Path $tar) { Remove-Item $tar -Force }
Push-Location $Root
try {
    tar -cf $tar CMakeLists.txt native scripts VERSION third_party/iceoryx2.rev
} finally {
    Pop-Location
}

Write-Host "Uploading to ${Remote}:$RemoteRoot ..."
Invoke-Ubuntu "mkdir -p $RemoteRoot"
scp -o ConnectTimeout=15 $tar "${Remote}:/tmp/iox2_cif_ubuntu2004.tar"
Invoke-Ubuntu "mkdir -p $RemoteRoot && tar -xf /tmp/iox2_cif_ubuntu2004.tar -C $RemoteRoot && rm -f /tmp/iox2_cif_ubuntu2004.tar && find $RemoteRoot/scripts -name '*.sh' -exec sed -i 's/\r$//' {} \; && chmod +x $RemoteRoot/scripts/build-ubuntu2004.sh"

Write-Host "Building Docker image $ImageName (first time installs gcc/cmake/rust)..."
Invoke-Ubuntu "docker build -t $ImageName -f $RemoteRoot/scripts/Dockerfile.ubuntu2004 $RemoteRoot/scripts"

Write-Host "Compiling libiox2_cif.so in Ubuntu 20.04 (glibc 2.31)..."
Invoke-Ubuntu "docker run --rm -v ${RemoteRoot}:/src -w /src $ImageName bash scripts/build-ubuntu2004.sh"

$version = (Get-Content (Join-Path $Root "VERSION") -Raw).Trim()
$soName = "libiox2_cif-ubuntu.so.$version"
$distDir = Join-Path $Root "dist\ubuntu"
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Write-Host "Copying $soName back to dist\ubuntu..."
scp -o ConnectTimeout=15 "${Remote}:${RemoteRoot}/dist/ubuntu/$soName" (Join-Path $distDir $soName)
scp -o ConnectTimeout=15 "${Remote}:${RemoteRoot}/dist/ubuntu/iox2_cif.h" (Join-Path $distDir "iox2_cif.h")

Write-Host "Done. Ubuntu 20.04 library (installer symlinks libiox2_cif.so):"
Write-Host "  $(Join-Path $distDir $soName)"
