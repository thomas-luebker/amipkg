/*
 * adfread.h — minimal read-only OFS/FFS ADF (Amiga floppy image) extractor.
 *
 * Some Aminet packages are distributed as a raw .adf disk image rather than an
 * LHA archive. This reader parses the OFS/FFS filesystem inside such an image
 * and writes its files+drawers out to a host directory, so amipkg can install
 * an ADF the same way it installs an LHA (fetch -> extract -> copy tree).
 *
 * Portable C (stdio only) so it runs on the 68k target AND is host-testable.
 * Directory creation is delegated to the platform via adf_mkdir().
 */
#ifndef AMIPKG_ADFREAD_H
#define AMIPKG_ADFREAD_H

/* Extract every file+drawer in `adf_path` (an OFS or FFS DD/HD image) into
 * `destdir` (created if needed). Returns the number of files written, or -1 on
 * a format error (not an ADF / unreadable). */
long adf_extract(const char *adf_path, const char *destdir);

/* Platform hook: create one directory, idempotently. Implemented with
 * dos.library CreateDir on the Amiga (install.c) and POSIX mkdir in the host
 * test harness. Returns 0 on success or if it already exists. */
int adf_mkdir(const char *path);

#endif /* AMIPKG_ADFREAD_H */
