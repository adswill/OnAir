# Host a receiver: put your radio on the internet and list it on web.on-air.dev

This guide is for people who have never done anything like this. You do not need to know what a "server" is. Every step says what to click or type, what you should see, and what to do if you do not see it.

At the end, anybody with a web browser can open a web page, tune your radio, and listen. Your radio can also appear in the public list at https://web.on-air.dev.


From OnAir 3.7 the quickest way is inside the app: **Server > Share this radio**, one button, a link and a QR code; no port forwarding and no files to edit. The options below are for running a standalone server. Until the OnAir relay is hosted, that button gives a temporary Cloudflare link that changes each time you share; for a link that stays the same use Way A, B or C below.

## Read this first: what is tested and what is not

Be honest with yourself about the state of this feature before you start.

* The server part (`onair-server` with OnAir Web) is tested a lot with the built-in test signal, with fake listeners, and on the author's Mac, including HTTPS with a certificate.
* It has **not yet been tried with a real browser over the real internet, on a Raspberry Pi, or through Tailscale, Cloudflare or Caddy.** The steps for those three are written from their official documents and from reading the OnAir code, and the parts that can be checked on one computer were checked (marked "checked" below). The rest is untested. If a step does not work, the troubleshooting table at the end is the place to start, and please tell us on GitHub what happened.
* **Which OnAir version do you have?** This matters:
  * **OnAir 3.2 or newer:** everything in this guide works, including the browser page (it is inside the download) and listing on web.on-air.dev.
  * **OnAir 3.1:** you can host a receiver. Two things are different: the browser page is not inside the 3.1 download (see the box in step 3), and you **cannot list on web.on-air.dev yet**, because the 3.1 download has no HTTPS support built in for that. Listing is coming in 3.2. Do not try to build OnAir from source for this; wait for 3.2.

## What you will do, in one picture

```
 your radio ---USB---> your computer running OnAir  --->  a "tunnel" or "proxy"  --->  the internet
                       (onair-server, port 8073)          (adds the padlock, HTTPS)     (listeners' browsers)
```

1. Install OnAir and plug in the radio.
2. Start the server and try it on your own computer.
3. Decide who may listen (a password).
4. Make it reachable from the internet. You choose one of three ways. The first one is the easiest and needs no changes to your router.
5. Make it start by itself.
6. Optional: list it on web.on-air.dev.

## Words you will see (short explanations)

* **Terminal** (or "command line"): a window where you type a command and press Enter instead of clicking. It looks plain on purpose. How to open one is in each step. To copy a command into it, copy the text from this page and paste (Windows terminal: right-click or Ctrl+V; Mac: Cmd+V).
* **Server:** a program that waits for other programs (here: web browsers) to connect and asks them nothing. Your computer is the "server" while `onair-server` runs.
* **IP address:** the number that identifies a computer on a network, like `192.168.1.20`. Addresses starting with `192.168.`, `10.` or `172.16.` to `172.31.` are *private*: they work only inside your home.
* **Port:** a numbered door on a computer. One program listens behind each door. OnAir Web uses door **8073**. The private control page uses door **8765**.
* **Router:** the box from your internet provider. It connects your home to the internet and hides your computers behind one public address.
* **HTTP and HTTPS:** how a browser talks to a web page. HTTPS is the same, but locked (the padlock in the address bar), so nobody on the way can read passwords. A page on the internet **must** use HTTPS.
* **Domain name:** a name like `radio.example.org`. You buy one from a registrar for a few euros a year. You do not need one for the easiest way in this guide.
* **Tunnel / proxy:** a helper program. Visitors talk to the helper; the helper talks to OnAir on your computer. The helper provides the HTTPS padlock for you.
* **Settings file:** a plain text file where OnAir keeps its settings. You can open it with Notepad (Windows), TextEdit (Mac) or nano (Linux).

## Before you start: a checklist

* A computer that can stay on while people listen: Windows 10 or 11, a Mac, a Linux PC, or a Raspberry Pi 4 or newer (64-bit). The Raspberry Pi is untested.
* A radio OnAir supports, plugged into that computer. No radio yet? You can do the whole guide with the built-in **test signal** (`--device test`), but a listing with the test signal is pointless. Use it to practise.
* An internet connection. Each listener uses roughly **0.1 to 0.2 Mbit/s of your upload speed** (the speed "up" in a speed test, the smaller number). Ten listeners need about 2 Mbit/s upload. Look up your upload speed at https://fast.com or https://speedtest.net.
* About 30 to 60 minutes.

## Safe defaults you get for free

* Listeners can only **listen**. Nobody can transmit, change your radio's frequency or gain, or touch your files.
* The private **control page** (door 8765) stays on your own computer unless you change it. This guide never opens it to the internet.
* With the settings used below, OnAir Web is not reachable from the other computers in your home either, until you put the HTTPS helper in front of it. That is on purpose.

---

## Step 1. Install OnAir

Get the installer from https://github.com/adswill/OnAir/releases (the newest release at the top; click "Assets" if the list is folded).

**Windows**

1. Download `OnAir-<version>-windows-x64-setup.exe`.
2. Double-click it. If Windows shows a blue "Windows protected your PC" box, click **More info**, then **Run anyway**. (OnAir is not signed with a paid certificate.)
3. Click Next until it finishes.
4. You should see **OnAir** and **OnAir Web server** in the Start menu. The program you need is `C:\Program Files\OnAir\onair-server.exe`.
5. If a USB radio is not found later, it needs a driver once. The README in the install folder says which one for your radio (for HackRF, RTL-SDR and Airspy it is WinUSB, installed with a tool called Zadig).

**Mac**

1. Download `OnAir-<version>-macos-arm64.dmg` (Apple chip: M1, M2, M3 or newer) or `OnAir-<version>-macos-x86_64.dmg` (older Intel Mac). Not sure? Apple menu, "About This Mac": "Chip: Apple ..." means arm64.
2. Open the .dmg and drag **OnAir** into **Applications**.
3. Open **OnAir** once from Applications. The first time, macOS says it is from an unidentified developer. Close that box, open **System Settings, Privacy & Security**, scroll down and click **Open Anyway** next to OnAir. This is needed once.
4. Quit OnAir (Cmd+Q).
5. The program you need is inside the app: `/Applications/OnAir.app/Contents/MacOS/onair-server`.

**Linux (Debian, Ubuntu, Mint) and Raspberry Pi OS**

1. Download `onair_<version>_amd64.deb` (normal PC) or `onair_<version>_arm64.deb` (Raspberry Pi 4 or 5 with a 64-bit system; to check, type `uname -m` in a terminal: `aarch64` means arm64, `x86_64` means amd64).
2. Open a terminal (on Raspberry Pi OS and most Linux desktops: press Ctrl+Alt+T). Go to the folder where the file is and install it. If the file is in your Downloads folder:
   ```
   cd ~/Downloads
   sudo apt install ./onair_*.deb
   ```
   Type your password when asked (nothing shows while you type; that is normal) and press Enter.
3. You should see a long list of packages being set up and no word "error" at the end. Check with:
   ```
   onair-server --version
   ```
   It prints `onair-server` and the version.
4. Other Linux systems: use `onair-<version>-linux-<arch>-portable.tar.gz`. Unpack it (`tar xzf onair-*-portable.tar.gz`) and use `./bin/onair-server` inside the folder in place of `onair-server` everywhere below.

If the version is 3.1, read the box in step 3 before you go on.

---

## Step 2. Start the server and look at it on your own computer

This step uses the built-in test signal, so it works with or without a radio.

1. Open a terminal:
   * **Windows:** press the Windows key, type `cmd`, press Enter. A black window opens.
   * **Mac:** press Cmd+Space, type `Terminal`, press Enter.
   * **Linux and Raspberry Pi:** Ctrl+Alt+T.
2. Type this command and press Enter. Use the line for your system:

   Windows:
   ```
   "C:\Program Files\OnAir\onair-server.exe" --web --device test
   ```
   Mac:
   ```
   /Applications/OnAir.app/Contents/MacOS/onair-server --web --device test
   ```
   Linux and Raspberry Pi (from the .deb):
   ```
   onair-server --web --device test
   ```
3. You should see lines like these (the radio-library lines before them can be ignored; the numbers differ):
   ```
   onair-server: OnAir Web admin token made (web_admin_token in /home/you/.config/onair/server.conf)
   onair-server: listening on this computer only: http://127.0.0.1:8765/ (use --lan to let other computers in)
   onair-server: OnAir Web on http://127.0.0.1:8073/ (open access, at most 8 listeners)
   ```
   `127.0.0.1` is the address that means "this computer". The window does not give the prompt back: the server is running. Leave it open.
4. Open a web browser **on the same computer** and go to `http://127.0.0.1:8073/`
   * You should see the OnAir Web page with a spectrum and waterfall.
   * If you see only a plain page that says "its browser interface is not installed", your OnAir is 3.1: read the box below.
5. Click on the spectrum to tune, and press the listen / audio button. With the test signal you hear the test tones. (On a phone or another computer, this address will not work yet. That is correct.)
6. To stop the server, click the terminal window and press **Ctrl+C**.
7. **If port 8765 or 8073 is "in use":** OnAir (the desktop app) or another copy of the server is already running. Quit it, or add `--port 18765 --web-port 18073` to the command.

The command you typed was saved in the settings file. Where it is:

| System | Settings file |
|---|---|
| Windows | `%APPDATA%\OnAir\server.conf` (type that into the Explorer address bar and press Enter) |
| Mac | `~/.config/onair/server.conf`, that is a folder named `.config` inside your home folder (hidden: in Finder press Cmd+Shift+G and paste the path) |
| Linux, Raspberry Pi (run by hand) | `~/.config/onair/server.conf` |
| Linux, Raspberry Pi (run as a service, step 6) | `/var/lib/onair/server.conf` |

It contains lines such as `web=1`, `device=test` and a secret `web_admin_token=...`. **Never post or share this file.**

> **Box for OnAir 3.1 only: the browser page is missing.** The 3.1 download contains the server but not the files of its web page, so the page says the interface is not installed. 3.2 fixes this. To use 3.1 anyway: on the release page download `onair-3.1.0-source.tar.gz` (or the green "Code" button on GitHub, "Download ZIP"), unpack it, and find the folder `web/onair-web` inside (it has `index.html`). Put that folder somewhere permanent, for example `C:\OnAir\onair-web` or `/home/you/onair-web`, and add this line to the settings file: `web_root=C:\OnAir\onair-web` (use your own path). Restart the server. You do not build anything. If this is too much, wait for 3.2.

---

## Step 3. Decide who may listen

With no password, **anybody who finds the address can listen, and they use your internet connection.** Start with a password. Do this before you open anything to the internet.

1. Make sure the server is stopped (Ctrl+C in its window).
2. Open the settings file from the table in step 2 in a text editor:
   * **Windows:** in the terminal type `notepad %APPDATA%\OnAir\server.conf` and press Enter.
   * **Mac:** `open -e ~/.config/onair/server.conf`
   * **Linux, Raspberry Pi:** `nano ~/.config/onair/server.conf` (save with Ctrl+O then Enter, leave with Ctrl+X).
3. At the end of the file add these lines. Change the password to something of your own, 8 or more characters, that you do not use anywhere else:
   ```
   web_auth=password
   web_password=choose-something-long-here
   web_max_users=6
   web_name=My first OnAir receiver
   ```
   `web_max_users=6` means at most 6 people listen at once. Choose a number your upload speed can carry (see the checklist).
4. Save the file and start the server again with the same command as before (you can leave out `--device test` once you use a real radio, see "Using your real radio" below).
5. Open `http://127.0.0.1:8073/` again. You should now be asked for the password. Enter it; the page opens.
6. If the server starts with the line `web_password must have 8 characters or more`, the password is too short.

**Using your real radio.** Plug it in and, in the settings file, replace the line `device=test` with nothing (delete the line) to use the first radio found, or set `device=HackRF` (a part of the radio's name). Also set where the radio listens: `freq_mhz=145.0` and `rate_msps=2.4` (the middle frequency in MHz and the width of the piece of spectrum in millions of samples a second; visitors can tune anywhere inside that piece). More settings: https://github.com/adswill/OnAir/blob/main/docs/ONAIR-WEB.md

Other ways to control who listens (all in the same document): invitation links for individual people (`web_auth=invite`), limits per listener, and frequency ranges that visitors may not tune.

---

## Step 4. Make it reachable from the internet

Pick **one** way. If you are unsure, take A.

| | A. Tailscale Funnel | B. Cloudflare Tunnel | C. Port forward + Caddy |
|---|---|---|---|
| Cost | free | free tunnel, but you must own a domain (about 5 to 15 euros a year) | a domain or a free dynamic-DNS name |
| Router changes | none | none | yes |
| Works if your provider shares one address between customers (CGNAT) | yes | yes | no |
| Address | `https://<computer>.<name>.ts.net`, free and stable | your own name, `https://radio.yourdomain.com` | your own name |
| Difficulty | easy | medium | hardest |

Every way needs two lines in the settings file. Add them now (stop the server first):

```
web_trust_proxy=1
web_origin=https://YOUR-ADDRESS-FROM-THE-STEPS-BELOW
```

`web_trust_proxy=1` tells OnAir that the helper (the tunnel or Caddy) is on this same computer, so it can tell your listeners apart (otherwise they all look like one visitor and only 2 can listen). `web_origin` is the public address of your page; you write it in after you have it (steps below say when). OnAir 3.2 and newer do not need the `web_origin` line, 3.1 does; it does no harm in 3.2.

**What we verified.** We started the server with `web_trust_proxy=1`, then sent it requests that look exactly like the ones a proxy sends (an `https` origin plus an `X-Forwarded-For` header) from the same computer. In 3.2 they are accepted. In 3.1 the browser connection was refused with `{"error":"this page's origin is not allowed"}` unless `web_origin` was set. This is the reason for the extra line.

Leave `web=1` and do **not** add `web_lan=1`. With the default, OnAir Web listens only on this computer (`127.0.0.1`), and only the helper can reach it. This is the safest setup.

### Way A. Tailscale Funnel (easiest)

Tailscale is a free service (free for personal use). Funnel is its feature that gives a program on your computer a public `https://...ts.net` address, with the padlock, without touching your router. It is called "beta" by Tailscale. Allowed public ports are 443, 8443 and 10000. Tailscale limits the bandwidth of Funnel and does not say how much. Details: https://tailscale.com/kb/1223/funnel

**Install Tailscale and sign in**

* **Windows:** download the installer from https://tailscale.com/download/windows, run it, and sign in when the browser opens (you can use a Google, Microsoft or GitHub account). The Tailscale icon appears near the clock.
* **Mac:** install the Tailscale app from the Mac App Store, or from https://tailscale.com/download/mac. Open it, click **Log in**, and sign in.
* **Linux and Raspberry Pi:** in a terminal:
  ```
  curl -fsSL https://tailscale.com/install.sh | sh
  sudo tailscale up
  ```
  `tailscale up` prints a web address; open it on any computer or phone and sign in.

You should see your computer in the list at https://login.tailscale.com/admin/machines with a green dot.

**Turn on the two switches Funnel needs** (once)

1. Go to https://login.tailscale.com/admin/dns . Make sure **MagicDNS** is on, and click **Enable HTTPS** (below "HTTPS Certificates") if it is not.
2. The first `funnel` command in the next part also prints a link and asks you to allow Funnel for the machine. Open the link and approve. (Funnel is allowed by default for the people in your tailnet; if you see a message that it is not allowed, the "funnel" attribute is missing in your tailnet policy file, see the Tailscale page above.)

**Start the server and publish it**

1. Start the server (see "Start by itself" in step 5 for how to keep it running). In its own terminal: Windows `"C:\Program Files\OnAir\onair-server.exe"`, Mac `/Applications/OnAir.app/Contents/MacOS/onair-server`, Linux `onair-server`. The settings file already holds everything.
2. Open a **second** terminal and type:
   ```
   tailscale funnel --bg 8073
   ```
   (On Windows, if `tailscale` is not found, close the terminal and open a new one. On a Mac with the App Store app the command may be missing; use `/Applications/Tailscale.app/Contents/MacOS/Tailscale funnel --bg 8073`.) On Linux and Raspberry Pi put `sudo` in front.
3. You should see a line like `Available on the internet: https://myhost.tail1234.ts.net/` and that it proxies to `http://127.0.0.1:8073`. That `https://...ts.net` address is **your public address.** Copy it. Check it any time with `tailscale funnel status`.
4. Stop the OnAir server, put that address into the settings file as `web_origin=https://myhost.tail1234.ts.net` (no slash at the end), and start the server again.
5. On your phone, **turn Wi-Fi off** (so that you are on mobile data and really outside your home), open the address in the phone browser, and enter the password. You should see the page and hear audio.
6. To turn the public address off: `tailscale funnel --bg 8073 off`, or `tailscale funnel reset`.

If step 3 prints an error about HTTPS or MagicDNS, redo "Turn on the two switches". If the address works on the computer but not from the phone, wait a minute or two: the name takes time to spread.

**Will the directory accept this address? (checked)** The web.on-air.dev server accepts normal `https://name.tail1234.ts.net` addresses: we ran its address check on such a name and it passed. It also looks the name up in public DNS when you first register, and only accepts names that point at public addresses. A `.ts.net` name has a public address only while Funnel is on, so list your receiver **after** the funnel works. This DNS part could not be tested without a real Funnel.

### Way B. Cloudflare Tunnel (you own a domain on Cloudflare)

You need a domain whose DNS is managed by Cloudflare (free account at https://dash.cloudflare.com ; add your domain there and follow Cloudflare's instructions to move its name servers). Cloudflare's terms limit what you may carry through its network; check them for audio streaming before you rely on this. This way is untested with OnAir.

1. In the Cloudflare dashboard open **Networking, Tunnels** (older dashboards: Zero Trust, Networks, Tunnels). Click **Create a tunnel**, choose **Cloudflared**, give it a name such as `onair`, click **Save tunnel**.
2. The page shows an installer command for your system (Windows, Mac, Debian, Red Hat...). Copy it and run it in a terminal on the computer that runs OnAir (Windows: a terminal as Administrator, press the Windows key, type `cmd`, right-click, Run as administrator). It installs `cloudflared` as a background service that starts with the computer.
3. Back in the dashboard the tunnel should say **Healthy** after a few seconds.
4. Open the tunnel's **Routes** tab (or "Public Hostname"), **Add route, Published application**. Choose a subdomain (for example `radio`) and your domain. Under **Service**, type `http://localhost:8073` . Save.
5. Your public address is `https://radio.yourdomain.com`. Put `web_origin=https://radio.yourdomain.com` and `web_trust_proxy=1` into the settings file and restart the server.
6. Test from a phone on mobile data, as in way A step 5.

### Way C. Router port forward + Caddy (the classic way)

This way needs a **public address that is yours alone.** If your provider uses CGNAT (shared addresses), it cannot work: compare the "WAN" or "Internet" address in your router's status page with what https://whatismyip.com shows. If they differ, or the router address starts with `100.` , use way A or B.

1. **Get a name.** Buy a domain, or get a free dynamic-DNS name (for example from https://www.duckdns.org). Point it at your home's public address. DuckDNS gives a small program or a web page to keep the address updated.
2. **Give the computer a fixed home address.** In your router's settings, find "DHCP reservation" or "static lease" and reserve one address for this computer.
3. **Forward two ports.** In the router, find "Port forwarding" (sometimes "Virtual server" or "NAT"). Forward **TCP ports 80 and 443** to the computer's fixed address, to the same port numbers. Do **not** forward 8073 or 8765.
4. **Install Caddy** (it gets and renews the free HTTPS certificate by itself):
   * **Debian, Ubuntu, Raspberry Pi OS:** follow https://caddyserver.com/docs/install#debian-ubuntu-raspbian (it is six lines to copy); finish with `sudo apt install caddy`. Caddy then runs as a service by itself.
   * **Windows:** with Chocolatey `choco install caddy`, or download from https://caddyserver.com/download .
   * **Mac:** `brew install caddy` (needs Homebrew from https://brew.sh ).
5. **Tell Caddy what to do.** Make a text file called `Caddyfile` (on Debian and Raspberry Pi OS it is `/etc/caddy/Caddyfile`; replace its content) with exactly:
   ```
   radio.example.org {
       reverse_proxy 127.0.0.1:8073
   }
   ```
   Use your own name instead of `radio.example.org`. Restart Caddy: Linux `sudo systemctl restart caddy`; elsewhere run `caddy run --config Caddyfile` in the folder of the file.
6. **Open the firewall** for ports 80 and 443 on the computer if it has one: Windows asks when Caddy first starts (click Allow); Linux with ufw: `sudo ufw allow 80,443/tcp`.
7. Put `web_origin=https://radio.example.org` and `web_trust_proxy=1` into the settings file, restart the server, and test from a phone on mobile data.
8. If Caddy cannot get a certificate, the log says why (Linux: `journalctl -u caddy -e`). Usually the name does not yet point at your address, or port 80 is not forwarded.

---

## Step 5. Start by itself after a restart

Until now you started the server by hand. The server and the helper (Tailscale, cloudflared, Caddy) should both start with the computer. Tailscale and cloudflared and the Linux Caddy package install themselves as background services; you only need to do the OnAir server. These steps are untested.

**Windows (starts when you sign in)**

1. Press Windows+R, type `shell:startup`, press Enter. A folder opens.
2. Right-click in the folder, **New, Shortcut**. For the location paste: `"C:\Program Files\OnAir\onair-server.exe" --web` . Click Next, name it `OnAir Web`, Finish.
3. Restart the computer and sign in. A black window should open by itself and show the OnAir lines. Do not close it (minimise it). Starting without signing in needs Task Scheduler; see https://github.com/adswill/OnAir/blob/main/docs/headless-server.md .

**Mac (starts when you sign in)**

1. Open Terminal and type these two lines (the first makes a small script, the second allows it to run):
   ```
   printf '#!/bin/zsh\n/Applications/OnAir.app/Contents/MacOS/onair-server --web\n' > ~/OnAir-Web.command
   chmod +x ~/OnAir-Web.command
   ```
2. Open **System Settings, General, Login Items**, click **+**, choose `OnAir-Web.command` in your home folder.
3. Restart and sign in. A Terminal window should open by itself and show the OnAir lines. Also, in System Settings, General, Energy (or Battery), switch off sleeping of the Mac while it is plugged in, or the radio goes quiet when it sleeps.

**Linux and Raspberry Pi (starts at boot, no login needed)**

The .deb already contains a service file, switched off. Switch it on:

1. Make a user for it (this user may use USB radios) and a place for its settings:
   ```
   sudo useradd --system --home /var/lib/onair --create-home --groups plugdev onair
   ```
2. Make its settings file. Replace the values with yours (the same lines as in steps 3 and 4; the file is **not** the one in your home folder):
   ```
   sudo tee /var/lib/onair/server.conf > /dev/null <<'EOF'
   web=1
   web_auth=password
   web_password=choose-something-long-here
   web_max_users=6
   web_name=My OnAir receiver
   web_trust_proxy=1
   web_origin=https://YOUR-ADDRESS
   freq_mhz=145.0
   rate_msps=2.4
   autostart=1
   EOF
   sudo chown onair:onair /var/lib/onair/server.conf
   sudo chmod 600 /var/lib/onair/server.conf
   ```
3. Start it and make it start at boot:
   ```
   sudo systemctl enable --now onair-server
   ```
4. See if it runs: `systemctl status onair-server` should say `active (running)` in green. The log: `journalctl -u onair-server -e`.
5. Open the radio's permissions once if the log says no radio is found: unplug and replug the radio, and check that you installed from the .deb (it brings the USB rules). Reboot once to be sure.

---

## Step 6. Safety, bandwidth and the law (read this)

* **Password or invitations first.** Open access means anyone on Earth. Start with `web_auth=password`, and think before you remove it.
* **Your upload.** Every listener is a stream of data from your home. Keep `web_max_users` low (see the checklist). If your video calls start to stutter, lower it. There are also caps `web_max_kbps` (all listeners together) and `web_user_kbps` (each).
* **Keep the control page private.** Port 8765 can change your radio. Never forward it, and never put it behind the helper. This guide does not.
* **The law is yours to check.** By running this you make what your radio hears available to other people. In many countries, receiving some things is allowed but passing them on is not (private messages, police and military radio, mobile phones, in some countries air traffic). Check the rules where you live. By default OnAir Web switches digital voice and pager decoders off for listeners for this reason. Read "Host responsibility": https://github.com/adswill/OnAir/blob/main/docs/ONAIR-WEB.md#host-responsibility
* **Updates.** Update OnAir when new versions come out, and keep Tailscale, Caddy or cloudflared updated.
* Listeners' addresses are not written to the log unless you set `web_log_ip=1`.

---

## Step 7. List your receiver on web.on-air.dev (OnAir 3.2 or newer)

> **OnAir 3.1 only.** The OnAir 3.1 download cannot talk to the directory (it has no HTTPS support built in), so listing does not work there. If you have 3.1, update to 3.2 or newer first. Please do not build from source for this.

The list is at https://web.on-air.dev . It is optional, switched off by default, and nobody has to approve you. Your server signs each message with its own key, so nobody else can change your entry. It sends your name, description, public address, a rough location (you choose how rough), what your radio covers and the **number** of listeners, every 5 minutes. It never sends listener addresses or what they listen to.

Your address must work first (step 4), and must be an `https://` address.

1. Stop the server. Open the settings file and add (use your own values):
   ```
   web_directory=1
   web_directory_url=https://web.on-air.dev
   web_public_url=https://myhost.tail1234.ts.net
   web_directory_radio=HackRF Pro
   web_directory_desc=Wideband receiver on the roof
   web_directory_loc=54.69,25.28
   web_directory_loc_decimals=1
   ```
   * Do not use `directory.on-air.dev`; that name does not exist. Use exactly `https://web.on-air.dev`.
   * `web_public_url` is your public address from step 4.
   * `web_directory_loc` is optional. Leave the line out for no location. Use latitude,longitude, or a locator like `KO24pq`. `web_directory_loc_decimals=1` makes it accurate to about 10 km; `2` to about 1 km.
2. Start the server. You should see something like this. **Nothing has been sent yet.**
   ```
   onair-server: OnAir Web directory: every 300 s this server will send an HTTPS POST to
     https://web.on-air.dev/api/v1/heartbeat
   with exactly this JSON (time, users and capture coverage change at each beat):
     {"v":1,"action":"heartbeat","id":"onair-1bd2c82b27d6299b", ... "name":"OnAir Web","url":"https://myhost.tail1234.ts.net/", ... }
   Nothing else is sent: no listener addresses, names or what anyone listens to, only the number of users.
   onair-server: web_directory is on, but nothing is sent yet: to confirm exactly this, add  web_directory_ack=34022415bce2  to the settings file
   ```
3. **Read the preview.** If you do not want to send any part of it, stop here and change the settings.
4. To agree, copy the code after `web_directory_ack=` (yours is different from the example) into the settings file as a new line `web_directory_ack=34022415bce2`, and restart the server.
5. Wait a minute or two, then reload https://web.on-air.dev . Your receiver should be in the list. If you change the name, address, location, radio, description or the directory address later, the code changes and you must read the preview and confirm again.
6. A key file `web_directory.key` is made next to the settings file. **Keep it.** It proves that the listing is yours. If you lose it, the old listing disappears by itself after about 15 minutes and you get a new one.

If something does not appear, read the troubleshooting table below.

### Remove your listing

* Quick: stop the server. After about 15 minutes the entry disappears by itself.
* Right now: run `onair-server --directory-remove` (the same program as before, for example `"C:\Program Files\OnAir\onair-server.exe" --directory-remove`). It sends a signed "delete" and exits.
* Or set `web_directory=0` in the settings file. The entry then expires by itself.

---

## Troubleshooting

| What you see | Likely cause | What to do |
|---|---|---|
| The browser says "this site can't be reached" on `http://127.0.0.1:8073/` | The server is not running, or you started it with another port | Look at the terminal window; start the server again; read the line `OnAir Web on http://...:8073/` for the real port |
| `cannot listen on TCP port 8765 (in use by another program?)` | The OnAir desktop app or another server is running | Quit it, or start with `--port 18765 --web-port 18073` |
| The page says the "browser interface is not installed" | OnAir 3.1 (page not in the download) | Do the box in step 2, or update to 3.2 |
| Page opens at home but not from the phone on mobile data | The helper is not running, or the address was typed wrong | Check `tailscale funnel status`, the Cloudflare tunnel's status, or Caddy's log; copy the address again |
| Browser shows a red warning about the certificate | Self-made certificate, or the name has no certificate yet | With Tailscale, Cloudflare or Caddy this should not happen: wait a few minutes; check that HTTPS is enabled (Tailscale) or that ports 80 and 443 are forwarded (Caddy) |
| The page opens but never shows the spectrum ("connecting...") | The page's address is not allowed to connect | Add `web_origin=https://your-address` and `web_trust_proxy=1` to the settings file, restart. In the server's log or with curl the answer is `this page's origin is not allowed` |
| `this server needs HTTPS for listeners from the internet` | The listener came in over plain HTTP from the internet | Use the `https://` address; with a helper make sure `web_trust_proxy=1` is set |
| Only two people can listen, then "busy" | All visitors look like one address | Set `web_trust_proxy=1`; or raise `web_max_per_ip` |
| "busy" before the limit | The CPU or bandwidth caps | Lower the number of listeners, or raise `web_max_kbps`; see ONAIR-WEB.md |
| `web_password must have 8 characters or more` | Password too short | Choose a longer one |
| `no TLS, so only private (LAN) addresses are let in` | You set `web_lan=1` and no HTTPS | Expected on a home network. For the internet use a helper (step 4) and leave `web_lan` out |
| `web_directory needs HTTPS, and this build has no TLS support` | OnAir 3.1 or a self-built program without TLS | Update to 3.2 |
| `no trusted certificate authorities found on this computer` | The computer has no list of trusted certificate authorities | Set `SSL_CERT_FILE` to a CA bundle `.pem` file (rare; Windows and Mac use the system list automatically in 3.2) |
| `TLS handshake failed ... certificate verify failed` when starting the directory | The computer's clock is wrong, or a company/antivirus program replaces certificates | Fix the date and time; try another network |
| Listing is not in the list after 10 minutes | You did not add `web_directory_ack=...`, or the directory refused the address | Read the log for the line `web_directory is on, but nothing is sent yet`; the address must be `https://`, a real public name, port 443 (or 8443 or 10000 with Funnel), no `.local` or `.lan` names |
| Server stops when you close the terminal | You started it by hand | Do step 5 |
| Radio not found on Linux | USB permission | Install from the .deb, replug the radio, make sure the `onair` user is in `plugdev` (step 5) |
| Radio not found on Windows | The driver is missing | See the README in the install folder (Zadig, WinUSB) |

If you are stuck, open an issue at https://github.com/adswill/OnAir/issues and include: your OnAir version, your system, which way in step 4 you used, and the terminal lines (**remove** `web_password`, `web_admin_token` and any key before you post).

## More reading

* All settings and the admin API: https://github.com/adswill/OnAir/blob/main/docs/ONAIR-WEB.md
* The headless server: https://github.com/adswill/OnAir/blob/main/docs/headless-server.md
* How the public list works and what it stores: https://github.com/adswill/OnAir/blob/main/docs/ONAIR-DIRECTORY.md
