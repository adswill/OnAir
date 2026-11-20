# Code signing and notarisation (release builds)

Release builds are signed only when the repository has the secrets below. Without them nothing changes: macOS builds stay ad hoc signed (the "unidentified developer" prompt) and Windows builds are unsigned. Nothing is bought or created by the code; the certificates are yours to get.

## macOS (Developer ID and notarisation)

Needs an Apple Developer Program membership (paid, yearly). Create a **Developer ID Application** certificate, export it with its private key as a `.p12`, and an App Store Connect API key (`.p8`) for notarisation.

| Secret (Settings > Secrets and variables > Actions) | Value |
|---|---|
| `MACOS_CERT_P12_BASE64` | the `.p12`, base64 (`base64 -i cert.p12`) |
| `MACOS_CERT_PASSWORD` | the `.p12` password |
| `MACOS_SIGN_IDENTITY` | the identity name, e.g. `Developer ID Application: Your Name (TEAMID)` |
| `NOTARY_API_KEY_P8_BASE64` | the API key `.p8`, base64 |
| `NOTARY_KEY_ID` | the API key id |
| `NOTARY_ISSUER_ID` | the issuer id |

`release.yml` imports the certificate into a temporary keychain and `tools/package/make_dmg.sh` signs every library, helper and the app with the hardened runtime (`packaging/macos/entitlements.plist`), signs the disk image, then notarises and staples it. With the first three secrets only, it signs but does not notarise.

## Windows (Authenticode)

Needs a code-signing certificate from a certificate authority (an EV certificate or a cloud-signing service removes the SmartScreen warning fastest; a standard one builds reputation over time). Export it as `.pfx`.

| Secret | Value |
|---|---|
| `WIN_SIGN_PFX_BASE64` | the `.pfx`, base64 |
| `WIN_SIGN_PFX_PASSWORD` | its password |

`tools/release/sign_windows.sh` (osslsigncode) signs the programs and DLLs, the zip is rebuilt from them, and the installer is signed last. If your certificate lives in a hardware token or a cloud HSM that cannot be exported as `.pfx`, this step needs a different signer; say so and it can be changed.

## Not tested

The scripts were written without the certificates, so the first signed run is the test. Run `release.yml` by hand on a branch with the secrets set, check `codesign -dvv`, `spctl -a -vv` and `signtool verify /pa`, and fix what the first run shows.
