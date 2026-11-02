#!/usr/bin/env python3
"""Extract the iPhone X (D22AP) device tree from a local IPSW for mkadt.py's /product.

  viva-extract-dt.py IPSW [OUT]     (OUT default research/ipsw-x/d22ap.adt)

The IM4P payload is LZFSE-compressed ("bvx2"); macOS libcompression decodes it,
so neither `ipsw` nor remotezip is needed.
"""
import ctypes
import os
import sys
import zipfile

MEMBER = 'Firmware/all_flash/DeviceTree.d22ap.im4p'
COMPRESSION_LZFSE = 0x801


def der(buf, off):
    """(tag, value_start, value_end) of the DER element at off."""
    tag = buf[off]
    n = buf[off + 1]
    off += 2
    if n & 0x80:
        k = n & 0x7f
        n = int.from_bytes(buf[off:off + k], 'big')
        off += k
    return tag, off, off + n


def im4p_payload(blob):
    tag, start, end = der(blob, 0)
    assert tag == 0x30, 'not a DER sequence'
    items, off = [], start
    while off < end:
        t, s, e = der(blob, off)
        items.append((t, blob[s:e]))
        off = e
    assert items[0] == (0x16, b'IM4P'), 'not an IM4P'
    return next(v for t, v in items[1:] if t == 0x04)


def lzfse_decode(data):
    lib = ctypes.CDLL('/usr/lib/libcompression.dylib')
    lib.compression_decode_buffer.restype = ctypes.c_size_t
    cap = len(data) * 16
    while True:
        out = ctypes.create_string_buffer(cap)
        n = lib.compression_decode_buffer(out, cap, data, len(data), None, COMPRESSION_LZFSE)
        if n == 0:
            sys.exit('LZFSE decode failed')
        if n < cap:
            return out.raw[:n]
        cap *= 2


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[2] if len(sys.argv) == 3 else os.path.join(here, '../research/ipsw-x/d22ap.adt')
    payload = im4p_payload(zipfile.ZipFile(sys.argv[1]).read(MEMBER))
    adt = lzfse_decode(payload) if payload[:4] == b'bvx2' else payload
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, 'wb').write(adt)
    print(f'{out}: {len(adt)} bytes')


if __name__ == '__main__':
    main()
