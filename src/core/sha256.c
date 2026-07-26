/* sha256.c - amipkg portable core. FIPS 180-4 SHA-256, byte-oriented. */

#include "sha256.h"

static const u32 K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32-(n))))

static void sha256_block(sha256_ctx *c, const unsigned char *p)
{
    u32 w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((u32)p[i*4] << 24) | ((u32)p[i*4+1] << 16)
             | ((u32)p[i*4+2] << 8) | (u32)p[i*4+3];
    for (i = 16; i < 64; i++) {
        u32 s0 = ROTR(w[i-15],7) ^ ROTR(w[i-15],18) ^ (w[i-15] >> 3);
        u32 s1 = ROTR(w[i-2],17) ^ ROTR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->state[0]; b=c->state[1]; cc=c->state[2]; d=c->state[3];
    e=c->state[4]; f=c->state[5]; g=c->state[6]; h=c->state[7];
    for (i = 0; i < 64; i++) {
        u32 S1 = ROTR(e,6) ^ ROTR(e,11) ^ ROTR(e,25);
        u32 ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + K[i] + w[i];
        u32 S0 = ROTR(a,2) ^ ROTR(a,13) ^ ROTR(a,22);
        u32 maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=d;
    c->state[4]+=e; c->state[5]+=f; c->state[6]+=g; c->state[7]+=h;
}

void sha256_init(sha256_ctx *c)
{
    c->state[0]=0x6a09e667u; c->state[1]=0xbb67ae85u;
    c->state[2]=0x3c6ef372u; c->state[3]=0xa54ff53au;
    c->state[4]=0x510e527fu; c->state[5]=0x9b05688cu;
    c->state[6]=0x1f83d9abu; c->state[7]=0x5be0cd19u;
    c->len_hi = c->len_lo = 0;
    c->buf_used = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    /* 64-bit bit-length as two u32s: len_lo counts bits, carries into len_hi. */
    u32 add_lo = (u32)(len << 3);
    u32 add_hi = (u32)(len >> 29);
    u32 old = c->len_lo;
    c->len_lo += add_lo;
    if (c->len_lo < old) c->len_hi++;
    c->len_hi += add_hi;

    while (len > 0) {
        size_t space = 64 - c->buf_used;
        size_t take = len < space ? len : space;
        size_t i;
        for (i = 0; i < take; i++) c->buf[c->buf_used + i] = p[i];
        c->buf_used += take; p += take; len -= take;
        if (c->buf_used == 64) { sha256_block(c, c->buf); c->buf_used = 0; }
    }
}

void sha256_final(sha256_ctx *c, unsigned char out[32])
{
    u32 hi = c->len_hi, lo = c->len_lo;
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    unsigned char lenb[8];
    int i;
    sha256_update(c, &pad, 1);
    while (c->buf_used != 56) sha256_update(c, &zero, 1);
    lenb[0]=(unsigned char)(hi>>24); lenb[1]=(unsigned char)(hi>>16);
    lenb[2]=(unsigned char)(hi>>8);  lenb[3]=(unsigned char)hi;
    lenb[4]=(unsigned char)(lo>>24); lenb[5]=(unsigned char)(lo>>16);
    lenb[6]=(unsigned char)(lo>>8);  lenb[7]=(unsigned char)lo;
    /* bypass the length accounting for the trailer itself */
    {
        size_t save_used = c->buf_used;
        size_t j;
        for (j = 0; j < 8; j++) c->buf[save_used + j] = lenb[j];
        c->buf_used += 8;
        if (c->buf_used == 64) { sha256_block(c, c->buf); c->buf_used = 0; }
    }
    for (i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(c->state[i] >> 24);
        out[i*4+1] = (unsigned char)(c->state[i] >> 16);
        out[i*4+2] = (unsigned char)(c->state[i] >> 8);
        out[i*4+3] = (unsigned char)(c->state[i]);
    }
}

void sha256_hex(const void *data, size_t len, char out_hex[65])
{
    static const char hexd[] = "0123456789abcdef";
    unsigned char digest[32];
    sha256_ctx c;
    int i;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, digest);
    for (i = 0; i < 32; i++) {
        out_hex[i*2]   = hexd[digest[i] >> 4];
        out_hex[i*2+1] = hexd[digest[i] & 0xf];
    }
    out_hex[64] = '\0';
}
