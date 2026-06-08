#!/usr/bin/env bash
# ir-crypt.sh — encrypt / decrypt the proprietary cab IRs.
#
# The measured Box of Rules cab impulse responses (ir/*.wav) are proprietary and
# are NEVER committed in plaintext. The repo holds only the encrypted blob
# ir/ir-pack.tar.gz.enc. This script packs/unpacks it.
#
# The passphrase comes from $BOR_IR_KEY (CI sets it from the repo secret of the
# same name; locally, export it or keep it in a file you `source`). openssl is the
# only dependency and ships on macOS, Linux, and the Git-Bash that GitHub's Windows
# runners use, so this one script works on every platform via `shell: bash`.
#
#   scripts/ir-crypt.sh decrypt   # ir-pack.tar.gz.enc -> ir/*.wav  (run before cmake)
#   scripts/ir-crypt.sh encrypt   # ir/*.wav -> ir-pack.tar.gz.enc  (after changing IRs)
set -euo pipefail

cmd="${1:-}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ir="$here/ir"
blob="$ir/ir-pack.tar.gz.enc"
iter=200000

if [[ -z "${BOR_IR_KEY:-}" ]]; then
  echo "ERROR: BOR_IR_KEY is not set." >&2
  echo "  CI: add it as a repo secret. Local: export BOR_IR_KEY=... (see ../.bass-betterer-ir-key)." >&2
  exit 1
fi

case "$cmd" in
  decrypt)
    [[ -f "$blob" ]] || { echo "ERROR: missing $blob" >&2; exit 1; }
    openssl enc -d -aes-256-cbc -pbkdf2 -iter "$iter" -pass "pass:$BOR_IR_KEY" -in "$blob" \
      | tar -xzf - -C "$ir"
    echo "Decrypted $(cd "$ir" && ls *.wav | wc -l | tr -d ' ') IR wav(s) into $ir"
    ;;
  encrypt)
    cd "$ir"
    wavs=(*.wav)
    [[ -e "${wavs[0]}" ]] || { echo "ERROR: no ir/*.wav to encrypt" >&2; exit 1; }
    tar -czf - "${wavs[@]}" \
      | openssl enc -aes-256-cbc -pbkdf2 -iter "$iter" -salt -pass "pass:$BOR_IR_KEY" -out "$blob"
    echo "Encrypted ${#wavs[@]} IR wav(s) -> $blob"
    ;;
  *)
    echo "usage: $0 {decrypt|encrypt}" >&2
    exit 2
    ;;
esac
