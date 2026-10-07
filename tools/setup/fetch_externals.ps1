<#
.SYNOPSIS
    Downloads the third-party libraries castlemist builds against into external/.

.DESCRIPTION
    Every library lands in the exact folder name cmake/Externals.cmake checks
    for, at the version external/README.md pins. Anything already present is
    left alone, so it is safe to re-run after a partial download.

    bink2-2.7d/ and jpegcfg/ are tracked in git and need nothing.

    -WithBgfx also clones the bgfx / bx / bimg trio at the commits Guild Wars 2
    itself ships (needs git). Without them castlemist still builds; only the
    optional "Game 1:1" render mode is hidden.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\setup\fetch_externals.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\setup\fetch_externals.ps1 -WithBgfx
#>
param(
    [string]$Dest = (Join-Path $PSScriptRoot '..\..\external'),
    [switch]$WithBgfx
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is ~10x faster without the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Dest = [IO.Path]::GetFullPath($Dest)
New-Item -ItemType Directory -Force $Dest | Out-Null
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("castlemist-ext-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null

# A zip whose single top-level folder becomes external/<Folder>.
function Get-Zip([string]$Folder, [string]$Witness, [string]$Url) {
    $target = Join-Path $Dest $Folder
    if (Test-Path (Join-Path $target $Witness)) { Write-Host "  ok       $Folder"; return }
    Write-Host "  fetching $Folder"
    $zip = Join-Path $tmp ($Folder + '.zip')
    Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
    $unz = Join-Path $tmp $Folder
    Expand-Archive -Path $zip -DestinationPath $unz -Force
    $top = Get-ChildItem $unz | Select-Object -First 1
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    Move-Item $top.FullName $target
    if (-not (Test-Path (Join-Path $target $Witness))) { throw "$Folder downloaded but $Witness is missing" }
}

# Single files straight into external/<Folder>.
function Get-Files([string]$Folder, [string]$BaseUrl, [string[]]$Files) {
    $target = Join-Path $Dest $Folder
    if (Test-Path (Join-Path $target $Files[0])) { Write-Host "  ok       $Folder"; return }
    Write-Host "  fetching $Folder"
    New-Item -ItemType Directory -Force $target | Out-Null
    foreach ($f in $Files) { Invoke-WebRequest -Uri "$BaseUrl/$f" -OutFile (Join-Path $target $f) -UseBasicParsing }
}

# A git checkout of one exact commit (shallow).
function Get-Commit([string]$Folder, [string]$Repo, [string]$Sha) {
    $target = Join-Path $Dest $Folder
    if (Test-Path (Join-Path $target 'src')) { Write-Host "  ok       $Folder"; return }
    Write-Host "  cloning  $Folder @ $($Sha.Substring(0, 10))"
    git init -q $target
    # bgfx's 3rdparty/ tree has paths past Windows' 260-character limit.
    git -C $target config core.longpaths true
    git -C $target fetch -q --depth 1 $Repo $Sha
    git -C $target checkout -q FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw "checking out $Folder failed" }
}

Write-Host "Fetching castlemist's libraries into $Dest"
try {
    Get-Zip 'nlohmann-json' 'single_include\nlohmann\json.hpp' 'https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.zip'
    Get-Zip 'stb-master' 'stb_image.h' 'https://github.com/nothings/stb/archive/refs/heads/master.zip'
    Get-Zip 'dr_libs-master' 'dr_mp3.h' 'https://github.com/mackron/dr_libs/archive/refs/heads/master.zip'
    Get-Zip 'glm-1.0.3' 'glm\glm.hpp' 'https://github.com/g-truc/glm/archive/refs/tags/1.0.3.zip'
    Get-Zip 'libjpeg-turbo-3.2.0' 'src\jpeglib.h' 'https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.2.0.zip'
    Get-Zip 'libwebp-1.6.0' 'src\webp\decode.h' 'https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.zip'
    Get-Zip 'sqlite3' 'sqlite3.c' 'https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip'
    Get-Files 'xatlas' 'https://raw.githubusercontent.com/jpcy/xatlas/f700c7790aaa030e794b52ba7791a05c085faf0c/source/xatlas' @('xatlas.h', 'xatlas.cpp')

    if ($WithBgfx) {
        if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw '-WithBgfx needs git on PATH' }
        Get-Commit 'bgfx' 'https://github.com/bkaradzic/bgfx' 'a476c5b9a42d3779af59a0099d4d222fa8898d36'
        Get-Commit 'bx'   'https://github.com/bkaradzic/bx'   'e7ede513dc8b90386960587e348c73b241f7735d'
        Get-Commit 'bimg' 'https://github.com/bkaradzic/bimg' '2afa64c14c1e3dd5d28412ee03bee0dfe7242f03'
    }
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
Write-Host 'Done. Next: cmake --preset debug'
