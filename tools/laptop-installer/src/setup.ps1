# OnAir laptop worker setup (embedded in OnAir-Laptop-Setup.cmd by build.py; edit this file, not the .cmd).
# Sets up WSL2 + Ubuntu 24.04 + build tools + a GitHub Actions self-hosted runner for the PRIVATE repo adswill/OnAir-ci.
# Outbound connections only. No SSH, no open ports, no firewall changes. Safe to run again. ASCII only.
param(
    [string]$SrcDir = '',
    [string]$Token = '',
    [switch]$Resume
)

# The coordinator replaces the placeholder in the next line right before handing the file over (valid for 1 hour).
$RunnerToken = '__RUNNER_TOKEN__'

$ErrorActionPreference = 'Continue'
$env:WSL_UTF8 = '1'
$Distro      = 'Ubuntu-24.04'
$LinuxUser   = 'onair'
$RepoUrl     = 'https://github.com/adswill/OnAir-ci'
$RunnerName  = 'onair-laptop'
$DataDir     = Join-Path $env:ProgramData 'OnAir'
$StepsDir    = Join-Path $DataDir 'steps'
$PrivDir     = Join-Path $DataDir 'private'
$LogFile     = Join-Path $env:USERPROFILE 'OnAir-Laptop-Setup.log'
$ResumeTask  = 'OnAir-Laptop-Setup-Resume'
$KeepTask    = 'OnAir-Laptop-Keepalive'
$script:LastOut = ''

function Log([string]$m) {
    try { Add-Content -Path $LogFile -Value ((Get-Date -Format 's') + ' ' + $m) -Encoding UTF8 } catch { }
}
function Say([string]$m)  { Write-Host ''; Write-Host ('==> ' + $m) -ForegroundColor Cyan; Log ('STEP ' + $m) }
function Info([string]$m) { Write-Host ('    ' + $m); Log $m }
function Warn([string]$m) { Write-Host ('    ' + $m) -ForegroundColor Yellow; Log ('WARN ' + $m) }
function Die([string]$m)  { throw (New-Object System.Exception($m)) }

# Runs a program, writes all its output to the log, shows only lines that start with '##' on screen, returns the exit code.
function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments)
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $lines = New-Object System.Collections.ArrayList
    try {
        & $Exe @Arguments 2>&1 | ForEach-Object {
            $line = ('' + $_) -replace "`0", ''
            if ($line.Trim().Length -gt 0) {
                [void]$lines.Add($line)
                Log $line
                if ($line.StartsWith('##')) { Write-Host ('    ' + $line.Substring(2).Trim()) }
            }
        }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    $script:LastOut = ($lines -join "`n")
    return $code
}

function Write-Lf([string]$path, [string]$text) {
    $t = $text -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($path, $t, (New-Object System.Text.UTF8Encoding($false)))
}
function To-WslPath([string]$p) {
    $d = $p.Substring(0, 1).ToLower()
    return '/mnt/' + $d + ($p.Substring(2) -replace '\\', '/')
}
function Wsl-Root([string[]]$cmd) {
    return (Invoke-Native 'wsl.exe' (@('-d', $Distro, '-u', 'root', '-e') + $cmd))
}
function Distro-Present {
    try {
        $o = (& wsl.exe -l -q 2>$null) -join "`n"
        $o = $o -replace "`0", ''
        foreach ($l in ($o -split "`n")) { if ($l.Trim() -eq $Distro) { return $true } }
    } catch { }
    return $false
}
function Wsl-Works {
    $c = Invoke-Native 'wsl.exe' @('--status')
    return ($c -eq 0)
}
function Wait-Internet {
    for ($i = 0; $i -lt 36; $i++) {
        try { [void][System.Net.Dns]::GetHostEntry('github.com'); return $true } catch { Start-Sleep -Seconds 5 }
    }
    return $false
}

function Register-ResumeTask {
    $self = Join-Path $PrivDir 'Setup.ps1'
    $arg = '-NoProfile -ExecutionPolicy Bypass -File "' + $self + '" -Resume -SrcDir "' + $SrcDir + '"'
    $who = $env:USERDOMAIN + '\' + $env:USERNAME
    $act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg
    $trg = New-ScheduledTaskTrigger -AtLogOn -User $who
    $pri = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
    $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 5)
    Register-ScheduledTask -TaskName $ResumeTask -Action $act -Trigger $trg -Principal $pri -Settings $set -Force | Out-Null
}
function Register-KeepaliveTask {
    $who = $env:USERDOMAIN + '\' + $env:USERNAME
    $build = [Environment]::OSVersion.Version.Build
    $wslArgs = '-d ' + $Distro + ' -u root -e sleep infinity'
    if ($build -ge 22000) {
        $act = New-ScheduledTaskAction -Execute 'conhost.exe' -Argument ('--headless wsl.exe ' + $wslArgs)
    } else {
        $act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -WindowStyle Hidden -Command "wsl.exe ' + $wslArgs + '"')
    }
    $trg = New-ScheduledTaskTrigger -AtLogOn -User $who
    $pri = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Limited
    $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -Hidden -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $KeepTask -Action $act -Trigger $trg -Principal $pri -Settings $set -Force | Out-Null
}
function Get-AcMinutes([string]$sub, [string]$setting) {
    $o = (& powercfg.exe /q SCHEME_CURRENT $sub $setting 2>$null) -join "`n"
    if ($o -match 'Current AC Power Setting Index:\s*0x([0-9a-fA-F]+)') { return [int]([Convert]::ToInt64($Matches[1], 16) / 60) }
    return $null
}

# The live monitor window (OnAir-Monitor.cmd). A full copy is used when it sits next to this installer; otherwise a
# small launcher runs the copy that every job installs to ~/onair-monitor inside Ubuntu.
$MonitorLauncher = @'
@echo off
rem OnAir Monitor (small launcher written by OnAir-Laptop-Setup). ASCII only, no secrets.
title OnAir Monitor
chcp 65001 >nul
if not defined WT_SESSION mode con: cols=120 lines=40
wsl.exe -d Ubuntu-24.04 -u onair -e sh -c "if [ -f ~/onair-monitor/monitor.py ]; then exec python3 ~/onair-monitor/monitor.py %*; else echo The monitor is installed by the first job. Until then double-click OnAir-Monitor.cmd.; sleep 10; fi"
if errorlevel 1 pause
'@

function Install-Monitor([bool]$autostart) {
    try {
        $dest = Join-Path $DataDir 'OnAir-Monitor.cmd'
        $full = Join-Path $SrcDir 'OnAir-Monitor.cmd'
        if (Test-Path $full) { Copy-Item -Path $full -Destination $dest -Force }
        else { Set-Content -Path $dest -Value $MonitorLauncher -Encoding ASCII }
        $sh = New-Object -ComObject WScript.Shell
        $dirs = @([Environment]::GetFolderPath('Desktop'))
        if ($autostart) { $dirs += [Environment]::GetFolderPath('Startup') }
        foreach ($d in $dirs) {
            if (-not $d) { continue }
            $lnk = $sh.CreateShortcut((Join-Path $d 'OnAir Monitor.lnk'))
            $lnk.TargetPath = $dest
            $lnk.WorkingDirectory = $DataDir
            $lnk.Description = 'Live view of what the OnAir laptop worker is building and testing'
            $lnk.IconLocation = (Join-Path $env:SystemRoot 'System32\cmd.exe') + ',0'
            $lnk.Save()
        }
        if (-not $autostart) {
            $st = [Environment]::GetFolderPath('Startup')
            if ($st) { Remove-Item -Path (Join-Path $st 'OnAir Monitor.lnk') -Force -ErrorAction SilentlyContinue }
        }
        Info ('Desktop shortcut "OnAir Monitor" created' + $(if ($autostart) { ' (it also opens at login).' } else { '.' }))
    } catch { Warn ('Could not create the OnAir Monitor shortcut: ' + $_.Exception.Message) }
}

# ---------------------------------------------------------------- bash step scripts (run inside Ubuntu as root)
$StepUser = @'
set -e
echo '## Creating the Linux user onair'
id onair >/dev/null 2>&1 || adduser --disabled-password --gecos '' onair
usermod -aG sudo onair
echo 'onair ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/onair
chmod 440 /etc/sudoers.d/onair
cat > /etc/wsl.conf <<'EOF'
[boot]
systemd=true

[user]
default=onair
EOF
echo 'user ready'
'@

$StepRunner = @'
set -uo pipefail
TOKEN="${1:-}"
RDIR=/home/onair/actions-runner
export DEBIAN_FRONTEND=noninteractive
echo '## Waiting for systemd'
for i in $(seq 1 60); do
  s=$(systemctl is-system-running 2>/dev/null || true)
  case "$s" in running|degraded) break;; esac
  sleep 2
done
echo "systemd state: $s"
case "$s" in running|degraded) ;; *) echo 'ERROR: systemd is not running in Ubuntu'; exit 10;; esac
echo '## Installing download tools'
apt-get update -y || exit 11
apt-get install -y curl jq ca-certificates tar sudo || exit 11
if [ ! -x "$RDIR/config.sh" ]; then
  echo '## Downloading the latest GitHub Actions runner (linux x64)'
  rel=$(curl -fsSL -H 'Accept: application/vnd.github+json' https://api.github.com/repos/actions/runner/releases/latest || true)
  ver=$(printf '%s' "$rel" | jq -r '.tag_name // empty' 2>/dev/null | sed 's/^v//')
  if [ -z "$ver" ]; then
    ver=$(curl -fsSLI -o /dev/null -w '%{url_effective}' https://github.com/actions/runner/releases/latest | sed 's#.*/tag/v##')
  fi
  [ -n "$ver" ] || { echo 'ERROR: could not find the latest runner version'; exit 20; }
  echo "runner version $ver"
  curl -fsSL -o /tmp/runner.tgz "https://github.com/actions/runner/releases/download/v$ver/actions-runner-linux-x64-$ver.tar.gz" || { echo 'ERROR: runner download failed'; exit 20; }
  want=$(printf '%s' "$rel" | jq -r '.body // empty' 2>/dev/null | grep -o 'BEGIN SHA linux-x64 -->[0-9a-f]*' | sed 's/.*-->//' | head -1 || true)
  if [ -n "$want" ]; then
    got=$(sha256sum /tmp/runner.tgz | cut -d' ' -f1)
    [ "$got" = "$want" ] || { echo "ERROR: runner checksum mismatch ($got vs $want)"; exit 21; }
    echo 'runner checksum verified'
  else
    echo 'note: no checksum published in the release notes, skipped'
  fi
  install -d -o onair -g onair "$RDIR"
  su onair -c "tar xzf /tmp/runner.tgz -C $RDIR" || exit 22
  rm -f /tmp/runner.tgz
  "$RDIR/bin/installdependencies.sh" || exit 23
fi
if [ -f "$RDIR/.runner" ]; then
  echo '## Runner is already registered, keeping it'
else
  [ -n "$TOKEN" ] || { echo 'ERROR: no registration token'; exit 30; }
  echo '## Registering the runner with adswill/OnAir-ci'
  su onair -c "cd $RDIR && ./config.sh --unattended --url https://github.com/adswill/OnAir-ci --token '$TOKEN' --name onair-laptop --labels onair-laptop --work _work --replace" || { echo 'ERROR: runner registration failed'; exit 31; }
fi
echo 'runner step done'
'@

$StepTools = @'
set -uo pipefail
export DEBIAN_FRONTEND=noninteractive
RDIR=/home/onair/actions-runner
echo '## Installing build tools (this takes a while)'
apt-get update -y || exit 40
apt-get install -y build-essential clang cmake ninja-build ccache git python3 python3-yaml pkg-config jq curl unzip zip xz-utils file gdb util-linux nasm ca-certificates \
  libhackrf-dev libsoapysdr-dev libusb-1.0-0-dev libglfw3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libhamlib-dev libssl-dev mesa-vulkan-drivers vulkan-tools xvfb libgl1-mesa-dri libglx-mesa0 fonts-dejavu-core || exit 41
echo '## Installing Rust (stable) and setting up ccache (20 GB)'
su - onair -c 'command -v rustup >/dev/null 2>&1 || curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal; . "$HOME/.cargo/env"; rustup update stable; rustc --version' || exit 42
su - onair -c 'mkdir -p "$HOME/.ccache" "$HOME/.config/ccache"; ccache --set-config=cache_dir="$HOME/.ccache"; ccache --set-config=max_size=20G; ccache --set-config=compression=true; ccache -p | grep -E "max_size|cache_dir"' || exit 43
# The runner service reads .path (PATH) and .env (variables) from its folder.
echo "/home/onair/.cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" > "$RDIR/.path"
printf 'CCACHE_DIR=/home/onair/.ccache\nCCACHE_MAXSIZE=20G\nCCACHE_COMPRESS=true\nCI=true\n' > "$RDIR/.env"
chown onair:onair "$RDIR/.path" "$RDIR/.env"
echo 'toolchain done'
'@

$StepService = @'
set -uo pipefail
RDIR=/home/onair/actions-runner
for i in $(seq 1 60); do
  s=$(systemctl is-system-running 2>/dev/null || true)
  case "$s" in running|degraded) break;; esac
  sleep 2
done
cd "$RDIR" || exit 50
[ -f .runner ] || { echo 'ERROR: the runner is not registered'; exit 51; }
echo '## Installing the runner as a service'
loginctl enable-linger onair >/dev/null 2>&1 || true
if [ -f .service ] && systemctl cat "$(cat .service)" >/dev/null 2>&1; then
  echo 'service already installed'
else
  ./svc.sh install onair || exit 52
fi
./svc.sh start || true
svc=$(cat .service 2>/dev/null || true)
systemctl enable "$svc" >/dev/null 2>&1 || true
echo '## Self-test: waiting for the runner to listen for jobs'
ok=0
for i in $(seq 1 60); do
  if systemctl is-active --quiet "$svc"; then
    if journalctl -u "$svc" --no-pager 2>/dev/null | grep -q 'Listening for Jobs' || grep -qs 'Listening for Jobs' "$RDIR"/_diag/Runner_*.log; then ok=1; break; fi
  fi
  sleep 3
done
systemctl status "$svc" --no-pager 2>&1 | head -12
if [ "$ok" = 1 ]; then echo 'SELFTEST OK'; exit 0; fi
echo 'SELFTEST FAILED: no "Listening for Jobs" line within 3 minutes'
journalctl -u "$svc" --no-pager -n 30 2>&1 || true
exit 53
'@

# ---------------------------------------------------------------- main
function Main {
    if ([string]::IsNullOrEmpty($SrcDir)) { $SrcDir = $env:ONAIR_DIR }
    if ([string]::IsNullOrEmpty($SrcDir)) { $SrcDir = (Get-Location).Path }
    $SrcDir = $SrcDir.TrimEnd('\')
    if ([string]::IsNullOrEmpty($Token) -and $env:ONAIR_TOKEN) { Set-Variable -Name Token -Value $env:ONAIR_TOKEN -Scope Script }
    $Token = $script:Token
    Set-Variable -Name SrcDir -Value $SrcDir -Scope Script

    # ---- elevation (one UAC prompt)
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Write-Host 'Windows will now ask for permission once. Please click Yes.' -ForegroundColor Yellow
        $a = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -SrcDir "' + $SrcDir + '"'
        if ($Token) { $a += ' -Token ' + $Token }
        try { Start-Process -FilePath 'powershell.exe' -ArgumentList $a -Verb RunAs } catch { Die 'Permission was not given, so nothing was changed. Run the file again and click Yes.' }
        return 'elevated'
    }

    New-Item -ItemType Directory -Force -Path $DataDir, $StepsDir, $PrivDir | Out-Null
    & icacls.exe $PrivDir /inheritance:r /grant:r '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-18:(OI)(CI)F' | Out-Null
    Log '=================================================================='
    Log ('OnAir-Laptop-Setup started. Resume=' + $Resume + ' SrcDir=' + $SrcDir)
    Write-Host ''
    Write-Host '  OnAir laptop worker setup' -ForegroundColor Green
    Write-Host '  You do not need to do anything. Progress is printed below; details go to:'
    Write-Host ('  ' + $LogFile)
    Write-Host ''

    # keep a private copy of this script for the restart (it may hold the token, so only admins can read it)
    $self = Join-Path $PrivDir 'Setup.ps1'
    if ($PSCommandPath -and ($PSCommandPath -ne $self)) { Copy-Item -Path $PSCommandPath -Destination $self -Force }

    if ($Resume) {
        Say 'Continuing setup after the restart'
        if (-not (Wait-Internet)) { Die 'No internet connection after the restart. Connect to the internet; setup continues at the next login.' }
    }

    # ---- token
    $tok = ''
    $placeholder = '__RUNNER_' + 'TOKEN__'
    if ($Token) { $tok = $Token }
    elseif ($RunnerToken -and ($RunnerToken -ne $placeholder)) { $tok = $RunnerToken }
    elseif (Test-Path (Join-Path $SrcDir 'runner-token.txt')) { $tok = (Get-Content (Join-Path $SrcDir 'runner-token.txt') -Raw).Trim() }
    elseif (Test-Path (Join-Path $PrivDir 'runner-token.txt')) { $tok = (Get-Content (Join-Path $PrivDir 'runner-token.txt') -Raw).Trim() }
    if ($tok -and ($tok -notmatch '^[A-Za-z0-9]{20,}$')) { Die 'The registration token has a wrong format. Ask Claude for a new installer.' }
    if ($tok) { Set-Content -Path (Join-Path $PrivDir 'runner-token.txt') -Value $tok -Encoding ASCII }

    # ---- the only question, asked now so the rest runs unattended (also after the restart)
    $autoFile = Join-Path $DataDir 'monitor-autostart.txt'
    if (-not (Test-Path $autoFile)) {
        $ans = ''
        if (-not $Resume) {
            try { $ans = Read-Host '  Open the OnAir Monitor window automatically when you log in? [y/N]' } catch { $ans = '' }
        }
        $v = 'no'
        if ($ans -match '^\s*[yY]') { $v = 'yes' }
        Set-Content -Path $autoFile -Value $v -Encoding ASCII
    }
    $monitorAuto = ((Get-Content $autoFile -Raw).Trim() -eq 'yes')

    # ---- 1. checks
    Say 'Step 1 of 8: checking this computer'
    $build = [Environment]::OSVersion.Version.Build
    if ($build -lt 19041) { Die ('This Windows version (build ' + $build + ') is too old for WSL2. Windows 10 2004 or newer, or Windows 11, is needed.') }
    $sysDrive = $env:SystemDrive.TrimEnd(':')
    $freeGB = [math]::Round((Get-PSDrive -Name $sysDrive).Free / 1GB, 1)
    $have = Distro-Present
    $need = 40
    if ($have) { $need = 10 }
    Info ('Free disk space on ' + $env:SystemDrive + ' is ' + $freeGB + ' GB (needed: ' + $need + ' GB, 60 GB is comfortable).')
    if ($freeGB -lt $need) { Die ('Not enough free disk space: ' + $freeGB + ' GB free on ' + $env:SystemDrive + ', at least ' + $need + ' GB are needed. Free some space and run this file again. Nothing was changed.') }
    try {
        $cs = Get-CimInstance Win32_ComputerSystem
        $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
        if ((-not $cs.HypervisorPresent) -and ($cpu.VirtualizationFirmwareEnabled -eq $false)) {
            Die 'Virtualization is switched off in the BIOS/UEFI. Restart, open the BIOS (Del/F2 at start), turn on "Intel Virtualization Technology" (VT-x), save, and run this file again. See docs/LAPTOP-WORKER.md, Troubleshooting.'
        }
    } catch [System.Exception] { if ($_.Exception.Message -like 'Virtualization*') { throw } else { Log ('virtualization check skipped: ' + $_.Exception.Message) } }
    if (-not $tok) {
        $configured = $false
        if ($have) { $configured = ((Wsl-Root @('test', '-f', '/home/onair/actions-runner/.runner')) -eq 0) }
        if (-not $configured) { Die 'This installer has no runner registration token (or it was removed). Ask Claude for a new installer.' }
    }
    Info 'The computer looks fine.'

    # ---- 2. Windows features
    Say 'Step 2 of 8: turning on the Windows features for WSL2'
    $needReboot = $false
    foreach ($f in @('Microsoft-Windows-Subsystem-Linux', 'VirtualMachinePlatform')) {
        $c = Invoke-Native 'dism.exe' @('/online', '/enable-feature', ('/featurename:' + $f), '/all', '/norestart')
        if ($c -eq 3010) { $needReboot = $true }
        elseif ($c -ne 0) { Die ('Could not enable the Windows feature ' + $f + ' (code ' + $c + '). See the log.') }
        try { if ((Get-WindowsOptionalFeature -Online -FeatureName $f).State -eq 'EnablePending') { $needReboot = $true } } catch { }
    }
    Info 'Windows features are on.'

    # ---- 3. .wslconfig
    Say 'Step 3 of 8: giving WSL its memory and CPU limits (22 GB, 14 processors, 8 GB swap)'
    $cfg = Join-Path $env:USERPROFILE '.wslconfig'
    $marker = '# OnAir-Laptop-Setup'
    $want = $marker + "`r`n[wsl2]`r`nmemory=22GB`r`nprocessors=14`r`nswap=8GB`r`n`r`n[experimental]`r`nsparseVhd=true`r`nautoMemoryReclaim=gradual`r`n"
    $cur = ''
    if (Test-Path $cfg) { $cur = Get-Content $cfg -Raw }
    if ($cur -ne $want) {
        if ((Test-Path $cfg) -and ($cur -notlike ('*' + $marker + '*')) -and -not (Test-Path ($cfg + '.onair-backup'))) { Copy-Item $cfg ($cfg + '.onair-backup') -Force; Info 'Your old .wslconfig was saved as .wslconfig.onair-backup' }
        Set-Content -Path $cfg -Value $want -Encoding ASCII
        Info ('Written: ' + $cfg)
    } else { Info '.wslconfig is already set.' }

    # ---- 4. WSL itself (may need a restart)
    Say 'Step 4 of 8: installing WSL2'
    [void](Invoke-Native 'wsl.exe' @('--install', '--no-distribution'))
    $flag = Join-Path $DataDir 'rebooted.flag'
    if ((-not (Test-Path $flag)) -and ($script:LastOut -match '(?i)(reboot|restart)')) { $needReboot = $true }
    [void](Invoke-Native 'wsl.exe' @('--update'))
    [void](Invoke-Native 'wsl.exe' @('--set-default-version', '2'))
    if (-not (Wsl-Works)) { $needReboot = $true }
    if ($needReboot) {
        if (Test-Path $flag) { Die 'WSL still does not work after the restart. Most likely virtualization is off in the BIOS/UEFI, or this Windows edition or policy blocks it. See docs/LAPTOP-WORKER.md, Troubleshooting.' }
        Set-Content -Path $flag -Value (Get-Date -Format 's') -Encoding ASCII
        Register-ResumeTask
        Write-Host ''
        Write-Host '  ============================================================' -ForegroundColor Yellow
        Write-Host '  Restarting in 30 seconds to finish setup.' -ForegroundColor Yellow
        Write-Host '  It continues by itself after you log in. Nothing to click.' -ForegroundColor Yellow
        Write-Host '  ============================================================' -ForegroundColor Yellow
        Log 'Restart requested; resume task registered.'
        & shutdown.exe /r /t 30 /c 'OnAir laptop setup: restarting to finish. It continues by itself after login.' | Out-Null
        return 'rebooting'
    }
    Info 'WSL2 works.'

    # ---- 5. Ubuntu 24.04
    Say 'Step 5 of 8: installing Ubuntu 24.04 (about 1 GB download)'
    if (Distro-Present) { Info 'Ubuntu 24.04 is already installed.' }
    else {
        [void](Invoke-Native 'wsl.exe' @('--install', '-d', $Distro, '--no-launch'))
        if (-not (Distro-Present)) {
            $exe = Get-Command 'ubuntu2404.exe' -ErrorAction SilentlyContinue
            if ($exe) { [void](Invoke-Native $exe.Source @('install', '--root')) }
        }
        if (-not (Distro-Present)) { Die 'Ubuntu 24.04 could not be installed. Check the internet connection and the log, then run this file again.' }
    }
    if ((Wsl-Root @('true')) -ne 0) { Die 'Ubuntu 24.04 does not start. See the log and docs/LAPTOP-WORKER.md, Troubleshooting.' }
    [void](Invoke-Native 'wsl.exe' @('--manage', $Distro, '--set-sparse', 'true'))
    Write-Lf (Join-Path $StepsDir 'step-user.sh') $StepUser
    Write-Lf (Join-Path $StepsDir 'step-runner.sh') $StepRunner
    Write-Lf (Join-Path $StepsDir 'step-tools.sh') $StepTools
    Write-Lf (Join-Path $StepsDir 'step-service.sh') $StepService
    $c = Wsl-Root @('bash', (To-WslPath (Join-Path $StepsDir 'step-user.sh')))
    if ($c -ne 0) { Die ('Could not create the Linux user (code ' + $c + '). See the log.') }
    # restart the distro so systemd (set in /etc/wsl.conf) and the .wslconfig limits take effect
    [void](Invoke-Native 'wsl.exe' @('--shutdown'))
    Start-Sleep -Seconds 8

    # ---- 6. runner download + registration (early, because the token lives only 1 hour)
    Say 'Step 6 of 8: downloading and registering the GitHub runner'
    $c = Wsl-Root @('bash', (To-WslPath (Join-Path $StepsDir 'step-runner.sh')), $tok)
    if ($c -eq 31 -or $c -eq 30) {
        Die 'GitHub refused the registration token (it is probably expired: tokens last 1 hour). Ask Claude for a new installer and run it; everything already installed is kept.'
    }
    if ($c -ne 0) { Die ('The runner step failed (code ' + $c + '). See the log.') }
    Info 'Runner registered as onair-laptop (not started yet).'

    # ---- 7. toolchain
    Say 'Step 7 of 8: installing the build tools inside Ubuntu (10 to 20 minutes)'
    $c = Wsl-Root @('bash', (To-WslPath (Join-Path $StepsDir 'step-tools.sh')))
    if ($c -ne 0) { Die ('Installing the build tools failed (code ' + $c + '). See the log. Run this file again to retry.') }

    # ---- 8. service, keep-alive, power, self-test
    Say 'Step 8 of 8: starting the runner service, keep-alive and power settings'
    Register-KeepaliveTask
    Install-Monitor $monitorAuto
    try { Start-ScheduledTask -TaskName $KeepTask } catch { Log ('keepalive start: ' + $_.Exception.Message) }
    $acStandby = Join-Path $DataDir 'prev-standby-ac-min.txt'
    if (-not (Test-Path $acStandby)) {
        $m = Get-AcMinutes 'SUB_SLEEP' 'STANDBYIDLE'
        if ($m -ne $null) { Set-Content -Path $acStandby -Value $m -Encoding ASCII }
    }
    [void](Invoke-Native 'powercfg.exe' @('/change', 'standby-timeout-ac', '0'))
    Info 'Sleep is off while plugged in (battery settings are not changed; closing the lid still follows your Windows setting).'
    $c = Wsl-Root @('bash', (To-WslPath (Join-Path $StepsDir 'step-service.sh')))

    # clean up the resume task and the private copy (it held the token)
    try { Unregister-ScheduledTask -TaskName $ResumeTask -Confirm:$false -ErrorAction SilentlyContinue } catch { }
    Remove-Item -Path (Join-Path $PrivDir 'runner-token.txt') -Force -ErrorAction SilentlyContinue
    if ($c -ne 0) {
        Write-Host ''
        Write-Host '  NOT READY: the runner did not report that it is listening for jobs.' -ForegroundColor Red
        Write-Host ('  Look at the end of the log: ' + $LogFile) -ForegroundColor Red
        Write-Host '  Run this file again to retry, or send the log to Claude.' -ForegroundColor Red
        return 'notready'
    }
    Write-Host ''
    Write-Host '  ########  ########    ###    ########  ##    ##' -ForegroundColor Green
    Write-Host '  ##     ## ##         ## ##   ##     ##  ##  ## ' -ForegroundColor Green
    Write-Host '  ########  ######    ##   ##  ##     ##   ####  ' -ForegroundColor Green
    Write-Host '  ##   ##   ##       ######### ##     ##    ##   ' -ForegroundColor Green
    Write-Host '  ##    ##  ######## ##     ## ########     ##   ' -ForegroundColor Green
    Write-Host ''
    Write-Host '  READY: the laptop is online as runner "onair-laptop". You can close this window.' -ForegroundColor Green
    Write-Host '  Keep the laptop on and logged in; it takes jobs by itself.' -ForegroundColor Green
    Log 'READY'
    return 'ready'
}

$result = ''
try {
    $result = @(Main)[-1]
} catch {
    $msg = $_.Exception.Message
    Write-Host ''
    Write-Host ('  PROBLEM: ' + $msg) -ForegroundColor Red
    Write-Host ('  Details: ' + $LogFile) -ForegroundColor Red
    Log ('ERROR ' + $msg)
    Log ($_ | Out-String)
    if ($Resume) {
        $cnt = Join-Path $DataDir 'resume-failures.txt'
        $n = 0
        if (Test-Path $cnt) { $n = [int](Get-Content $cnt -Raw) }
        $n++
        Set-Content -Path $cnt -Value $n -Encoding ASCII
        if ($n -ge 3) {
            try { Unregister-ScheduledTask -TaskName $ResumeTask -Confirm:$false -ErrorAction SilentlyContinue } catch { }
            Write-Host '  Setup failed 3 times after restarts; the automatic resume was removed. Ask Claude for help with the log.' -ForegroundColor Red
        }
    }
}
if ($result -ne 'rebooting' -and $result -ne 'elevated') {
    Write-Host ''
    [void](Read-Host 'Press Enter to close this window')
}
