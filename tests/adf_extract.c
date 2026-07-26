/*
 * adf_extract.c — host harness to exercise the ADF reader on a real image.
 *   build/adf_extract <image.adf> <destdir>
 * Provides the POSIX adf_mkdir() hook and prints the file count.
 */
#include "../src/core/adfread.h"

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

int adf_mkdir(const char *path)
{
    if (mkdir(path, 0777) == 0) return 0;
    return 0;   /* already-exists / benign: extraction continues */
}

int main(int argc, char **argv)
{
    long n;
    if (argc < 3) { fprintf(stderr, "usage: adf_extract <image.adf> <destdir>\n"); return 2; }
    n = adf_extract(argv[1], argv[2]);
    if (n < 0) { fprintf(stderr, "adf_extract: not a readable OFS/FFS ADF\n"); return 1; }
    printf("extracted %ld file(s) from %s to %s\n", n, argv[1], argv[2]);
    return 0;
}
