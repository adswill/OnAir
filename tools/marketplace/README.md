# OnAir plugin registry tools

A registry is a folder of static files; OnAir's Store reads it over HTTPS (or from a local folder). No server, no database. Design and trust model: `docs/PLUGIN-MARKETPLACE-3.9.md` in the OnAir repository.

- `onair_registry.py`: lint, build the index, sign, verify (Python 3.8+, standard library only; its Ed25519 passes RFC 8032 test 1 on every start).
- `sample-registry/`: a complete example registry with the three SDK example plugins as **stub** packages (their "library" is a text file) and an index signed with a **published test key**. For tests and as a template. Never put its key into a real build. Regenerate with `make_sample_registry.py`.
- `sample-registry/POLICY.md`, `CONTRIBUTING.md`, `.github/workflows/validate.yml`: copy them into the real registry repository.

Registry layout:

```
index.json            made by `index`, signed
index.json.sig        128 hex digits, Ed25519 over the exact bytes of index.json
revoked.json          optional: [{"id", "version", "reason"}]  (merged into index.json)
plugins/<id>/plugin.json   per-plugin page: description, links, downloads per platform (url, size, sha256)
packages/...          optional: packages hosted in the registry itself (a plugin.json may point to any https URL instead)
tools/onair_registry.py    copy of the tool (for the CI workflow)
```

Maintainer routine: merge a PR, then

```
python3 tools/onair_registry.py lint . --fetch
python3 tools/onair_registry.py index . --serial <last serial + 1>
python3 tools/onair_registry.py sign . /path/to/offline/onair-registry.key
python3 tools/onair_registry.py verify . <public key hex>
git add -A && git commit -m "registry: serial N" && git push
```

The serial must rise with every published index; OnAir refuses an index with a lower serial than one it has seen (replay protection).
