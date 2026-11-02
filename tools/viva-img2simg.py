#!/usr/bin/env python3
"""Raw image -> Android sparse image for `fastboot flash` (img2simg without AOSP).

  viva-img2simg.py IN.img OUT.simg

Holes of a sparse input file (mkfile -n) become DONT_CARE chunks, so fastboot
sends only the blocks the filesystem wrote; zero-filled data blocks become FILL.
"""
import os
import struct
import sys

BLK = 4096
CHUNK_RAW, CHUNK_FILL, CHUNK_DONT_CARE = 0xCAC1, 0xCAC2, 0xCAC3
MAX_RAW = 16 << 20  # bytes per RAW chunk
ZERO = bytes(BLK)


def data_ranges(f, size):
    """(start, end) of non-hole regions, block aligned."""
    fd = f.fileno()
    pos = 0
    while pos < size:
        try:
            start = os.lseek(fd, pos, os.SEEK_DATA)
        except OSError:
            break
        end = os.lseek(fd, start, os.SEEK_HOLE)
        yield start // BLK * BLK, min(size, (end + BLK - 1) // BLK * BLK)
        pos = end


def convert(src, dst):
    size = os.path.getsize(src)
    if size % BLK:
        sys.exit(f'{src}: size not a multiple of {BLK}')
    total = size // BLK
    chunks = []  # (type, blocks, payload or fill)

    def dont_care(n):
        if n:
            chunks.append((CHUNK_DONT_CARE, n, b''))

    with open(src, 'rb') as f:
        cur = 0
        for start, end in data_ranges(f, size):
            dont_care((start - cur) // BLK)
            f.seek(start)
            off = start
            raw = bytearray()
            zeros = 0
            while off < end:
                b = f.read(BLK)
                if b == ZERO:
                    if raw:
                        chunks.append((CHUNK_RAW, len(raw) // BLK, bytes(raw)))
                        raw = bytearray()
                    zeros += 1
                else:
                    if zeros:
                        chunks.append((CHUNK_FILL, zeros, struct.pack('<I', 0)))
                        zeros = 0
                    raw += b
                    if len(raw) >= MAX_RAW:
                        chunks.append((CHUNK_RAW, len(raw) // BLK, bytes(raw)))
                        raw = bytearray()
                off += BLK
            if raw:
                chunks.append((CHUNK_RAW, len(raw) // BLK, bytes(raw)))
            if zeros:
                chunks.append((CHUNK_FILL, zeros, struct.pack('<I', 0)))
            cur = end
        dont_care(total - cur)

    with open(dst, 'wb') as o:
        o.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BLK, total, len(chunks), 0))
        for typ, n, payload in chunks:
            o.write(struct.pack('<HHII', typ, 0, n, 12 + len(payload)))
            o.write(payload)
    raw_blocks = sum(n for t, n, _ in chunks if t == CHUNK_RAW)
    print(f'{dst}: {total} blocks, {len(chunks)} chunks, {raw_blocks * BLK >> 20} MiB of data')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    convert(sys.argv[1], sys.argv[2])
