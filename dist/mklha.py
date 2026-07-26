#!/usr/bin/env python3
# Minimal LHA level-0 "-lh0-" (stored, no compression) archive writer. Every
# AmigaOS has C:lha to extract it. Flat files only (no directory entries).
import struct, sys, os

def crc16(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF

def entry(name, data):
    # fixed MS-DOS timestamp 2026-07-24 12:00:00 (deterministic archive)
    ts = ((2026-1980) << 25) | (7 << 21) | (24 << 16) | (12 << 11) | (0 << 5) | 0
    fn = name.encode('latin-1')
    body  = b'-lh0-'
    body += struct.pack('<I', len(data))   # compressed size (== original for lh0)
    body += struct.pack('<I', len(data))   # original size
    body += struct.pack('<I', ts)          # timestamp
    body += bytes([0x20, 0x00, len(fn)])   # attribute, header level 0, name len
    body += fn
    body += struct.pack('<H', crc16(data)) # CRC-16 of the data
    return bytes([len(body) & 0xFF, sum(body) & 0xFF]) + body + data

def main():
    out, files = sys.argv[1], sys.argv[2:]
    with open(out, 'wb') as f:
        for p in files:
            f.write(entry(os.path.basename(p), open(p, 'rb').read()))
        f.write(b'\x00')   # end-of-archive marker

main()
