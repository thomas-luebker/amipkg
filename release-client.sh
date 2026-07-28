#!/bin/sh
# release-client.sh — the ONE command for an amipkg client release.
#
#   amipkg/release-client.sh 0.5.8 [--notes "..."] [--notes-file f]
#                                  [--dry-run] [--no-commit] [--aminet] [--no-smoke]
#
# Automates the full chain that was done by hand five times on 2026-07-27
# (and misfired once): version bump -> CLEAN cross-build -> $VER verify of
# ALL THREE binaries -> dist bumps -> bundle -> GitHub release -> re-pin the
# catalog entry (version + VERSIONED url + sha256 + sizeBytes TOGETHER) ->
# docs bundle copy -> push -> Pi publish -> org-mirror verification ->
# imager bundle refresh -> public-repo sync. Encoded hard rules:
#
#   * `make clean` always (the 0.5.4 assets shipped a stale 0.5.3 CLI when
#     only store.h changed) — and $VER is verified per binary regardless.
#   * The pin NEVER uses releases/latest (it floats under the fixed sha —
#     guaranteed mismatch for stale catalogs).
#   * Nothing publishes unless every prior step verified.
#
# Env overrides: AMIPKG_CROSS (bebbo bin dir), AMIGA_PKG_REPO, PUBLIC_REPO,
# AMIPKG_STAGE (binary staging), AMINET_EMAIL.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"                 # .../AmigaImager/amipkg
IMAGER="$(dirname "$HERE")"
PKGREPO="${AMIGA_PKG_REPO:-$HOME/Development/amiga-pkg}"
PUBREPO="${PUBLIC_REPO:-$HOME/Development/amipkg}"
STAGE="${AMIPKG_STAGE:-$HOME/Desktop/amipkg-68k}"
CROSS="${AMIPKG_CROSS:-$HOME/opt/amiga/bin}"
AMINET_EMAIL="${AMINET_EMAIL:-thomas@amiga-imager.com}"

VER="${1:-}"; shift || true
case "$VER" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "usage: release-client.sh <version, e.g. 0.5.8> [--notes ...] [--notes-file f] [--dry-run] [--no-commit] [--aminet] [--no-smoke]"; exit 1 ;;
esac

NOTES=""; NOTES_FILE=""; DRY=0; COMMIT=1; AMINET=0; SMOKE=1
while [ $# -gt 0 ]; do
    case "$1" in
        --notes)      NOTES="$2"; shift 2 ;;
        --notes-file) NOTES_FILE="$2"; shift 2 ;;
        --dry-run)    DRY=1; shift ;;
        --no-commit)  COMMIT=0; shift ;;
        --aminet)     AMINET=1; shift ;;
        --no-smoke)   SMOKE=0; shift ;;
        *) echo "unknown option: $1"; exit 1 ;;
    esac
done

fail() { echo "RELEASE ABORTED: $*" >&2; exit 1; }

command -v gh >/dev/null 2>&1 || PATH="/opt/homebrew/bin:$PATH"
[ "$DRY" = 1 ] || command -v gh >/dev/null 2>&1 || fail "gh CLI not found"
[ -x "$CROSS/m68k-amigaos-gcc" ] || fail "cross toolchain not at $CROSS"
[ -d "$PKGREPO/.git" ] || fail "amiga-pkg repo not at $PKGREPO"
DATE_DE="$(date +%-d.%-m.%Y)"

OLD_VER="$(sed -n 's/#define AMIPKG_VERSION "\(.*\)"/\1/p' "$HERE/src/core/store.h")"
[ -n "$OLD_VER" ] || fail "cannot read current version from store.h"
[ "$OLD_VER" != "$VER" ] || fail "version $VER is already current"
echo "==> amipkg $OLD_VER -> $VER (dist date $DATE_DE)"

# --- 1. version + dist bumps -------------------------------------------------
sed -i '' "s/#define AMIPKG_VERSION \"$OLD_VER\"/#define AMIPKG_VERSION \"$VER\"/" "$HERE/src/core/store.h"
sed -i '' "s/#define AMIPKG_VERDATE \".*\"/#define AMIPKG_VERDATE \"$DATE_DE\"/" "$HERE/src/core/store.h"
sed -i '' "s/^Version:      $OLD_VER/Version:      $VER/" "$HERE/dist/amipkg.readme"
sed -i '' "s/Echo \"amipkg|$OLD_VER|1|0\"/Echo \"amipkg|$VER|1|0\"/" "$HERE/dist/Install"
sed -i '' "s/@\$VER: amipkg.guide $OLD_VER (.*)/@\$VER: amipkg.guide $VER ($DATE_DE)/" "$HERE/dist/amipkg.guide"
grep -q "\"$VER\"" "$HERE/src/core/store.h" || fail "store.h bump did not take"
grep -q "^Version:      $VER" "$HERE/dist/amipkg.readme" || fail "readme bump did not take"
grep -q "amipkg|$VER|1|0" "$HERE/dist/Install" || fail "Install seed bump did not take"

# --- 2. CLEAN build + per-binary $VER verify ---------------------------------
( cd "$HERE" && make clean >/dev/null && PATH="$CROSS:$PATH" make ) >/dev/null 2>&1 \
    || { ( cd "$HERE" && PATH="$CROSS:$PATH" make ) 2>&1 | tail -5; fail "cross build failed"; }
( cd "$HERE" && make -f Makefile.host ) >/dev/null 2>&1 || fail "host build failed"
for b in amipkg amipkg-gui amipkg-mui; do
    got="$(strings "$HERE/$b" | sed -n "s/.*\$VER: $b \([0-9.]*\).*/\1/p" | head -1)"
    [ "$got" = "$VER" ] || fail "$b reports \$VER '$got', expected $VER (stale object?)"
    echo "    $b: \$VER $VER ok"
done

# --- 3. bundle ---------------------------------------------------------------
mkdir -p "$STAGE"
cp "$HERE/amipkg" "$HERE/amipkg-gui" "$HERE/amipkg-mui" "$STAGE/"
sh "$HERE/dist/make-bundle.sh" >/dev/null
SHA="$(shasum -a 256 "$HOME/Desktop/amipkg-client.lha" | cut -d' ' -f1)"
SIZE="$(stat -f%z "$HOME/Desktop/amipkg-client.lha")"
echo "    bundle: amipkg-client.lha $SIZE bytes, sha $SHA"

# --- 3b. REAL-AmigaOS smoke gauntlet (FS-UAE) ---------------------------------
# The just-built client must survive a live boot + catalog cycle before any
# byte is published. --no-smoke skips (e.g. headless boxes without FS-UAE).
if [ "$SMOKE" = 1 ] && [ "$DRY" = 0 ]; then
    echo "==> smoke gauntlet (FS-UAE, ~90s)..."
    sh "$HERE/smoke/run-smoke.sh" >"$HOME/Desktop/amipkg-smoke-$VER.log" 2>&1 \
        || fail "SMOKE GAUNTLET FAILED - see ~/Desktop/amipkg-smoke-$VER.log (release NOT published)"
    rm -f "$HOME/Desktop/amipkg-smoke-$VER.log"
    echo "    smoke passed"
fi

if [ "$DRY" = 1 ]; then
    echo "==> DRY RUN — stopping before publish. Would have:"
    echo "    gh release create v$VER (+2 assets)"
    echo "    pin: version=$VER url=releases/download/v$VER/amipkg-client.lha sha=$SHA size=$SIZE"
    echo "    push amiga-pkg, Pi publish, mirror verify, imager bundle refresh"
    echo "    NOTE: version bumps are LEFT IN PLACE for inspection (git checkout to revert)"
    exit 0
fi

# --- 4. GitHub release -------------------------------------------------------
[ -n "$NOTES$NOTES_FILE" ] || NOTES="amipkg $VER."
if [ -n "$NOTES_FILE" ]; then
    gh release create "v$VER" --repo thomas-luebker/amiga-pkg --title "amipkg $VER" \
        --notes-file "$NOTES_FILE" "$HOME/Desktop/amipkg.lha" "$HOME/Desktop/amipkg-client.lha"
else
    gh release create "v$VER" --repo thomas-luebker/amiga-pkg --title "amipkg $VER" \
        --notes "$NOTES" "$HOME/Desktop/amipkg.lha" "$HOME/Desktop/amipkg-client.lha"
fi

# --- 5. re-pin (versioned URL — never releases/latest) -----------------------
cd "$PKGREPO"
# A dirty tree here aborted the 0.6.2 run mid-release - autostash keeps
# unrelated local edits out of the way instead of failing.
git pull -q --rebase --autostash
VER="$VER" SHA="$SHA" SIZE="$SIZE" python3 - <<'PYEOF'
import json, os
v, sha, size = os.environ["VER"], os.environ["SHA"], int(os.environ["SIZE"])
p = json.load(open("packages/amipkg.json"))
p["version"] = v
p["archive"]["url"] = f"https://github.com/thomas-luebker/amiga-pkg/releases/download/v{v}/amipkg-client.lha"
p["archive"]["sha256"] = sha
p["archive"]["sizeBytes"] = size
json.dump(p, open("packages/amipkg.json", "w"), indent=2, sort_keys=True)
open("packages/amipkg.json", "a").write("\n")
print(f"    pinned {v} (versioned url)")
PYEOF
cp "$HOME/Desktop/amipkg.lha" docs/amipkg.lha
python3 amigapkg.py validate >/dev/null || fail "catalog validate failed after re-pin"
git add packages/amipkg.json docs/amipkg.lha
git commit -q -m "amipkg $VER: re-pin self-update client + Pages bundle"
git push -q || { git pull -q --rebase && git push -q; }

# --- 6. Pi publish + mirror verification -------------------------------------
echo "==> Pi publish..."
ssh -o ConnectTimeout=10 loki@192.168.178.91 \
    'systemctl --user start amipkg-nightly.service && sleep 75 && tail -2 ~/amipkg-publisher/logs/nightly-$(date +%Y%m%d).log' \
    || fail "Pi publish failed - run it manually and re-verify"
MIRROR_SHA="$(curl -sf --max-time 60 "http://amiga-imager.org/packages/amipkg-client.lha" | shasum -a 256 | cut -d' ' -f1)"
[ "$MIRROR_SHA" = "$SHA" ] || fail "org mirror serves $MIRROR_SHA, pin is $SHA (cache lag? retry in 5 min)"
echo "    org mirror sha == pin ok"
PIN_LIVE="$(curl -sf --max-time 30 "https://thomas-luebker.github.io/amiga-pkg/packages.json" | python3 -c "import json,sys; d=json.load(sys.stdin); print([p['version'] for p in d['packages'] if p['id']=='amipkg'][0])")"
[ "$PIN_LIVE" = "$VER" ] || echo "    NOTE: Pages still serves pin $PIN_LIVE (CDN lag, typically <10 min)"

# --- 7. imager bundles + public repo sync ------------------------------------
cp "$HERE/amipkg" "$HERE/amipkg-gui" "$HERE/amipkg-mui" "$IMAGER/BundledResources/Assets/AmigaFiles/PkgManager/"
cp "$HERE/dist/amipkg.guide" "$IMAGER/BundledResources/Assets/AmigaFiles/PkgManager/amipkg.guide"
sh "$HERE/sync-public.sh" >/dev/null 2>&1 || true
if [ "$COMMIT" = 1 ]; then
    ( cd "$IMAGER" && git add amipkg/ BundledResources/Assets/AmigaFiles/PkgManager/ \
        && git commit -q -m "chore(amipkg): $VER release sync" && git push -q ) \
        || echo "    NOTE: imager repo commit skipped/failed - commit manually"
    ( cd "$PUBREPO" && git add -A && git diff --cached --quiet \
        || { git commit -q -m "$VER release sync" && git push -q; } ) \
        || echo "    NOTE: public repo commit skipped/failed - commit manually"
fi

# --- 8. optional Aminet ------------------------------------------------------
if [ "$AMINET" = 1 ]; then
    cp "$HERE/dist/amipkg.readme" "$HOME/Desktop/amipkg.readme"
    ( cd "$HOME/Desktop" \
        && curl -sS --max-time 300 -T amipkg.lha "ftp://main.aminet.net/new/amipkg.lha" --user "anonymous:$AMINET_EMAIL" \
        && curl -sS --max-time 60 -T amipkg.readme "ftp://main.aminet.net/new/amipkg.readme" --user "anonymous:$AMINET_EMAIL" ) \
        && echo "    Aminet /new updated" || echo "    NOTE: Aminet upload failed - retry manually"
fi

echo "==> amipkg $VER RELEASED: gh v$VER · pin $SHA · mirror verified"
echo "    Reminder: on-Amiga clients pick it up via 'amipkg update' / Update All."
