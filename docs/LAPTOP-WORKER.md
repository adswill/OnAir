# Laptop worker (one file, no SSH)

`OnAir-Laptop-Setup.cmd` turns the Windows 11 laptop into a build and test worker. You copy it to Downloads and double-click it. Nothing else.

The laptop only connects OUT to GitHub. There is no SSH, no open port, no VPN and no firewall change. It runs a GitHub Actions runner that is registered to the private repository `adswill/OnAir-ci`. It is never registered to the public OnAir repositories, because a runner on a public repository could run strangers' code.

## What you do

1. Put `OnAir-Laptop-Setup.cmd` (and nothing else) in Downloads. Double-click it. If Windows says "Windows protected your PC", click More info, then Run anyway.
2. Click Yes on the one Windows permission question.
3. Wait. The window prints progress in plain words. Windows restarts once (you get a 30 second warning: "restarting to finish setup; it continues by itself"). Log in as usual; the window comes back and continues.
4. When the window shows a big READY, you are done. Keep the laptop on and logged in when you want it to work.

Time: about 30 to 60 minutes, mostly downloads. The file contains a one-hour registration token, so run it within an hour of receiving it. If the token is too old the window says so; ask Claude for a new installer (everything already installed is kept and reused).

## Needs

- Windows 11 (or Windows 10 2004+), Home or Pro, virtualization on in the BIOS.
- Free disk space on C: at least 40 GB (60 GB is comfortable). The setup stops with a message before changing anything if there is less.
- Internet. About 1 GB for Ubuntu plus about 1.5 GB for the tools.

## What the file does, step by step

1. Checks Windows version, free disk and that virtualization is on.
2. Turns on the Windows features WSL and Virtual Machine Platform.
3. Writes `%USERPROFILE%\.wslconfig`: 22 GB memory, 14 processors, 8 GB swap, sparse disk (an existing file is saved as `.wslconfig.onair-backup`).
4. Installs WSL2. If Windows needs a restart, a scheduled task "OnAir-Laptop-Setup-Resume" continues the setup at the next login, then removes itself.
5. Installs Ubuntu 24.04 with the user `onair` (no password), enables systemd.
6. Downloads the latest official GitHub Actions runner, checks its checksum and registers it as `onair-laptop` (labels `self-hosted, linux, x64, onair-laptop`) in `adswill/OnAir-ci`. This is done early because the token lives only one hour.
7. Installs the build tools in Ubuntu: compilers, cmake, ninja, ccache (20 GB), git, python, Rust, the OnAir libraries, Vulkan software drivers, xvfb.
8. Installs the runner as a service, adds a hidden scheduled task "OnAir-Laptop-Keepalive" that keeps Ubuntu running after every login, and stops the laptop sleeping while plugged in. Battery settings are not changed. Closing the lid still does what your Windows setting says (set "Do nothing" when plugged in if you want it to keep working with the lid closed).
9. Self-test: waits until the runner service is active and logs "Listening for Jobs", then prints READY.

It is safe to run twice: every step checks first.

## Restarts to expect

One restart during the first run (only if Windows needs it for WSL). Later runs: none. After a normal reboot, log in and the worker is back within a minute (the keep-alive task starts Ubuntu and the runner service starts with it). It does not work while you are logged out.

## Logs

- Setup log: `%USERPROFILE%\OnAir-Laptop-Setup.log`.
- Runner log inside Ubuntu: `wsl -d Ubuntu-24.04 -u root -e journalctl -u "actions.runner.*" -n 100`.
- Job logs and summaries: the Actions tab of `github.com/adswill/OnAir-ci`.

## How to turn it off

- Quick pause: Task Manager, or `wsl --shutdown` in a terminal. The runner shows offline and jobs wait.
- Disable the keep-alive: Task Scheduler, disable "OnAir-Laptop-Keepalive".
- Remove everything: double-click `OnAir-Laptop-Uninstall.cmd`. It removes the tasks, stops and removes the runner service, asks y/N before deleting the Ubuntu distro, puts the old `.wslconfig` and the sleep setting back. Then delete the offline runner at GitHub: `adswill/OnAir-ci` > Settings > Actions > Runners.

## How it is kept safe

- Outbound only; no listening port, no firewall rule.
- Registered only to the private repo `adswill/OnAir-ci` (only the owner can start jobs). Never to the public OnAir repos.
- The jobs check out the public OnAir repo without any token, so no secret is on the laptop. The registration token is used once and deleted from the disk at the end; it expires after one hour anyway.
- The runner runs as the unprivileged Linux user `onair` inside the Ubuntu VM, not as Windows administrator.
- Workflow inputs are checked against a character whitelist before use.

## Troubleshooting

- "Virtualization is switched off": open Task Manager > Performance > CPU. If it says "Virtualization: Disabled", restart into the BIOS/UEFI (Del, F2 or F10 at start; on many laptops hold Shift while clicking Restart > Troubleshoot > UEFI settings), turn on Intel Virtualization Technology (VT-x) and run the file again.
- Windows Home or Pro: both work (WSL2 does not need Hyper-V Pro). Windows N editions and very old builds may lack parts; `winver` should show Windows 11.
- Company or school computers: group policy may block WSL, virtual machines or scheduled tasks. Setup then stops with the reason in the log; this needs the administrator.
- "WSL still does not work after the restart": run `wsl --status` and `wsl --update` in a terminal, then run the file again.
- "token refused / expired": ask Claude for a new installer.
- Runner shows offline: log in to Windows (the keep-alive task runs at login); check `wsl -l -v` shows Ubuntu-24.04 Running; run the setup file again.
- Disk grows: WSL's virtual disk (ext4.vhdx) is sparse but only shrinks after `wsl --shutdown` and a trim; the build cache is limited to 20 GB.
- Antivirus blocks the .cmd: allow it, or right-click > Properties > Unblock.

## For the Mac side

`tools/laptop-installer/dispatch_laptop.sh status` shows whether the runner is online. `dispatch_laptop.sh <branch> '<ctest regex>' [targets...]` starts a build and waits for the result. The workflows live in the private repo OnAir-ci (`build-test.yml`, `full-test.yml`). The GPU tests need a second runner on native Windows; the plan is in the OnAir-ci README (later, not now).

The .cmd files are generated: edit `tools/laptop-installer/src/*.ps1` and run `python3 tools/laptop-installer/build.py`. Before handing the file over the coordinator replaces `__RUNNER_TOKEN__` with a fresh token (`gh api -X POST repos/adswill/OnAir-ci/actions/runners/registration-token --jq .token`), keeping the CRLF line endings; the token can also be placed in `runner-token.txt` next to the .cmd or passed as the first argument.
