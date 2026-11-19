# Fetch the iceoryx2 revision pinned in third_party/iceoryx2.rev.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$RevFile = Join-Path $Root "third_party\iceoryx2.rev"
$Iox2 = Join-Path $Root "third_party\iceoryx2"
$Url = "https://github.com/eclipse-iceoryx/iceoryx2.git"

$Rev = $null
foreach ($line in Get-Content $RevFile) {
    $line = ($line -replace '#.*', '').Trim()
    if ($line) {
        $Rev = $line
        break
    }
}
if (-not $Rev) {
    throw "No revision in $RevFile"
}

$cmake = Join-Path $Iox2 "CMakeLists.txt"
if (Test-Path $cmake) {
    $head = (git -C $Iox2 rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) { $head = $null }
    if ($head) { $head = $head.Trim() }
    if ($head -ne $Rev) {
        $shown = if ($head) { $head } else { "not a git checkout" }
        throw "third_party/iceoryx2 is $shown, pinned revision is $Rev. Remove third_party/iceoryx2 and rebuild."
    }
    Write-Host "iceoryx2 $Rev"
    return
}

Write-Host "Fetching iceoryx2 $Rev"
if (Test-Path $Iox2) { Remove-Item $Iox2 -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Iox2 | Out-Null
git -C $Iox2 init
if ($LASTEXITCODE -ne 0) { throw "git init failed" }
git -C $Iox2 remote add origin $Url
if ($LASTEXITCODE -ne 0) { throw "git remote add failed" }
git -C $Iox2 fetch --depth 1 origin $Rev
if ($LASTEXITCODE -ne 0) { throw "git fetch of $Rev failed" }
git -C $Iox2 checkout --detach FETCH_HEAD
if ($LASTEXITCODE -ne 0) { throw "git checkout of $Rev failed" }
