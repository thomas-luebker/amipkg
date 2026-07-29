#!/bin/sh
# make-bundle.sh — assemble the standalone amipkg.lha for external testers.
#
# Bundles the freshly-built 68k binaries + the current signed AmigaPKG catalog
# + the Install script and ReadMe into a flat lh0 .lha that any AmigaOS can
# extract with C:lha. The catalog is baked in so browsing works offline right
# after install; `amipkg update` refreshes it online.
#
#   dist/make-bundle.sh [output.lha]
#
# Env overrides:
#   AMIPKG_BIN    dir holding amipkg, amipkg-gui, amipkg-gui.info
#                 (default: amipkg/build/stage — where release-client.sh stages)
#   AMIPKG_INDEX  dir holding packages.json + packages.json.sig
#                 (default: ~/Development/amiga-pkg/docs — the signed repo index)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${AMIPKG_BIN:-$(dirname "$HERE")/build/stage}"
INDEX="${AMIPKG_INDEX:-$HOME/Development/amiga-pkg/docs}"
OUT="${1:-$(dirname "$HERE")/build/amipkg.lha}"

STAGE="$(mktemp -d)/AmiPKG"
mkdir -p "$STAGE"
cp "$BIN/amipkg" "$BIN/amipkg-gui" "$BIN/amipkg-gui.info" "$STAGE/"
cp "$BIN/amipkg-mui" "$STAGE/amipkg-mui"
cp "$BIN/amipkg-gui.info" "$STAGE/amipkg-mui.info"   # same parcel icon
cp "$INDEX/packages.json" "$INDEX/packages.json.sig" "$STAGE/"
cp "$HERE/Install" "$HERE/Install-System" "$HERE/ReadMe" "$HERE/amipkg.guide" "$STAGE/"

# A double-clickable installer icon: WBPROJECT + DefaultTool IconX runs the
# Install script (the extracted Install has no script-bit, so this + the
# documented `Execute Install` are the two ways to run it).
python3 "$HERE/mkprojicon.py" "$STAGE/amipkg-gui.info" "$STAGE/Install.info" IconX
python3 "$HERE/mkprojicon.py" "$STAGE/amipkg-gui.info" "$STAGE/Install-System.info" IconX

# Native lh5 encoder when the AmigaDiskCLI is built (deterministic via the
# fixed epoch; ~half the download size); mklha.py (stored lh0) as fallback.
DISKCLI="${AMIGA_DISK_CLI:-$HOME/Development/AmigaImager/AmigaDiskKit/.build/release/AmigaDiskCLI}"
LHA_EPOCH=1784894400   # 2026-07-24 12:00 UTC, mklha.py parity
if [ -x "$DISKCLI" ] && [ -z "${AMIPKG_BUNDLE_STORE:-}" ]; then
    "$DISKCLI" lha create "$OUT" "$STAGE" --epoch "$LHA_EPOCH"
else
    ( cd "$STAGE" && python3 "$HERE/mklha.py" "$OUT" \
        amipkg amipkg-gui amipkg-gui.info amipkg-mui amipkg-mui.info \
        amipkg.guide packages.json packages.json.sig \
        Install Install.info Install-System Install-System.info ReadMe )
fi

# The SELF-UPDATE / release asset: binaries only, NO catalog. (The catalog
# carries this archive's sha256 — including the catalog would make the hash
# self-referential. The amipkg package entry's placeFile recipe installs
# these into the AMIPKG: home drawer.)
CLIENT="$(dirname "$OUT")/amipkg-client.lha"
if [ -x "$DISKCLI" ] && [ -z "${AMIPKG_BUNDLE_STORE:-}" ]; then
    CSTAGE="$(dirname "$STAGE")/AmiPKG-client"
    mkdir -p "$CSTAGE"
    for f in amipkg amipkg-gui amipkg-gui.info amipkg-mui amipkg-mui.info amipkg.guide; do
        cp "$STAGE/$f" "$CSTAGE/"
    done
    "$DISKCLI" lha create "$CLIENT" "$CSTAGE" --epoch "$LHA_EPOCH"
else
    ( cd "$STAGE" && python3 "$HERE/mklha.py" "$CLIENT" \
        amipkg amipkg-gui amipkg-gui.info amipkg-mui amipkg-mui.info amipkg.guide )
fi
rm -rf "$(dirname "$STAGE")"

echo "wrote $OUT"
echo "wrote $CLIENT (self-update / release asset)"
command -v lha >/dev/null 2>&1 && lha l "$OUT" || true
