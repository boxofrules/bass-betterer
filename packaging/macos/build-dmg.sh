#!/usr/bin/env bash
# build-dmg.sh — wrap the built AU + VST3 in a proper macOS installer .pkg, inside a .dmg.
#
#   packaging/macos/build-dmg.sh [ARTEFACTS_DIR] [OUT_DMG] [VERSION]
#
# Defaults assume a Release build. The .pkg installs to the system plug-in folders
# (/Library/Audio/Plug-Ins/{Components,VST3}) — the installer asks for an admin password.
# The bundles are ad-hoc signed so they load after install; the pkg/dmg are NOT notarized,
# so the first open needs right-click -> Open (Gatekeeper).
set -euo pipefail

ART="${1:-build/BoRBassEnhancer_artefacts/Release}"
OUT="${2:-Bass-Better-er-macOS.dmg}"
VER="${3:-0.1.0}"
NAME="Bass Better-er"
ID="com.boxofrules.bassbetterer"

[[ -d "$ART/AU/$NAME.component" ]] || { echo "ERROR: missing $ART/AU/$NAME.component (build first)" >&2; exit 1; }
[[ -d "$ART/VST3/$NAME.vst3"   ]] || { echo "ERROR: missing $ART/VST3/$NAME.vst3 (build first)"   >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
root="$work/root"
mkdir -p "$root/Library/Audio/Plug-Ins/Components" "$root/Library/Audio/Plug-Ins/VST3"
cp -R "$ART/AU/$NAME.component" "$root/Library/Audio/Plug-Ins/Components/"
cp -R "$ART/VST3/$NAME.vst3"    "$root/Library/Audio/Plug-Ins/VST3/"

# ad-hoc sign so the plugins load on the user's machine after install
codesign --force --deep -s - "$root/Library/Audio/Plug-Ins/Components/$NAME.component"
codesign --force --deep -s - "$root/Library/Audio/Plug-Ins/VST3/$NAME.vst3"

# component package -> distribution-wrapped installer (nicer UI + title)
pkgbuild --root "$root" --install-location "/" --identifier "$ID" --version "$VER" "$work/component.pkg"

cat > "$work/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>$NAME $VER</title>
  <organization>com.boxofrules</organization>
  <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <domains enable_localSystem="true"/>
  <choices-outline><line choice="default"/></choices-outline>
  <choice id="default"><pkg-ref id="$ID"/></choice>
  <pkg-ref id="$ID" version="$VER" onConclusion="none">component.pkg</pkg-ref>
</installer-gui-script>
XML
productbuild --distribution "$work/distribution.xml" --package-path "$work" "$work/$NAME $VER.pkg"

# wrap the pkg in a compressed dmg
dmgroot="$work/dmg"; mkdir -p "$dmgroot"
cp "$work/$NAME $VER.pkg" "$dmgroot/$NAME $VER.pkg"
rm -f "$OUT"
hdiutil create -volname "$NAME" -srcfolder "$dmgroot" -ov -format UDZO "$OUT" >/dev/null

echo "Created $OUT"
