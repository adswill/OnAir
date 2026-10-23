# Remote build worker (the Windows laptop)

## What it does
The Mac is the bottleneck when many agents build at once. The laptop (i7-12700H, 20 threads, 31.7 GB, NVMe) runs Linux builds and tests inside
WSL2 (Ubuntu 24.04). An agent on the Mac runs one command; the laptop fetches the pushed branch from GitHub (the repo is public, no token),
builds only the requested targets with Ninja + ccache, runs the chosen tests, and the Mac sees the log and a one-line result.
The laptop never gets a secret and never pushes anything.

## One-time setup
1. On the Mac make a key just for this (if you have none): `ssh-keygen -t ed25519 -f ~/.ssh/onair_remote -C onair-remote`
2. Copy `~/.ssh/onair_remote.pub` (the .pub only) to the laptop, for example by email to yourself or a USB stick.
3. Copy `tools/remote-build/setup-windows.ps1` to the laptop. Open PowerShell **as Administrator** and run
   `powershell -ExecutionPolicy Bypass -File .\setup-windows.ps1 -PubKeyFile C:\path\onair_remote.pub`
   (add `-AllowFrom "26.0.0.0/8,100.64.0.0/10,192.168.1.0/24"` if you also want your home LAN; default is Radmin VPN + Tailscale only).
4. It asks y/N before every change: WSL2 (a reboot may be needed, then run it again), `.wslconfig` (25 GB, 16 threads, so Windows keeps 6 GB and 4 threads),
   Ubuntu 24.04, the toolchain, SSH, the firewall rule. Expected end: a host-key fingerprint, the laptop addresses, and one firewall rule listed.
5. Mark the VPN adapter as a **Private** network in Windows (the rule only applies to Private networks).
6. On the Mac create `~/.config/onair-remote.env` (never commit it):

        ONAIR_REMOTE_HOST=26.x.y.z        # the laptop address on Radmin / Tailscale / LAN
        ONAIR_REMOTE_PORT=2222
        ONAIR_REMOTE_USER=onair
        ONAIR_REMOTE_KEY=$HOME/.ssh/onair_remote

7. Test: `tools/remote-build/remote_ctest.sh ping` should print `ok`, the core count and `SUMMARY: worker reachable`. The first connection stores the host key;
   compare it with the fingerprint printed in step 4 (`ssh-keygen -lf ~/.ssh/known_hosts`).
8. First real run (it clones and configures, so it is slow once):
   `tools/remote-build/remote_ctest.sh main "dect2core" ""`

## How to connect (find the address)
- **Radmin VPN**: the laptop's Radmin address (26.x.x.x) is shown in the Radmin window, or `ipconfig` in PowerShell under the Radmin adapter. Both machines must be in the same Radmin network. The Mac needs Radmin too (Windows only) - if it does not run there, use Tailscale.
- **Tailscale** (works on both, recommended): install on both, sign in, use the laptop's 100.x.y.z address (`tailscale ip -4` on the laptop). Covered by the default firewall subnet.
- **Same LAN**: `ipconfig`, IPv4 of Wi-Fi/Ethernet; re-run the setup with `-AllowFrom` including your LAN subnet. Give the laptop a fixed address in the router.
The setup script forwards Windows port 2222 to WSL. WSL's own address can change after a reboot; re-run the script (it only redoes the forward) if `ping` stops working after a restart.

## How agents use it
    tools/remote-build/remote_ctest.sh <branch> "<targets>" [<ctest -R regex>] [extra cmake args]
    tools/remote-build/remote_ctest.sh feature-x "dect2core test_dvbt" "dvbt|t2"
    tools/remote-build/remote_ctest.sh feature-x all ""            # full build, no tests
    tools/remote-build/remote_ctest.sh ping
The branch must be pushed to `pub` first. One job at a time (a lock); a second call prints `SUMMARY: BUSY` and exits 4. A job is killed after 90 minutes
(`ONAIR_REMOTE_TIMEOUT` in the env file). Logs: `~/onair-logs/<branch>-<time>.log` on the laptop (kept 7 days). Last line: `SUMMARY: PASS|FAIL|TIMEOUT | ... | log:`;
exit 0 pass, 1 fail, 3 unreachable, 4 busy, 124 timeout. Build flags equal the Mac's (`Release`, `DECT2_BUILD_APP=ON`, ccache); Linux has no GPU decoder, so GPU tests are not run here.

## Security
- SSH only, **key login only** (no passwords), root off, `AllowUsers onair`, port 2222, 3 tries.
- The only thing opened: Windows firewall rule "OnAir remote build SSH", TCP 2222, from 26.0.0.0/8 (Radmin) and 100.64.0.0/10 (Tailscale) [+ what you add], Private profile. Nothing is opened to the internet and no router port forward is needed. Do not add one.
- Secrets: none on the laptop. The repo is cloned over public https; no GitHub token, no Mac private key, no signing keys. Only the Mac's **public** key is there.
- The commands from the Mac are checked (branch, targets and regex are restricted characters) and the build runs as the unprivileged `onair` user inside WSL.
- Builds run code from the branches (tests, build scripts): only build your own branches.
- **If the laptop is lost or stolen**: it holds no secrets, but remove the Mac key trust anyway: nothing on the Mac trusts the laptop except `known_hosts`, so delete its line (`ssh-keygen -R <address>`); generate a new key pair for a new worker; leave the Radmin network / Tailscale (remove the device in the admin page); BitLocker should be on.

## Troubleshooting
- `ping` says not reachable: laptop awake? (disable sleep on AC power) VPN up? `wsl -d Ubuntu-24.04 -- systemctl status ssh`; re-run the setup script (port forward); adapter Private?
- `Permission denied (publickey)`: wrong key in the env file, or a different `.pub` was authorized (`/home/onair/.ssh/authorized_keys` in WSL).
- Host key changed: WSL was reinstalled; verify the fingerprint, then `ssh-keygen -R`.
- `BUSY`: another job runs. Wait. A stale lock cannot occur (flock is released when the job dies).
- Out of space: `ccache -C`, delete `~/onair/build`, or `wsl --manage Ubuntu-24.04 --set-sparse true`.
- Windows feels slow: lower `processors` in `%USERPROFILE%\.wslconfig`, then `wsl --shutdown`.
- Build fails only on the worker: it is Linux x86_64 with GCC 13 / clang; the same as the Linux CI job.

## How to turn it off
In an Administrator PowerShell on the laptop:

    Remove-NetFirewallRule -DisplayName "OnAir remote build SSH"
    netsh interface portproxy delete v4tov4 listenport=2222 listenaddress=0.0.0.0
    wsl -d Ubuntu-24.04 -u root -- systemctl disable --now ssh
    wsl --shutdown
Full removal: `wsl --unregister Ubuntu-24.04` (deletes the build tree and ccache) and delete `%USERPROFILE%\.wslconfig`.

## Later phases (not done, no scripts yet)
**GPU tests:** see `tools/remote-build/remote_gpu_tests.md` (Direct3D 11 LDPC on the RTX 3080 Ti, native Windows; CUDA for issue #39 is testable there but not implemented).

**Native Windows builds.** Today Windows is cross-built on the Mac with MinGW (`tools/package/make_windows_cross.sh`, toolchain `packaging/windows/mingw-toolchain.cmake`),
and the CI Windows job uses MSYS2 UCRT64 (`mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,ccache,rust}`, see `.github/workflows/ci.yml`). A native build on the laptop would use
MSYS2 UCRT64 (not MSVC: the code is built and packaged with GCC/MinGW). Needs: MSYS2 + those packages, the dependencies the cross script builds into `~/onair-win-prefix`
(FFmpeg, GLFW, libusb, libhackrf and the radio libraries; `tools/package/cross_deps_windows.sh`) - the Windows CI job needs them vendored from the Mac, so the first step would be
to zip the Mac's `~/onair-win-prefix` once and unpack it on the laptop (ONAIR_WIN_PREFIX). Then `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON` in the UCRT64 shell, and
the same ctest subset, with `test_gpu_ldpc` running on the RTX. A `remote_ctest.sh --windows` mode (ssh into MSYS2) would follow once that works by hand.
