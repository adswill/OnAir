# Quick CI: check a branch on GitHub runners

Push your branch, then run `tools/quick_ci.sh <branch> '<ctest -R regex>' [targets...] [--os macos-latest]`. It dispatches
`quick.yml`, which builds only the targets those tests run (or the ones you name), runs only the matching tests, and the script
waits (one API call every 45 s), prints a summary (passed/failed counts, failing tests, first compile errors with file:line,
times, ccache hit rate) and exits 0 on success. `tools/quick_ci.sh <branch> --full` runs `quick-full.yml`: one build of
everything, then 7 fast test shards plus 1 slow shard in parallel, merged into one summary (for the integration branch).
Nothing is built on the Mac. The full logs are the `quick-logs` artifact of the run (kept 3 days).

## Until it is on main
GitHub only dispatches a `workflow_dispatch` workflow that exists on the default branch: dispatching `quick.yml` from the branch
`quick-ci-030` returns HTTP 404 (tried, both with `gh workflow run` and the REST API). So the script works only AFTER `quick-ci-030`
is merged into main (owner action). Until then the workflows carry a temporary `push` trigger that runs them on every push to
`quick-ci-030` with defaults (tests `^modes$`); remove that trigger after the merge. The helper scripts (`tools/quick_ci/`) are taken
from the commit the workflow runs from, so the tested branch needs no copy of them. `--via <ref>` picks another branch for the
workflow file once main has it.

## Details
- Inputs of `quick.yml`: `ref`, `tests` (required), `targets` (empty: found from the tests' commands with `ctest --show-only=json-v1`),
  `os` (`ubuntu-24.04` default, `macos-latest`, `ubuntu-24.04-arm`), `jobs`, `timeout` (per test, default 900 s).
- Build: Ninja, Release, `DECT2_BUILD_APP=OFF` (tools and tests only, no GUI libraries), same dependencies as `ci-full.yml`.
  Linting, docs and packaging are skipped. A target list that does not match the tests can be given by hand.
- Caches: ccache (key os + hash of CMake files + run, restore falls back to the newest cache of the same os; shared by quick and
  quick-full, not with ci.yml) and the cargo registry. The Rust layers build from workspace sources, so no cargo target cache.
- Concurrency: one run per `ref` at a time (a new dispatch for the same ref cancels the older); other refs run in parallel.
- Cancel with `gh run cancel <id>`. If the script times out (`--max-wait`, default 90 min) it prints the run id and exits 3.

## Limits and cost
Free for a public repository (no minutes charged). GitHub Free plan limits: 20 concurrent jobs on Linux and Windows in total, 5 for
macOS (so macOS runs queue first; use it only for platform-specific checks), 6 h per job (the workflows stop at 90/120 min), 500 MB..10 GB
caches per repo (the oldest are evicted; the cache is shared by every branch, so the same-os fallback is usually warm), artifacts kept
1-3 days. Each quick-full run uses 10 jobs; do not start many at once. API use: one `gh run view` per 45 s per waiting script.

## Relation to the laptop worker
`tools/remote-build/` (branch `pub/remote-build-030`) is the other way to take builds off agents' shoulders: a worker on the owner's
laptop. Quick CI needs a pushed branch and the internet, takes a few minutes for cold queueing but runs any number of branches in
parallel; the laptop worker is for runs that need local hardware or unpushed work. They do not depend on each other.
