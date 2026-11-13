# Updates and Discord status

OnAir checks GitHub for a newer version a few seconds after it starts (then at most once a day), downloads the package for your system in the background, verifies it against the SHA-256 that GitHub lists, and installs it when you close the program.

- The version button at the top right shows the state and has the settings: turn the check off, turn automatic install off (you then get a button instead), or leave out pre-releases.
- macOS replaces `OnAir.app`; Windows runs the installer quietly (Windows asks for permission); Linux replaces the portable folder or the AppImage in place; a `.deb` install is downloaded and checked, and OnAir shows the `sudo apt install` command to run.
- Only github.com is contacted, and nothing is sent but the usual request. If GitHub is blocked or very slow where you are (for example in China), see **[docs/UPDATE-MIRROR.md](UPDATE-MIRROR.md)**: an optional, off-by-default mirror for the update check and the download, with the same checksum checks.

## Discord status

Off by default. **View > Show in Discord** shows on your Discord profile that you are using OnAir, the mode and (optionally, **Show the frequency**) the frequency. It needs the Discord desktop app running, talks only to it on your computer, and sends nothing else anywhere. To use your own application in a fork: set the environment variable `ONAIR_DISCORD_APP_ID` (or `kDiscordAppId` in `app/platform/discord_rpc.cpp`) and upload the icon as a Rich Presence art asset named `onair`.
