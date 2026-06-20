#!/usr/bin/env bash
# build-dmg.sh — wrap the built AU + VST3 in a proper macOS installer .pkg, inside a .dmg.
#
#   packaging/macos/build-dmg.sh [ARTEFACTS_DIR] [OUT_DMG] [VERSION]
#
# The .pkg installs to the system plug-in folders (/Library/Audio/Plug-Ins/{Components,VST3})
# — the installer asks for an admin password.
#
# Signing / notarization is driven entirely by env vars. With none set it falls back to
# ad-hoc signing + no notarization (fine for local iteration; Gatekeeper will block first
# open). Set these (CI does, from repo secrets) to ship a signed + notarized build with no
# Gatekeeper warning:
#
#   BOR_CODESIGN_ID    codesign identity for the bundles, e.g. "Developer ID Application"
#                      (a prefix is fine when exactly one matching identity is in the
#                      keychain search list). Unset/empty -> ad-hoc "-".
#   BOR_INSTALLER_ID   identity for the .pkg, e.g. "Developer ID Installer". Unset -> pkg
#                      is left unsigned (only valid alongside an unset BOR_CODESIGN_ID).
#   BOR_NOTARY_KEY     path to the App Store Connect API key .p8 file
#   BOR_NOTARY_KEY_ID  the key's Key ID
#   BOR_NOTARY_ISSUER  the API key Issuer ID (UUID)
#                      All three set -> the .pkg and .dmg are notarized + stapled.
set -euo pipefail

ART="${1:-build/BoRBassEnhancer_artefacts/Release}"
OUT="${2:-Bass-Better-er-macOS.dmg}"
VER="${3:-0.1.0}"
NAME="Bass Better-er"
ID="com.boxofrules.bassbetterer"

[[ -d "$ART/AU/$NAME.component" ]] || { echo "ERROR: missing $ART/AU/$NAME.component (build first)" >&2; exit 1; }
[[ -d "$ART/VST3/$NAME.vst3"   ]] || { echo "ERROR: missing $ART/VST3/$NAME.vst3 (build first)"   >&2; exit 1; }

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
disclaimer="$here/../DISCLAIMER.txt"
entitlements="$here/entitlements.plist"

# --- signing config ---------------------------------------------------------
CODESIGN_ID="${BOR_CODESIGN_ID:-}"
if [[ -n "$CODESIGN_ID" ]]; then
  # real Developer ID: hardened runtime + secure timestamp + entitlements
  SIGN_ARGS=(--force --deep --options runtime --timestamp --entitlements "$entitlements" -s "$CODESIGN_ID")
  echo "Signing with: $CODESIGN_ID (hardened runtime)"
else
  # ad-hoc (local/dev): just enough to load on this machine
  CODESIGN_ID="-"
  SIGN_ARGS=(--force --deep -s -)
  echo "Signing ad-hoc (no Developer ID set — build will NOT be notarized)"
fi

# notarization is enabled only when all three API-key vars are present
NOTARIZE=0
if [[ -n "${BOR_NOTARY_KEY:-}" && -n "${BOR_NOTARY_KEY_ID:-}" && -n "${BOR_NOTARY_ISSUER:-}" ]]; then
  NOTARIZE=1
fi

notarize_and_staple() {  # $1 = file to submit + staple
  local f="$1" sid status
  local creds=(--key "$BOR_NOTARY_KEY" --key-id "$BOR_NOTARY_KEY_ID" --issuer "$BOR_NOTARY_ISSUER")
  echo "Submitting $f to the notary service ..."
  # Submit and capture the id, so we can poll + pull the log ourselves. We deliberately
  # don't use a bare `--wait`: Apple's notary service intermittently sticks a submission
  # at "In Progress" for a very long time, and an un-timed wait would hang the whole job.
  sid="$(xcrun notarytool submit "$f" "${creds[@]}" --output-format json | plutil -extract id raw -o - -)"
  echo "  submission id: $sid"
  # Wait with a hard ceiling. On timeout this exits non-zero; we then inspect the status.
  xcrun notarytool wait "$sid" "${creds[@]}" --timeout "${BOR_NOTARY_TIMEOUT:-20m}" || true
  status="$(xcrun notarytool info "$sid" "${creds[@]}" --output-format json | plutil -extract status raw -o - - 2>/dev/null || echo unknown)"
  echo "  status: $status"
  if [ "$status" != "Accepted" ]; then
    echo "ERROR: notarization not Accepted for $f (status=$status, id=$sid). Notary log:" >&2
    xcrun notarytool log "$sid" "${creds[@]}" || true
    exit 1
  fi
  xcrun stapler staple "$f"
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
root="$work/root"
mkdir -p "$root/Library/Audio/Plug-Ins/Components" "$root/Library/Audio/Plug-Ins/VST3"
cp -R "$ART/AU/$NAME.component" "$root/Library/Audio/Plug-Ins/Components/"
cp -R "$ART/VST3/$NAME.vst3"    "$root/Library/Audio/Plug-Ins/VST3/"

# sign the bundles so they load on the user's machine after install
codesign "${SIGN_ARGS[@]}" "$root/Library/Audio/Plug-Ins/Components/$NAME.component"
codesign "${SIGN_ARGS[@]}" "$root/Library/Audio/Plug-Ins/VST3/$NAME.vst3"

# component package -> distribution-wrapped installer (nicer UI + title)
pkgbuild --root "$root" --install-location "/" --identifier "$ID" --version "$VER" "$work/component.pkg"

# terms/disclaimer shown as a must-agree license pane in the installer
cp "$disclaimer" "$work/disclaimer.txt" 2>/dev/null || true
cat > "$work/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>$NAME $VER</title>
  <organization>com.boxofrules</organization>
  <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <domains enable_localSystem="true"/>
  <license file="disclaimer.txt"/>
  <choices-outline><line choice="default"/></choices-outline>
  <choice id="default"><pkg-ref id="$ID"/></choice>
  <pkg-ref id="$ID" version="$VER" onConclusion="none">component.pkg</pkg-ref>
</installer-gui-script>
XML

pkg="$work/$NAME $VER.pkg"
if [[ -n "${BOR_INSTALLER_ID:-}" ]]; then
  productbuild --distribution "$work/distribution.xml" --package-path "$work" --resources "$work" \
    --sign "$BOR_INSTALLER_ID" --timestamp "$pkg"
else
  productbuild --distribution "$work/distribution.xml" --package-path "$work" --resources "$work" "$pkg"
fi

# notarize + staple the installer itself, so the .pkg passes Gatekeeper even offline
[[ "$NOTARIZE" == 1 ]] && notarize_and_staple "$pkg"

# wrap the (stapled) pkg in a compressed dmg
dmgroot="$work/dmg"; mkdir -p "$dmgroot"
cp "$pkg" "$dmgroot/$NAME $VER.pkg"
rm -f "$OUT"
hdiutil create -volname "$NAME" -srcfolder "$dmgroot" -ov -format UDZO "$OUT" >/dev/null

# notarize + staple the dmg too (what the user actually downloads)
[[ "$NOTARIZE" == 1 ]] && notarize_and_staple "$OUT"

echo "Created $OUT"
