# One-time setup of the OnAir remote build worker on the Windows laptop (WSL2 + Ubuntu 24.04 + SSH).
# Run in an ELEVATED PowerShell:   powershell -ExecutionPolicy Bypass -File setup-windows.ps1
# Safe to run again: every step checks first. It asks before each change to the system. No secret is stored.
# What it opens: ONE Windows firewall rule, TCP <port> (default 2222), only from the subnets you give (default: Radmin VPN 26.0.0.0/8
# and Tailscale 100.64.0.0/10), Private network profile only. Nothing is opened to the internet. Guide: docs/REMOTE-BUILD.md
param(
    [int]$Port = 2222,
    [string]$AllowFrom = "26.0.0.0/8,100.64.0.0/10",   # add your home LAN (for example 192.168.1.0/24) if you use it
    [string]$Distro = "Ubuntu-24.04",
    [string]$LinuxUser = "onair",
    [string]$PubKeyFile = ""                              # file with the Mac's PUBLIC key (id_ed25519.pub); asked for if empty
)
$ErrorActionPreference = "Stop"
function Ask($q) { $a = Read-Host "$q [y/N]"; return ($a -match '^(y|yes)$') }
function Say($m) { Write-Host "==> $m" -ForegroundColor Cyan }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "Run this in an elevated (Administrator) PowerShell." }
$restart = $false

# 1. WSL2
Say "Step 1: WSL2"
$wslOk = $false
try { wsl --status *> $null; $wslOk = ($LASTEXITCODE -eq 0) } catch {}
if ($wslOk) { Write-Host "WSL is already installed." } else {
    if (Ask "Install WSL2 (Windows feature; needs a reboot afterwards)?") {
        wsl --install --no-distribution
        Write-Host "Reboot Windows now, then run this script again." -ForegroundColor Yellow; exit 0
    } else { throw "WSL2 is required." }
}
wsl --set-default-version 2 | Out-Null

# 2. .wslconfig: this laptop has 20 threads and 31.7 GB; leave 4 threads and ~6 GB for Windows
Say "Step 2: WSL memory and processor limits"
$cfg = Join-Path $env:USERPROFILE ".wslconfig"
$want = "[wsl2]`r`nmemory=25GB`r`nprocessors=16`r`nswap=8GB`r`n`r`n[experimental]`r`nautoMemoryReclaim=gradual`r`n"
if ((Test-Path $cfg) -and ((Get-Content $cfg -Raw) -match "memory=")) { Write-Host ".wslconfig already sets limits, left as it is: $cfg" } else {
    Write-Host "Will write $cfg :`n$want"
    if (Ask "Write it?") { Set-Content -Path $cfg -Value $want -Encoding ASCII; $restart = $true }
}

# 3. Ubuntu 24.04
Say "Step 3: $Distro"
$have = (wsl -l -q) -replace "`0", "" | Where-Object { $_.Trim() -eq $Distro }
if ($have) { Write-Host "$Distro is already installed." } else {
    if (Ask "Install $Distro (about 1 GB download)?") { wsl --install -d $Distro --no-launch } else { throw "The distro is required." }
}
if ($restart) { wsl --shutdown }
if (Ask "Create Linux user '$LinuxUser' (no password; key login only) and enable systemd in ${Distro}?") {
    wsl -d $Distro -u root -- bash -c "id $LinuxUser >/dev/null 2>&1 || adduser --disabled-password --gecos '' $LinuxUser; grep -q 'systemd=true' /etc/wsl.conf 2>/dev/null || printf '[boot]\nsystemd=true\n\n[user]\ndefault=$LinuxUser\n' >> /etc/wsl.conf"
    wsl --terminate $Distro
}

# 4. Toolchain inside Ubuntu (the list comes from the Linux CI job and tools/dev/Dockerfile.linux, plus ccache, ninja and Rust)
Say "Step 4: build toolchain inside $Distro (apt packages, Rust, ccache 30G)"
$apt = "build-essential clang cmake ninja-build ccache git python3 pkg-config util-linux file gdb openssh-server curl ca-certificates " +
       "libhackrf-dev libsoapysdr-dev libusb-1.0-0-dev libglfw3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev " +
       "libhamlib-dev libssl-dev mesa-vulkan-drivers vulkan-tools xvfb libgl1-mesa-dri libglx-mesa0 fonts-dejavu-core"
Write-Host "apt packages: $apt"
if (Ask "Install them?") { wsl -d $Distro -u root -- bash -c "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y $apt" }
if (Ask "Install Rust with rustup (stable, needs >= 1.89) and set ccache to 30G for ${LinuxUser}?") {
    wsl -d $Distro -u $LinuxUser -- bash -c 'command -v rustup >/dev/null || curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal; . "$HOME/.cargo/env"; rustup update stable; rustc --version; mkdir -p ~/onair-logs; ccache --set-config=max_size=30G; ccache --set-config=compression=true; ccache -p | grep max_size'
}
Write-Host "sccache is optional and not installed (ccache covers the C++; the cargo builds are small)."

# 5. SSH server in WSL (key only)
Say "Step 5: OpenSSH server in $Distro on port $Port (key login only)"
if (-not $PubKeyFile) { $PubKeyFile = Read-Host "Path to the Mac's PUBLIC key file (copy ~/.ssh/id_ed25519.pub from the Mac; never the private key)" }
if (-not (Test-Path $PubKeyFile)) { throw "Public key file not found: $PubKeyFile" }
$pub = (Get-Content $PubKeyFile -Raw).Trim()
if ($pub -match "PRIVATE KEY") { throw "That is a PRIVATE key. Stop. Give the .pub file." }
if (Ask "Write the sshd config (port $Port, no passwords, no root, AllowUsers $LinuxUser) and authorize that key?") {
    $conf = "Port $Port`nPasswordAuthentication no`nKbdInteractiveAuthentication no`nPubkeyAuthentication yes`nPermitRootLogin no`nAllowUsers $LinuxUser`nX11Forwarding no`nMaxAuthTries 3`nClientAliveInterval 60`n"
    $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($conf))
    $kb64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pub + "`n"))
    wsl -d $Distro -u root -- bash -c "echo $b64 | base64 -d > /etc/ssh/sshd_config.d/10-onair.conf; install -d -m700 -o $LinuxUser -g $LinuxUser /home/$LinuxUser/.ssh; echo $kb64 | base64 -d > /home/$LinuxUser/.ssh/authorized_keys; chown ${LinuxUser}:${LinuxUser} /home/$LinuxUser/.ssh/authorized_keys; chmod 600 /home/$LinuxUser/.ssh/authorized_keys; ssh-keygen -A; systemctl enable ssh; systemctl disable ssh.socket 2>/dev/null; systemctl restart ssh; systemctl is-active ssh"
}

# 6. WSL2 is behind NAT: forward the port from Windows to WSL, and open the firewall for the VPN subnets only
Say "Step 6: Windows firewall rule and port forward to WSL"
Write-Host "This opens TCP $Port on Windows from [$AllowFrom] only, Private profile only. Not the internet."
if (Ask "Create the firewall rule 'OnAir remote build SSH' and the port forward $Port -> WSL?") {
    $wslIp = ((wsl -d $Distro -u root -- hostname -I).Trim() -split " ")[0]
    netsh interface portproxy delete v4tov4 listenport=$Port listenaddress=0.0.0.0 2>$null | Out-Null
    netsh interface portproxy add v4tov4 listenport=$Port listenaddress=0.0.0.0 connectport=$Port connectaddress=$wslIp | Out-Null
    Get-NetFirewallRule -DisplayName "OnAir remote build SSH" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName "OnAir remote build SSH" -Direction Inbound -Protocol TCP -LocalPort $Port -RemoteAddress ($AllowFrom -split ",") -Profile Private -Action Allow | Out-Null
    Write-Host "Forward: 0.0.0.0:$Port -> ${wslIp}:$Port (the WSL address can change at reboot: run this script again, it only redoes this step when everything else is in place)." -ForegroundColor Yellow
    Write-Host "Mark the Radmin/Tailscale adapter as a PRIVATE network (Settings > Network & internet > its properties), or the rule does not apply."
}

# 7. Report
Say "Step 7: what to put on the Mac"
$fp = wsl -d $Distro -u root -- ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
Write-Host "Host key fingerprint (compare it when the Mac connects the first time): $fp"
Write-Host "Addresses of this laptop (use the Radmin/Tailscale one):"
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notmatch '^(127|169\.254)' } | Format-Table InterfaceAlias, IPAddress -AutoSize
Write-Host "Firewall rule:"
Get-NetFirewallRule -DisplayName "OnAir remote build SSH" -ErrorAction SilentlyContinue | Format-Table DisplayName, Enabled, Profile, Direction, Action
Get-NetFirewallRule -DisplayName "OnAir remote build SSH" -ErrorAction SilentlyContinue | Get-NetFirewallAddressFilter | Format-Table RemoteAddress
Write-Host "To turn it off later see 'How to turn it off' in docs/REMOTE-BUILD.md."
Write-Host "Done." -ForegroundColor Green
