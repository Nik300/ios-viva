#!/usr/bin/env python3
"""Minimal ELF64 helper for the viva shim build (no binutils on macOS).

  viva-check-abs.py OBJ...            fail if any object has R_AARCH64_ABS64 relocations
  viva-check-abs.py --symbol NAME ELF print the symbol's value (decimal)
"""
import struct
import sys

R_AARCH64_ABS64 = 257
SHT_SYMTAB, SHT_RELA = 2, 4


def sections(d):
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, _ = struct.unpack_from('<HHH', d, 0x3a)
    return [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]


def symbol(path, want):
    d = open(path, 'rb').read()
    secs = sections(d)
    symtab = next(s for s in secs if s[1] == SHT_SYMTAB)
    strtab = secs[symtab[6]]
    for o in range(symtab[4], symtab[4] + symtab[5], 24):
        name, _, _, _, value, _ = struct.unpack_from('<IBBHQQ', d, o)
        start = strtab[4] + name
        if d[start:d.index(b'\0', start)] == want.encode():
            return value
    sys.exit(f'{path}: no symbol {want}')


def abs_relocs(path):
    d = open(path, 'rb').read()
    n = 0
    for s in sections(d):
        if s[1] == SHT_RELA:
            for o in range(s[4], s[4] + s[5], 24):
                _, info, _ = struct.unpack_from('<QQq', d, o)
                n += (info & 0xffffffff) == R_AARCH64_ABS64
    return n


if __name__ == '__main__':
    if sys.argv[1] == '--symbol':
        print(symbol(sys.argv[3], sys.argv[2]))
    else:
        bad = {p: abs_relocs(p) for p in sys.argv[1:]}
        bad = {p: n for p, n in bad.items() if n}
        if bad:
            sys.exit(f'error: absolute relocations: {bad}')
