# Troubleshooting

<details>
<summary><b>Common problems and where to look</b></summary>

- **The radio is not listed.** Windows: install the WinUSB driver once with [Zadig](https://zadig.akeo.ie), see [Install](INSTALL.md#install). SDRplay RSPs need the SDRplay API installed first. Notes per radio are in [DEVICES.md](../DEVICES.md).
- **The picture breaks up.** Use **Auto-tune** or **Optimise gain**, and move the antenna while you watch the quality score, see [Quick start](../README.md#quick-start). A weak or echo-filled signal is the usual cause.
- **macOS will not open the app.** It is not signed with a developer certificate yet: right-click it and choose *Open* on the first launch.
- **No voice in DMR, P25, NXDN, dPMR or D-STAR.** Install mbelib yourself, see [Good to know](GOOD-TO-KNOW.md) and [docs/CODECS.md](CODECS.md). Built codecs: join the [Discord](https://discord.gg/ptxkzCuzR), the Dropbox link with the files is posted there.
- **No radio at all.** Open an IQ recording through the *IQ recording file* source, or press **Tour**.
- **Still stuck.** Press **Save bug report** (one zip with diagnostics, log and crash report) and send it on [Discord](https://discord.gg/ptxkzCuzR) or in [GitHub Issues](https://github.com/adswill/OnAir/issues).
</details>

## Reporting a bug

Open a [GitHub issue](https://github.com/adswill/OnAir/issues). bug reports. Attach an IQ recording of the problem: press **Record IQ** in the Source panel (or run `onair-cli --record auto --secs 10`); files go to the `recordings` folder of the app data folder, and **Show recording** opens it. With **SigMF** ticked (the default) every recording also gets a `.sigmf-meta` file, so it opens directly in [IQEngine](https://iqengine.org). **Save bug report** makes one zip with diagnostics, log and crash report.
