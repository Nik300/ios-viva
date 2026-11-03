#!/usr/bin/env python3
"""Extract one logical partition from an Android (sparse) super.img, without lpunpack/simg2img.

  viva-super-extract.py SUPER.img            list the logical partitions
  viva-super-extract.py SUPER.img NAME OUT   write partition NAME (e.g. vendor_a) raw to OUT

Formats: sparse image (system/core/libsparse), LP metadata (system/core/fs_mgr/liblp:
geometry at 4096, primary metadata at 4096 + 2 * 4096, 512-byte sectors).
"""
import bisect, struct, sys


class Sparse:
    """Random access to the expanded image."""
    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(28)
        magic, major, minor, fhs, chs, self.bs, blocks, chunks, _ = struct.unpack('<IHHHHIIII', hdr)
        if magic != 0xed26ff3a:
            raise ValueError('not a sparse image')
        self.f.seek(fhs)
        self.starts, self.chunks = [], []
        pos = 0
        for _ in range(chunks):
            ctype, _, nblk, total = struct.unpack('<HHII', self.f.read(chs))
            data = self.f.tell()
            self.starts.append(pos)
            self.chunks.append((ctype, nblk * self.bs, data))
            self.f.seek(data + total - chs)
            pos += nblk * self.bs if ctype != 0xcac4 else 0
        self.size = pos

    def read(self, off, n):
        out = bytearray()
        while n > 0:
            i = bisect.bisect_right(self.starts, off) - 1
            ctype, length, data = self.chunks[i]
            inner = off - self.starts[i]
            take = min(n, length - inner)
            if ctype == 0xcac1:
                self.f.seek(data + inner)
                out += self.f.read(take)
            elif ctype == 0xcac2:
                self.f.seek(data)
                fill = self.f.read(4)
                out += (fill * (take // 4 + 2))[inner % 4:inner % 4 + take]
            else:
                out += bytes(take)
            off += take
            n -= take
        return bytes(out)


def partitions(img):
    geo = img.read(4096, 52)
    if struct.unpack('<I', geo[:4])[0] != 0x616c4467:
        raise ValueError('no LP geometry')
    base = 4096 + 2 * 4096
    h = img.read(base, 256)
    magic, major, minor, hsize = struct.unpack('<IHHI', h[:12])
    if magic != 0x414c5030:
        raise ValueError('no LP metadata')
    tables = struct.unpack('<12I', h[80:128])     # partitions, extents, groups, block devices: offset, count, size
    t = base + hsize
    poff, pn, psz, eoff, en, esz = tables[:6]
    ext = [struct.unpack('<QIQI', img.read(t + eoff + i * esz, 24)) for i in range(en)]
    out = {}
    for i in range(pn):
        e = img.read(t + poff + i * psz, psz)
        name = e[:36].split(b'\0')[0].decode()
        first, num = struct.unpack('<II', e[40:48])
        out[name] = [(sectors * 512, target * 512) for sectors, ttype, target, _ in ext[first:first + num] if ttype == 0]
    return out


def main():
    img = Sparse(sys.argv[1])
    parts = partitions(img)
    if len(sys.argv) < 4:
        for name, exts in parts.items():
            print('%-24s %8d MiB' % (name, sum(n for n, _ in exts) >> 20))
        return
    with open(sys.argv[3], 'wb') as out:
        for length, phys in parts[sys.argv[2]]:
            for off in range(0, length, 1 << 24):
                out.write(img.read(phys + off, min(1 << 24, length - off)))
    print('%s: %d MiB' % (sys.argv[3], sum(n for n, _ in parts[sys.argv[2]]) >> 20))


if __name__ == '__main__':
    main()
