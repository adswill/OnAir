# Adding or updating a plugin

1. Build your plugin and publish a `.onairplugin` per platform (zip with `manifest.json` and the library) as release assets of your own repository. Platforms: `windows-x64`, `macos-arm64`, `macos-x86_64`, `linux-x86_64`, `linux-aarch64`.
2. Fork this repository. Add `plugins/<id>/plugin.json` (copy another one). For each platform put `url` (https), `size` and `sha256` (`python3 tools/onair_registry.py artefact <file>` prints the last two). `id` must equal the plugin's manifest id and the folder name.
3. Run `python3 tools/onair_registry.py lint . --fetch`. Fix every ERROR.
4. Open a pull request. CI runs the same validator. A maintainer reads the page and (for `reviewed`) the source, then merges. Do not edit `index.json`, `index.json.sig` or `review`: the maintainer builds, signs and sets them.
5. An update is a pull request that raises `version` and replaces the downloads. Versions never go down.

Do not put secrets or keys into a pull request. See `POLICY.md`.
