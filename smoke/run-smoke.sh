#!/bin/sh
# run-smoke.sh — boot a REAL AmigaOS in FS-UAE and run amipkg through a
# live gauntlet against the production catalog. The weekend of 2026-07-27
# proved every serious client bug lives in the layer only a running
# AmigaOS exercises (PROGDIR semantics, receipts, DOS paths, GUI limits);
# this harness runs that layer before any human tester does.
#
#   amipkg/smoke/run-smoke.sh [--keep]
#
# Flow: APFS-clone the golden image -> swap in the CURRENT amipkg build ->
# replace the Startup-Sequence with a minimal CLI boot that executes the
# smoke script -> boot FS-UAE (bsdsocket = real network) with a host
# directory mounted as SMOKE: for LIVE result streaming -> poll for the
# done marker -> kill the emulator -> assert.
#
# Env overrides:
#   SMOKE_IMAGE  golden .hdf   (default: the 3.2.3 RTG 8 GB build)
#   SMOKE_KICK   kickstart rom (default: A1200 47.115)
#   SMOKE_FSUAE  fs-uae binary
#   SMOKE_PKG    package id to install/remove (default: xxd — 37 KB)
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
AMIPKG_DIR="$(dirname "$HERE")"
CLI="${AMIGA_DISK_CLI:-$HOME/Development/AmigaImager/AmigaDiskKit/.build/release/AmigaDiskCLI}"
IMAGE="${SMOKE_IMAGE:-$HOME/Desktop/build/uae_3.2.3_rtg_8gb_270726.hdf}"
KICK="${SMOKE_KICK:-$HOME/Desktop/build/A1200.47.115.rom}"
FSUAE="${SMOKE_FSUAE:-/Applications/FS-UAE.app/Contents/MacOS/fs-uae}"
PKG="${SMOKE_PKG:-xxd}"
TIMEOUT=420          # generous: boot + update + download + install + remove
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

fail() { echo "SMOKE FAILED: $*" >&2; exit 1; }
[ -x "$CLI" ]   || fail "AmigaDiskCLI not built at $CLI"
[ -f "$IMAGE" ] || fail "golden image missing: $IMAGE"
[ -f "$KICK" ]  || fail "kickstart missing: $KICK"
[ -x "$FSUAE" ] || fail "fs-uae missing: $FSUAE"
[ -f "$AMIPKG_DIR/amipkg" ] || fail "build amipkg first (make in $AMIPKG_DIR)"

WORK="$(mktemp -d /tmp/amipkg-smoke.XXXXXX)"
HDF="$WORK/smoke.hdf"
HOSTDIR="$WORK/SMOKE"
mkdir -p "$HOSTDIR"
cleanup() {
    [ -n "${EMUPID:-}" ] && kill "$EMUPID" 2>/dev/null || true
    if [ "$KEEP" = 1 ]; then echo "(kept: $WORK)"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

echo "==> cloning golden image (APFS clone, instant)"
cp -c "$IMAGE" "$HDF" 2>/dev/null || cp "$IMAGE" "$HDF"

echo "==> installing the CURRENT amipkg build into the clone"
"$CLI" disk fs copy "$HDF" DH0 "$AMIPKG_DIR/amipkg" "Tools/AmigaImager/amipkg" >/dev/null

echo "==> injecting smoke boot sequence"
cat > "$WORK/Startup-Sequence" <<'SS'
; amipkg smoke harness — minimal CLI boot, no Workbench, no first-boot.
C:SetPatch >NIL: QUIET
Assign >NIL: ENV: RAM: 
MakeDir >NIL: RAM:T RAM:Clipboards
Assign >NIL: T: RAM:T
Assign >NIL: CLIPS: RAM:Clipboards
Execute S:amigaimager-assigns
Execute S:smoke-test
SS
cat > "$WORK/smoke-test" <<SS
Echo "SMOKE-START" >SMOKE:results.txt
AMIPKG:amipkg update >>SMOKE:results.txt
Echo "rc_update=\$RC" >>SMOKE:results.txt
AMIPKG:amipkg install $PKG >>SMOKE:results.txt
Echo "rc_install=\$RC" >>SMOKE:results.txt
Echo "--- installed.txt after install ---" >>SMOKE:results.txt
Type AMIPKG:db/installed.txt >>SMOKE:results.txt
AMIPKG:amipkg list >>SMOKE:results.txt
AMIPKG:amipkg remove $PKG >>SMOKE:results.txt
Echo "rc_remove=\$RC" >>SMOKE:results.txt
Echo "--- installed.txt after remove ---" >>SMOKE:results.txt
Type AMIPKG:db/installed.txt >>SMOKE:results.txt
Echo "SMOKE-DONE" >>SMOKE:results.txt
Echo "done" >SMOKE:done-marker
SS
"$CLI" disk fs copy "$HDF" DH0 "$WORK/Startup-Sequence" "S/Startup-Sequence" >/dev/null
"$CLI" disk fs copy "$HDF" DH0 "$WORK/smoke-test" "S/smoke-test" >/dev/null

echo "==> booting FS-UAE (timeout ${TIMEOUT}s)"
cat > "$WORK/smoke.fs-uae" <<CFG
amiga_model = A1200/020
kickstart_file = $KICK
hard_drive_0 = $HDF
hard_drive_1 = $HOSTDIR
hard_drive_1_label = SMOKE
fast_memory = 8192
bsdsocket_library = 1
floppy_drive_volume = 0
automatic_input_grab = 0
CFG
"$FSUAE" "$WORK/smoke.fs-uae" >/dev/null 2>&1 &
EMUPID=$!

SECS=0
while [ ! -f "$HOSTDIR/done-marker" ]; do
    kill -0 "$EMUPID" 2>/dev/null || fail "fs-uae exited early (crash?)"
    [ "$SECS" -ge "$TIMEOUT" ] && { cp "$HOSTDIR/results.txt" "$WORK/" 2>/dev/null || true; fail "timeout after ${TIMEOUT}s — results so far: $(tail -3 "$HOSTDIR/results.txt" 2>/dev/null || echo none)"; }
    sleep 5
    SECS=$((SECS + 5))
done
echo "==> guest finished after ~${SECS}s, shutting emulator down"
kill "$EMUPID" 2>/dev/null || true
wait "$EMUPID" 2>/dev/null || true
EMUPID=""

RESULTS="$HOSTDIR/results.txt"
echo "==> asserting"
ok=1
assert_contains() {
    if grep -q "$1" "$RESULTS"; then echo "    ok: $2"; else echo "    FAIL: $2" >&2; ok=0; fi
}
assert_absent_after() {
    # $PKG must not appear AFTER the '--- installed.txt' marker line
    if sed -n '/--- installed.txt after remove/,$p' "$RESULTS" | grep -q "^$PKG|"; then
        echo "    FAIL: $1" >&2; ok=0
    else echo "    ok: $1"; fi
}
assert_contains "SMOKE-START"   "guest booted and smoke script ran"
assert_contains "rc_update=0"   "amipkg update succeeded (real network, signed index verified on-device)"
assert_contains "rc_install=0"  "amipkg install $PKG succeeded (download + sha + receipts)"
if sed -n '/--- installed.txt after install/,/--- installed.txt after remove/p' "$RESULTS" | grep -q "^$PKG|"; then
    echo "    ok: $PKG receipt present after install"
else
    echo "    FAIL: $PKG receipt missing after install" >&2; ok=0
fi
assert_contains "rc_remove=0"   "amipkg remove $PKG succeeded"
assert_absent_after "receipt gone after remove"
assert_contains "SMOKE-DONE"    "gauntlet completed"

echo "---- results.txt ----"
cat "$RESULTS"
echo "---------------------"
[ "$ok" = 1 ] || fail "one or more assertions failed"
echo "==> SMOKE PASSED — amipkg $(strings "$AMIPKG_DIR/amipkg" | sed -n 's/.*\$VER: amipkg \([0-9.]*\).*/\1/p' | head -1) survived a real AmigaOS boot + live catalog gauntlet"
