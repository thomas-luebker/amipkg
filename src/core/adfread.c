/*
 * adfread.c - minimal read-only OFS/FFS ADF extractor. See adfread.h.
 *
 * Block layout (big-endian longs, BSIZE=512), per the Amiga FFS/OFS format:
 *   header/dir/root block:
 *     long 0   type (T_HEADER=2)
 *     long 2   high_seq (file: # data-block ptrs used here)
 *     longs 6..77  hashtable[72] (root/dir)  OR  data-block table (file),
 *                  the data-block table filled from the TOP: ptr i is at
 *                  long (77 - i), first data block at 77.
 *     long 81  byte_size (file length in bytes)
 *     byte 432 name_len, byte 433.. name
 *     long 124 hash_chain (next entry in same bucket)
 *     long 126 extension (file: next file-extension block; 0 = none)
 *     long 127 sec_type (ST_ROOT=1, ST_USERDIR=2, ST_FILE=-3)
 *   OFS data block: long0 type=8, long3 data_size, payload at byte 24.
 *   FFS data block: raw 512 bytes of file data.
 */
#include "adfread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BSIZE 512
#define HT_LO 6           /* first hashtable/datablock long index */
#define HT_SIZE 72        /* BSIZE/4 - 56 */
#define ST_USERDIR 2
#define ST_FILE (-3)
#define MAX_DEPTH 24

static FILE *g_adf;
static long  g_nblocks;
static int   g_ffs;       /* 1 = FFS data blocks (raw), 0 = OFS (24-byte hdr) */

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
         | ((unsigned long)p[2] << 8)  | (unsigned long)p[3];
}
static long sbe32(const unsigned char *p) { return (long)(int)be32(p); }
static unsigned long lng(const unsigned char *b, int idx) { return be32(b + idx * 4); }

static int read_block(long blk, unsigned char *buf)
{
    if (blk < 0 || blk >= g_nblocks) return 0;
    if (fseek(g_adf, blk * BSIZE, SEEK_SET) != 0) return 0;
    return fread(buf, 1, BSIZE, g_adf) == (size_t)BSIZE;
}

/* Sanitise an Amiga filename into a single path component (defensive: strip any
 * '/' or ':' that would escape the target drawer). */
static void safe_name(const unsigned char *b, char *out, size_t n)
{
    int len = b[432], i, o = 0;
    if (len < 0) len = 0;
    if (len > 30) len = 30;
    for (i = 0; i < len && (size_t)o < n - 1; i++) {
        unsigned char c = b[433 + i];
        if (c == '/' || c == ':' || c == 0) c = '_';
        out[o++] = (char)c;
    }
    out[o] = '\0';
    if (o == 0) { strncpy(out, "_unnamed", n - 1); out[n - 1] = '\0'; }
}

/* Collect a file's data-block numbers (header + any extension blocks) in order.
 * Returns the count, capped at max. */
static long file_blocks(const unsigned char *hdr, long *out, long max)
{
    unsigned char ext[BSIZE];
    const unsigned char *b = hdr;
    long count = 0;
    long extblk;
    int guard = 0;
    for (;;) {
        long hs = (long)lng(b, 2), i;
        if (hs > HT_SIZE) hs = HT_SIZE;
        for (i = 0; i < hs && count < max; i++)
            out[count++] = (long)be32(b + (77 - i) * 4);
        extblk = (long)be32(b + 126 * 4);        /* file extension block */
        if (extblk <= 0 || count >= max || ++guard > 4096) break;
        if (!read_block(extblk, ext)) break;
        b = ext;
    }
    return count;
}

static int extract_file(const unsigned char *hdr, const char *path)
{
    static long blks[8192];
    unsigned char data[BSIZE];
    long n, i;
    unsigned long remaining = lng(hdr, 81);      /* byte_size */
    FILE *out = fopen(path, "wb");
    if (!out) return 0;
    n = file_blocks(hdr, blks, (long)(sizeof blks / sizeof blks[0]));
    for (i = 0; i < n && remaining > 0; i++) {
        unsigned long take;
        if (!read_block(blks[i], data)) break;
        if (g_ffs) {
            take = remaining < (unsigned long)BSIZE ? remaining : (unsigned long)BSIZE;
            fwrite(data, 1, take, out);
        } else {
            unsigned long dsize = lng(data, 3);  /* OFS data_size */
            if (dsize > (unsigned long)(BSIZE - 24)) dsize = BSIZE - 24;
            take = remaining < dsize ? remaining : dsize;
            fwrite(data + 24, 1, take, out);
        }
        remaining -= take;
    }
    fclose(out);
    return 1;
}

static long extract_dir(long blk, const char *destpath, int depth, long *files)
{
    unsigned char hdr[BSIZE], eb[BSIZE];
    int bucket;
    if (depth > MAX_DEPTH) return 0;
    if (!read_block(blk, hdr)) return 0;
    for (bucket = 0; bucket < HT_SIZE; bucket++) {
        long e = (long)lng(hdr, HT_LO + bucket);
        int chain = 0;
        while (e > 0 && chain++ < 4096) {
            char name[64], child[512];
            long sec;
            if (!read_block(e, eb)) break;
            safe_name(eb, name, sizeof name);
            sec = sbe32(eb + 508);               /* sec_type */
            snprintf(child, sizeof child, "%s/%s", destpath, name);
            if (sec == ST_USERDIR) {
                adf_mkdir(child);
                extract_dir(e, child, depth + 1, files);
            } else if (sec == ST_FILE) {
                if (extract_file(eb, child)) (*files)++;
            }
            e = (long)be32(eb + 496);            /* hash_chain */
        }
    }
    return 0;
}

long adf_extract(const char *adf_path, const char *destdir)
{
    unsigned char boot[BSIZE];
    long size, rootblk, files = 0;
    g_adf = fopen(adf_path, "rb");
    if (!g_adf) return -1;
    fseek(g_adf, 0, SEEK_END);
    size = ftell(g_adf);
    fseek(g_adf, 0, SEEK_SET);
    g_nblocks = size / BSIZE;
    if (g_nblocks < 2 || !read_block(0, boot)) { fclose(g_adf); return -1; }
    if (boot[0] != 'D' || boot[1] != 'O' || boot[2] != 'S') { fclose(g_adf); return -1; }
    g_ffs = (boot[3] & 1) ? 1 : 0;               /* DOS0=OFS, DOS1=FFS */
    rootblk = g_nblocks / 2;
    adf_mkdir(destdir);
    extract_dir(rootblk, destdir, 0, &files);
    fclose(g_adf);
    return files;
}
