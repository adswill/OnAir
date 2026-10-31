# OnAir voice codec build helper for Windows (PowerShell).
# Run it yourself:  powershell -ExecutionPolicy Bypass -File tools\codec-builder\build-codecs.ps1
# It asks which plug-ins you want, shows a patent/licence notice, and fetches the upstream source ONLY after you type "yes".
# OnAir (the app and the project) hosts and ships no codec binary and no codec source. See docs/CODECS-BUILD-HELPER.md.
# Prerequisite: MSYS2 with the MinGW-w64 toolchain (see the docs page). This script installs nothing.

$ErrorActionPreference = 'Stop'

$RepoUrl = 'https://github.com/arancormonk/mbelib-neo'
# Pinned upstream commit (mbelib-neo 2.6.2, GPL-2.0-or-later). The script refuses to build any other commit.
$PinCommit = 'a3c7bbdae9eb21cbfab57006c1a5daca7bdcba7f'

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Wrapper = Join-Path $Here 'onair_codec_wrapper.c'
$OutDir = Join-Path $env:LOCALAPPDATA 'OnAir\codecs'

function Stop-Here($msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

Write-Host 'OnAir voice codec build helper'
Write-Host '=============================='
Write-Host ''
Write-Host 'Plug-ins you can build:'
Write-Host '  tetra  TETRA voice (unencrypted calls only)'
Write-Host '  ambe2  AMBE+2 for DMR, NXDN, dPMR'
Write-Host '  ambe   AMBE for D-STAR'
Write-Host '  imbe   IMBE for P25'
Write-Host '  ysf    Yaesu System Fusion (only the 7-byte V/D mode 2 frames)'
Write-Host ''
$choice = Read-Host 'Which ones? Type ids separated by spaces, or "all"'
if ($choice.Trim() -eq 'all') { $choice = 'tetra ambe2 ambe imbe ysf' }
$ids = @($choice -split '\s+' | Where-Object { $_ -ne '' })
foreach ($id in $ids) {
    if (@('tetra', 'ambe2', 'ambe', 'imbe', 'ysf') -notcontains $id) { Stop-Here "unknown plug-in id: $id" }
}
if ($ids.Count -eq 0) { Stop-Here 'nothing chosen' }

Write-Host ''
Write-Host 'IMPORTANT, please read.'
Write-Host ' - These speech codecs (TETRA ACELP, IMBE, AMBE, AMBE+2) may be covered by patents in some countries.'
Write-Host ' - Building and using them is your own choice and your own responsibility. Check the rules where you live,'
Write-Host '   including the rules on listening to radio traffic. OnAir never decodes encrypted calls.'
Write-Host ' - OnAir does not include, download or link these codecs, and the OnAir project hosts no codec files.'
Write-Host " - If you continue, THIS script (running on your computer, at your request) will download the source of mbelib-neo from"
Write-Host "   $RepoUrl"
Write-Host "   at commit $PinCommit. That source is GPL-2.0-or-later: you get it from its authors under their licence."
Write-Host ' - It will then compile it on your computer and copy the plug-in files into:'
Write-Host "   $OutDir"
Write-Host (' - Chosen plug-ins: ' + ($ids -join ' '))
Write-Host ''
$answer = Read-Host 'Type yes to continue (anything else stops here, nothing is downloaded)'
if ($answer -ne 'yes') {
    Write-Host 'Stopped. Nothing was downloaded.'
    exit 0
}

# Tools: look in PATH, then in the usual MSYS2 folders.
foreach ($d in @('C:\msys64\mingw64\bin', 'C:\msys64\usr\bin')) {
    if ((Test-Path $d) -and ($env:PATH -notlike "*$d*")) { $env:PATH = "$d;$env:PATH" }
}
$missing = @()
foreach ($t in @('git', 'cmake', 'gcc', 'mingw32-make')) {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { $missing += $t }
}
if ($missing.Count -gt 0) {
    Write-Host ('Missing tools: ' + ($missing -join ', ')) -ForegroundColor Yellow
    Write-Host 'Install MSYS2 from https://www.msys2.org, open the "MSYS2 MINGW64" shell and run:'
    Write-Host '  pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make git'
    Write-Host 'Nothing was installed or downloaded. Run this script again afterwards.'
    exit 1
}
if (-not (Test-Path $Wrapper)) { Stop-Here "onair_codec_wrapper.c not found next to this script ($Here). Use the whole tools\codec-builder folder." }

$Work = Join-Path $env:TEMP ('onair-codecs-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $Work | Out-Null
Write-Host "Work folder: $Work (you can delete it afterwards)"
$Src = Join-Path $Work 'src'
$Build = Join-Path $Work 'build'

function Run-Checked($exe, $argList, $what) {
    & $exe @argList
    if ($LASTEXITCODE -ne 0) { Stop-Here "$what failed" }
}

Write-Host 'Fetching mbelib-neo source ...'
Run-Checked 'git' @('clone', '--quiet', $RepoUrl, $Src) 'download'
Run-Checked 'git' @('-C', $Src, 'checkout', '--quiet', $PinCommit) 'checkout of the pinned commit'
$head = (& git -C $Src rev-parse HEAD).Trim()
if ($head -ne $PinCommit) { Stop-Here 'commit check failed' }

Write-Host 'Building mbelib-neo ...'
Run-Checked 'cmake' @('-S', $Src, '-B', $Build, '-G', 'MinGW Makefiles', '-DCMAKE_BUILD_TYPE=Release',
    '-DMBELIB_BUILD_TESTS=OFF', '-DMBELIB_BUILD_EXAMPLES=OFF', '-DMBELIB_WARNINGS_AS_ERRORS=OFF') 'cmake configure'
Run-Checked 'cmake' @('--build', $Build) 'build'

$lib = Get-ChildItem -Path $Build -Recurse -Filter 'libmbe*static*.a' | Select-Object -First 1
if (-not $lib) { $lib = Get-ChildItem -Path $Build -Recurse -Filter 'libmbe*.a' | Select-Object -First 1 }
if (-not $lib) { Stop-Here "static library not found in $Build" }
$ver = Get-ChildItem -Path $Build -Recurse -Filter 'version.h' | Where-Object { $_.FullName -like '*mbelib-neo*' } | Select-Object -First 1
if (-not $ver) { Stop-Here "generated version.h not found in $Build" }
$GenInc = Split-Path -Parent (Split-Path -Parent $ver.FullName)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$done = @()
foreach ($id in $ids) {
    $out = Join-Path $Work "onair-codec-$id.dll"
    Write-Host "Building plug-in $id ..."
    & gcc -shared -O2 ("-DCODEC_" + $id.ToUpper()) ("-I" + (Join-Path $Src 'include')) ("-I" + $GenInc) $Wrapper $lib.FullName -lm -o $out
    if ($LASTEXITCODE -eq 0) {
        Copy-Item $out $OutDir -Force
        $done += $id
    } else {
        Write-Host "  plug-in $id failed (see above)" -ForegroundColor Yellow
    }
}

Write-Host ''
if ($done.Count -gt 0) {
    Write-Host ("Installed into ${OutDir}: " + ($done -join ' '))
    Write-Host 'Next: open OnAir, Help > Voice codecs, press Rescan, and accept the notice for each codec.'
} else {
    Write-Host 'No plug-in was built.'
}
Write-Host "You can delete $Work now."
