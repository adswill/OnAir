# OnAir laptop worker uninstall (embedded in OnAir-Laptop-Uninstall.cmd by build.py). ASCII only.
$ErrorActionPreference = 'Continue'
$env:WSL_UTF8 = '1'
$Distro   = 'Ubuntu-24.04'
$DataDir  = Join-Path $env:ProgramData 'OnAir'
$LogFile  = Join-Path $env:USERPROFILE 'OnAir-Laptop-Setup.log'

function Log([string]$m) { try { Add-Content -Path $LogFile -Value ((Get-Date -Format 's') + ' [uninstall] ' + $m) -Encoding UTF8 } catch { } }
function Say([string]$m)  { Write-Host ''; Write-Host ('==> ' + $m) -ForegroundColor Cyan; Log $m }
function Info([string]$m) { Write-Host ('    ' + $m); Log $m }
function Run-Quiet([string]$exe, [string[]]$a) {
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { & $exe @a 2>&1 | ForEach-Object { Log (('' + $_) -replace "`0", '') }; return $LASTEXITCODE } finally { $ErrorActionPreference = $old }
}
function Distro-Present {
    try {
        $o = ((& wsl.exe -l -q 2>$null) -join "`n") -replace "`0", ''
        foreach ($l in ($o -split "`n")) { if ($l.Trim() -eq $Distro) { return $true } }
    } catch { }
    return $false
}

function Main {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Write-Host 'Windows will now ask for permission once. Please click Yes.' -ForegroundColor Yellow
        try { Start-Process -FilePath 'powershell.exe' -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '"') -Verb RunAs } catch { throw (New-Object System.Exception('Permission was not given, nothing was changed.')) }
        return 'elevated'
    }
    Write-Host ''
    Write-Host '  OnAir laptop worker: uninstall' -ForegroundColor Green
    Log 'uninstall started'

    Say 'Step 1: removing the scheduled tasks'
    foreach ($t in @('OnAir-Laptop-Keepalive', 'OnAir-Laptop-Setup-Resume')) {
        try { Unregister-ScheduledTask -TaskName $t -Confirm:$false -ErrorAction SilentlyContinue } catch { }
    }
    foreach ($dir in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Startup'))) {
        if ($dir) { Remove-Item -Path (Join-Path $dir 'OnAir Monitor.lnk') -Force -ErrorAction SilentlyContinue }
    }
    Info 'Done (the OnAir Monitor shortcuts are removed too).'

    Say 'Step 2: stopping the runner'
    if (Distro-Present) {
        $cmd = 'cd /home/onair/actions-runner 2>/dev/null && { ./svc.sh stop; ./svc.sh uninstall; } ; true'
        [void](Run-Quiet 'wsl.exe' @('-d', $Distro, '-u', 'root', '-e', 'bash', '-c', $cmd))
        Info 'The runner service is stopped and removed.'
        Info 'Note: GitHub still lists "onair-laptop" as offline. Remove it at github.com/adswill/OnAir-ci > Settings > Actions > Runners.'
    } else { Info 'Ubuntu 24.04 is not installed, nothing to stop.' }

    Say 'Step 3: Ubuntu 24.04'
    if (Distro-Present) {
        Write-Host '    Delete the whole Ubuntu 24.04 installation (all files inside it, the build cache, the runner)? [y/N] ' -NoNewline -ForegroundColor Yellow
        $a = Read-Host
        if ($a -match '^(y|yes)$') {
            [void](Run-Quiet 'wsl.exe' @('--unregister', $Distro))
            Info 'Ubuntu 24.04 was deleted.'
        } else { Info 'Ubuntu 24.04 was kept.' }
    }

    Say 'Step 4: .wslconfig and sleep setting'
    $cfg = Join-Path $env:USERPROFILE '.wslconfig'
    if ((Test-Path $cfg) -and ((Get-Content $cfg -Raw) -like '*# OnAir-Laptop-Setup*')) {
        if (Test-Path ($cfg + '.onair-backup')) { Move-Item ($cfg + '.onair-backup') $cfg -Force; Info 'Your old .wslconfig was put back.' }
        else { Remove-Item $cfg -Force; Info '.wslconfig removed.' }
        [void](Run-Quiet 'wsl.exe' @('--shutdown'))
    } else { Info '.wslconfig was not made by OnAir, left alone.' }
    $prev = Join-Path $DataDir 'prev-standby-ac-min.txt'
    if (Test-Path $prev) {
        $m = (Get-Content $prev -Raw).Trim()
        if ($m -match '^\d+$') { [void](Run-Quiet 'powercfg.exe' @('/change', 'standby-timeout-ac', $m)); Info ('Sleep while plugged in is back to ' + $m + ' minutes (0 = never).') }
    }
    if (Test-Path $DataDir) { Remove-Item $DataDir -Recurse -Force -ErrorAction SilentlyContinue }
    Write-Host ''
    Write-Host '  Uninstall finished. WSL itself and the Windows features were left on (other programs may use them).' -ForegroundColor Green
    return 'done'
}

$r = ''
try { $r = @(Main)[-1] } catch {
    Write-Host ('  PROBLEM: ' + $_.Exception.Message) -ForegroundColor Red
    Log ('ERROR ' + $_.Exception.Message)
}
if ($r -ne 'elevated') { Write-Host ''; [void](Read-Host 'Press Enter to close this window') }
