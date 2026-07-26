# amipkg — AmigaOS build (bebbo amiga-gcc, https://github.com/bebbo/amiga-gcc)
#
#   make            → amipkg (68000 baseline; runs on every Amiga)
#   make CPU=68020  → 68020+ build
#
# NOT built in the AmigaImager CI/dev environment (no cross-toolchain there);
# the portable core is host-tested via Makefile.host instead.

CC      = m68k-amigaos-gcc
CPU     = 68000
# vendor/amissl: the AmiSSL SDK header subset (68k inlines) for the optional
# runtime https support in http.c — no link lib needed, calls inline via bases.
CFLAGS  = -Os -fomit-frame-pointer -m$(CPU) -Wall -Wextra -D__amigaos__ -Ivendor/amissl/include
# -lgcc after the objects: newlib's printf/scanf pull in libgcc's 64-bit
# integer (__udivdi3/__umoddi3) and soft-float (__extenddfxf2) helpers, which
# sit after libc in the default link order.
LDFLAGS = -s -lgcc

CORE = src/core/sha256.c src/core/aver.c src/core/receipts.c \
       src/core/ajson.c src/core/aindex.c src/core/resolve.c src/core/arecipe.c src/core/arun.c \
       src/core/store.c
# CLI-only: on-device Ed25519 verify (averify.c) over vendored TweetNaCl.
CRYPTO = src/core/averify.c
AMIGA = src/amiga/http.c src/amiga/install.c src/amiga/main.c src/core/adfread.c
GUI   = src/amiga/gui.c

all: amipkg amipkg-gui amipkg-mui

# TweetNaCl (vendored public-domain Ed25519/SHA-512) — terse crypto, compiled
# on its own with warnings off so it doesn't pollute the app's build.
tweetnacl.o: src/core/tweetnacl.c
	$(CC) $(CFLAGS) -w -c $< -o $@

amipkg: $(CORE) $(CRYPTO) $(AMIGA) tweetnacl.o
	$(CC) $(CFLAGS) -o $@ $(CORE) $(CRYPTO) $(AMIGA) tweetnacl.o $(LDFLAGS)

# The GadTools GUI shares the portable core but shells out to `C:amipkg` for the
# net/crypto actions, so it links neither http nor TweetNaCl.
amipkg-gui: $(CORE) $(GUI)
	$(CC) $(CFLAGS) -o $@ $(CORE) $(GUI) $(LDFLAGS)

# The MUI front-end: vendored MUI 3.8 dev-kit headers; -lamiga for
# DoMethod/HookEntry (amiga.lib).
amipkg-mui: $(CORE) src/amiga/mui.c
	$(CC) $(CFLAGS) -Ivendor/mui/include -o $@ $(CORE) src/amiga/mui.c -s -lamiga -lgcc

clean:
	rm -f amipkg amipkg-gui amipkg-mui tweetnacl.o

.PHONY: all clean
