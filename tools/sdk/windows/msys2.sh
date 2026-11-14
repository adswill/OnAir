# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# windows/msys2.sh - sourced by windows/fetch_sysroot.sh: MSYS2's repositories, the trust in
# MSYS2's signatures, and cached downloads. The caller defines die(), say(), TMP (a private temporary folder), CACHE
# (the download cache) and GNUPGHOME (an empty gpg home inside TMP).

# shellcheck disable=SC2034  # used by fetch_sysroot.sh, which sources this file
MINGW_REPO=https://repo.msys2.org/mingw/ucrt64           # ucrt64.db + the packages
MSYS_REPO=https://repo.msys2.org/msys/x86_64             # msys.db: only to find msys2-keyring
# MSYS2's master keys (msys2-keyring, file msys2-trusted). A packager key is trusted when one of these certified it.
MASTER_KEYS=(
  D55E7A6D7CE9BA1587C0ACACF40D263ECA25678A
  B91BCF3303284BF90CC043CA9F418C233E652008
  9DD0D4217D75A33B896159E6DA7EF2ABAEEA755C
  6E8FEAFF9644F54EED90EEA0790AE56A1D3CFDDC
  69985C5EB351011C78DF7F6D755B8182ACD22879
)

download() {   # url file: download to file (via file.part)
  curl -fsSL --retry 6 --retry-delay 5 --retry-all-errors --connect-timeout 30 -o "$2.part" "$1" || { rm -f "$2.part"; die "download failed: $1"; }
  mv "$2.part" "$2"
}
cached() {     # url-folder file sha256: the file in $CACHE, downloaded when missing or wrong; checks the sha256; also file.sig
  local f="$CACHE/$2"
  if [ ! -f "$f" ] || [ "$(sha256sum "$f" | cut -d' ' -f1)" != "$3" ]; then
    download "$1/$2" "$f"
    [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$3" ] || { rm -f "$f"; die "$2: sha256 differs from the lock file / repository database"; }
  fi
  [ -f "$f.sig" ] || download "$1/$2.sig" "$f.sig"
}
msys2_verify() {   # file: its .sig must be a good signature by a trusted MSYS2 key (msys2_trust first)
  gpgv --keyring "$TMP/trusted.gpg" "$1.sig" "$1" >/dev/null 2>&1 || die "bad or unknown signature: $1"
}
desc_field() { sed -n "/^%$2%\$/{n;p;q}" "$1"; }

msys2_latest_keyring() {   # sets KEYRING_FILE and KEYRING_SHA to today's msys2-keyring package
  say "reading msys.db (for msys2-keyring)"
  download "$MSYS_REPO/msys.db" "$TMP/msys.db"
  mkdir -p "$TMP/msysdb"; tar -xf "$TMP/msys.db" -C "$TMP/msysdb"
  local d; d=$(ls -d "$TMP"/msysdb/msys2-keyring-[0-9]* | sort -V | tail -1)
  # shellcheck disable=SC2034  # read by fetch_sysroot.sh, which sources this file
  KEYRING_FILE=$(desc_field "$d/desc" FILENAME); KEYRING_SHA=$(desc_field "$d/desc" SHA256SUM)
}

msys2_trust() {   # keyring-file sha256: $TMP/trusted.gpg = the master keys pinned above + the packager keys they certified
  local kfile=$1 ksha=$2 kr mids revoked m
  say "keyring $kfile"
  cached "$MSYS_REPO" "$kfile" "$ksha"
  mkdir -p "$TMP/keyring"; tar -xf "$CACHE/$kfile" -C "$TMP/keyring" usr/share/pacman/keyrings
  kr="$TMP/keyring/usr/share/pacman/keyrings"
  gpg -q --batch --import "$kr/msys2.gpg" 2>/dev/null
  mids=$(for m in "${MASTER_KEYS[@]}"; do echo "${m: -16}"; done | tr '\n' ' ')
  revoked=$(cut -d: -f1 "$kr/msys2-revoked" 2>/dev/null | tr '\n' ' ')
  mapfile -t TRUSTED < <(gpg --batch --with-colons --check-sigs 2>/dev/null | awk -F: -v mids="$mids" -v rev="$revoked" '
    BEGIN { n = split(mids, a, " "); for (i = 1; i <= n; i++) M[a[i]] = 1; n = split(rev, b, " "); for (i = 1; i <= n; i++) R[b[i]] = 1 }
    /^pub/ { want = 1; next }
    /^fpr/ && want { cur = $10; want = 0; next }
    /^sig/ && $2 == "!" && ($5 in M) && !(cur in R) { ok[cur] = 1 }
    END { for (k in ok) print k }')
  for m in "${MASTER_KEYS[@]}"; do printf '%s\n' "${TRUSTED[@]}" | grep -qx "$m" || die "master key $m is not in $kfile (or not self-signed)"; done
  gpg -q --batch --export "${TRUSTED[@]}" > "$TMP/trusted.gpg"
  msys2_verify "$CACHE/$kfile"
  say "${#TRUSTED[@]} trusted keys (${#MASTER_KEYS[@]} master keys and the packager keys they certified)"
}
