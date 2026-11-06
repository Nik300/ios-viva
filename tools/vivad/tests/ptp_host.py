#!/usr/bin/env python3
"""PTP/MTP initiator for vivausbd --test-sock (run by run_tests.py; main(sockdir, root, udid))."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from vtest import Endpoint, check, Fail, RESET, CANCEL  # noqa: E402

CMD, DATA, RESP = 1, 2, 3
OK = 0x2001
STORAGE = 0x00010001


def pstr(b, off):
    n = b[off]
    off += 1
    s = b[off:off + 2 * n].decode('utf-16-le').rstrip('\0')
    return s, off + 2 * n


def mkstr(s):
    if not s:
        return b'\0'
    u = (s + '\0').encode('utf-16-le')
    return bytes([len(u) // 2]) + u


class PTP:
    def __init__(self, sockdir):
        self.ep = Endpoint(os.path.join(sockdir, 'ptp.sock'), timeout=20)
        self.tid = 0
        self.last_data_frames = []

    def _container(self):
        """Reassemble one IN container by its length; remembers the write sizes."""
        f = self.ep.recv()
        frames = [len(f)]
        (length,) = struct.unpack('<I', f[:4])
        buf = bytearray(f)
        while len(buf) < length:
            f = self.ep.recv()
            frames.append(len(f))
            buf += f
        check(len(buf) == length, 'container overrun: %d > %d' % (len(buf), length))
        typ, code, tid = struct.unpack('<HHI', buf[4:12])
        return typ, code, tid, bytes(buf[12:]), frames

    def op(self, code, *params, data_out=None, split_header=False, expect_data=None):
        self.tid += 1
        tid = self.tid
        self.ep.send(struct.pack('<IHHI', 12 + 4 * len(params), CMD, code, tid) +
                     b''.join(struct.pack('<I', p) for p in params))
        if data_out is not None:
            hdr = struct.pack('<IHHI', 12 + len(data_out), DATA, code, tid)
            if split_header:
                # header alone, then the payload as 512 KiB transfers (like ImageCaptureCore)
                self.ep.send(hdr)
                for i in range(0, len(data_out), 512 * 1024):
                    self.ep.send(data_out[i:i + 512 * 1024])
            else:
                self.ep.send(hdr + data_out)
        data = None
        typ, rc, rtid, payload, frames = self._container()
        if typ == DATA:
            check(rc == code and rtid == tid, 'data phase code/tid %x/%d' % (rc, rtid))
            data = payload
            self.last_data_frames = frames
            check(all(n <= 65536 for n in frames), 'a data write > 64 KiB: %r' % frames[:4])
            typ, rc, rtid, payload, _ = self._container()
        check(typ == RESP, 'expected a response, got type %d' % typ)
        check(rtid == tid, 'response tid %d != %d' % (rtid, tid))
        params = list(struct.unpack('<%dI' % (len(payload) // 4), payload))
        if expect_data is not None:
            check((data is not None) == expect_data, 'data phase presence for %04x' % code)
        return rc, params, data

    def ok(self, code, *params, **kw):
        rc, p, d = self.op(code, *params, **kw)
        check(rc == OK, 'op %04x params %r -> %04x' % (code, params, rc))
        return p, d


def parse_devinfo(d):
    std, vext, vver = struct.unpack('<HIH', d[:8])
    desc, off = pstr(d, 8)
    off += 2
    arrays = []
    for _ in range(5):
        (n,) = struct.unpack('<I', d[off:off + 4])
        arrays.append(struct.unpack('<%dH' % n, d[off + 4:off + 4 + 2 * n]))
        off += 4 + 2 * n
    strs = []
    for _ in range(4):
        s, off = pstr(d, off)
        strs.append(s)
    return dict(ext=desc, vext=vext, ops=arrays[0], manufacturer=strs[0], model=strs[1], serial=strs[3])


def parse_objinfo(d):
    storage, fmt, prot, size = struct.unpack('<IHHI', d[:12])
    parent, assoc = struct.unpack('<IH', d[38:44])
    name, off = pstr(d, 52)
    cdate, off = pstr(d, off)
    mdate, off = pstr(d, off)
    return dict(storage=storage, fmt=fmt, size=size, parent=parent, assoc=assoc, name=name, mdate=mdate)


def objectinfo_dataset(name, size, fmt=0x3000):
    d = struct.pack('<IHHIHIIIIIIIHII', 0, fmt, 0, size, 0, 0, 0, 0, 0, 0, 0, 0,
                    1 if fmt == 0x3001 else 0, 0, 0)
    check(len(d) == 52, 'dataset layout')
    return d + mkstr(name) + mkstr('') + mkstr('') + mkstr('')


def main(sockdir, root, udid):
    results = []
    p = PTP(sockdir)

    def test(name, fn):
        try:
            fn()
            results.append((name, None))
            print('PASS ptp', name, flush=True)
        except Fail as e:
            results.append((name, str(e)))
            print('FAIL ptp', name, '-', e, flush=True)

    def devinfo():
        _, d = p.ok(0x1001, expect_data=True)
        di = parse_devinfo(d)
        check(di['ext'] == 'microsoft.com: 1.0;' and di['vext'] == 6, 'vendor extension %r' % di['ext'])
        check(di['manufacturer'] == 'Apple Inc.' and di['model'] == 'iPhone', 'strings %r' % di)
        check(di['serial'] == udid, 'serial %r' % di['serial'])
        check(0x9805 in di['ops'] and 0x100D in di['ops'], 'ops list')
    test('GetDeviceInfo', devinfo)
    test('OpenSession', lambda: p.ok(0x1002, 1))

    def storage():
        _, d = p.ok(0x1004)
        check(struct.unpack('<II', d) == (1, STORAGE), 'storage ids %r' % d)
        _, d = p.ok(0x1005, STORAGE)
        typ, fs, acc, maxc, free = struct.unpack('<HHHQQ', d[:22])
        st = os.statvfs(root)
        check(maxc == st.f_blocks * st.f_frsize or maxc > 0, 'capacity')
        check(free > 0 and free <= maxc, 'free %d max %d' % (free, maxc))
        desc, _ = pstr(d, 26)
        check(desc == 'iPhone', 'description %r' % desc)
    test('GetStorageIDs/GetStorageInfo (statfs)', storage)

    tree = {}

    def walk(parent, relbase):
        _, d = p.ok(0x1007, STORAGE, 0, parent)
        n = struct.unpack('<I', d[:4])[0]
        hs = struct.unpack('<%dI' % n, d[4:4 + 4 * n])
        for h in hs:
            _, oi = p.ok(0x1008, h)
            info = parse_objinfo(oi)
            rel = os.path.join(relbase, info['name']) if relbase else info['name']
            check(info['parent'] == (0 if parent == 0xffffffff else parent), 'parent of %s' % rel)
            tree[rel] = (h, info)
            if info['fmt'] == 0x3001:
                check(info['assoc'] == 1, 'association type of %s' % rel)
                walk(h, rel)

    def listing():
        tree.clear()
        walk(0xffffffff, '')
        expect = set()
        for dp, dns, fns in os.walk(root):
            for n in dns + fns:
                full = os.path.join(dp, n)
                if os.path.islink(full):
                    continue
                expect.add(os.path.relpath(full, root))
            dns[:] = [x for x in dns if not os.path.islink(os.path.join(dp, x))]
        check(set(tree) == expect, 'listing differs: extra %r missing %r' %
              (sorted(set(tree) - expect)[:5], sorted(expect - set(tree))[:5]))
        for rel, (h, info) in tree.items():
            if info['fmt'] != 0x3001:
                check(info['size'] == os.path.getsize(os.path.join(root, rel)), 'size of %s' % rel)
        check(tree['DCIM/100APPLE/IMG_0001.JPG'][1]['fmt'] == 0x3801, 'JPEG format')
        check(tree['DCIM/100APPLE/IMG_0003.MOV'][1]['fmt'] == 0x300D, 'MOV format')
        check(tree['DCIM/100APPLE/IMG_0004.PNG'][1]['fmt'] == 0x380B, 'PNG format')
        check(not any('escape' in k for k in tree), 'symlink listed')
        rc, prm, _ = p.op(0x1006, STORAGE, 0, 0)
        check(rc == OK and prm[0] == len(expect), 'GetNumObjects(all) %r vs %d' % (prm, len(expect)))
    test('recursive listing == filesystem (no symlinks)', listing)

    def getobject():
        h = tree['DCIM/100APPLE/IMG_0002.HEIC'][0]
        _, d = p.ok(0x1009, h)
        want = open(os.path.join(root, 'DCIM/100APPLE/IMG_0002.HEIC'), 'rb').read()
        check(d == want, 'GetObject bytes differ (%d vs %d)' % (len(d), len(want)))
        fr = p.last_data_frames
        check(len(fr) > 1 and fr[0] == 65536, 'data phase writes %r' % fr[:3])
        prm, d = p.ok(0x101B, h, 1000000, 300000)
        check(d == want[1000000:1300000] and prm == [300000], 'GetPartialObject')
        rc, _, _ = p.op(0x100A, h)
        check(rc == 0x2010, 'GetThumb -> %04x' % rc)
    test('GetObject multi-MB byte-for-byte, GetPartialObject, GetThumb', getobject)

    def mtp_props():
        _, d = p.ok(0x9801, 0x3000)
        n = struct.unpack('<I', d[:4])[0]
        props = struct.unpack('<%dH' % n, d[4:4 + 2 * n])
        for need in (0xDC01, 0xDC02, 0xDC03, 0xDC04, 0xDC07, 0xDC09, 0xDC0B, 0xDC41, 0xDC44):
            check(need in props, 'prop %04x missing' % need)
        _, d = p.ok(0x9802, 0xDC07, 0x3000)
        check(struct.unpack('<HHB', d[:5]) == (0xDC07, 0xFFFF, 0), 'PropDesc filename')
        h, info = tree['DCIM/100APPLE/IMG_0001.JPG']
        _, d = p.ok(0x9803, h, 0xDC07)
        check(pstr(d, 0)[0] == 'IMG_0001.JPG', 'PropValue filename')
        _, d = p.ok(0x9803, h, 0xDC04)
        check(struct.unpack('<Q', d)[0] == info['size'], 'PropValue size')
        # whole property list of the root level (what Windows asks first)
        _, d = p.ok(0x9805, 0xffffffff, 0, 0xffffffff, 0, 1)
        n = struct.unpack('<I', d[:4])[0]
        off, names = 4, set()
        for _ in range(n):
            hh, pc, dt = struct.unpack('<IHH', d[off:off + 8])
            off += 8
            if dt == 0xFFFF:
                s, off = pstr(d, off)
                if pc == 0xDC07:
                    names.add(s)
            else:
                off += {2: 1, 4: 2, 6: 4, 8: 8, 10: 16}[dt]
        check(off == len(d), 'prop list parse %d != %d' % (off, len(d)))
        check(names == {k for k in tree if '/' not in k}, 'prop list names %r' % names)
        _, d = p.ok(0x1015, 0xD402)
        check(pstr(d, 0)[0] == 'iPhone', 'friendly name')
        _, d = p.ok(0x1014, 0x5001)
        check(struct.unpack('<HH', d[:4]) == (0x5001, 2), 'battery desc')
    test('MTP object properties + device props', mtp_props)

    def send_object():
        dcim = tree['DCIM'][0]
        album = tree['DCIM/100APPLE'][0]
        for split, name in ((True, 'upload1.bin'), (False, 'upload2.jpg')):
            data = os.urandom(3 * 1024 * 1024 + 77) if split else os.urandom(40000)
            prm, _ = p.ok(0x100C, STORAGE, album, data_out=objectinfo_dataset(name, len(data)),
                          split_header=split)
            check(prm[0] == STORAGE and prm[1] == album and prm[2] > 0, 'SendObjectInfo params %r' % prm)
            h = prm[2]
            p.ok(0x100D, data_out=data, split_header=split)
            path = os.path.join(root, 'DCIM/100APPLE', name)
            check(open(path, 'rb').read() == data, 'stored file differs (%s)' % name)
            _, d = p.ok(0x1007, STORAGE, 0, album)
            hs = struct.unpack('<%dI' % struct.unpack('<I', d[:4])[0], d[4:])
            check(h in hs, 'new handle not listed')
            _, d = p.ok(0x1009, h)
            check(d == data, 'read back')
        # new folder
        prm, _ = p.ok(0x100C, STORAGE, dcim, data_out=objectinfo_dataset('NEWALBUM', 0, 0x3001))
        check(os.path.isdir(os.path.join(root, 'DCIM/NEWALBUM')), 'folder not created')
        tree['DCIM/NEWALBUM'] = (prm[2], None)
        # traversal attempts
        for bad in ('../evil', 'a/b', '..', '.'):
            rc, _, _ = p.op(0x100C, STORAGE, dcim, data_out=objectinfo_dataset(bad, 4))
            check(rc != OK, 'name %r accepted' % bad)
        check(not os.path.exists(os.path.join(os.path.dirname(root), 'evil')), 'escaped the root')
        rc, _, _ = p.op(0x100D, data_out=b'oops')
        check(rc != OK, 'SendObject without a valid SendObjectInfo accepted')
    test('SendObjectInfo/SendObject (split + inline), mkdir, traversal refused', send_object)

    def delete():
        h = tree['DCIM/100APPLE/IMG_0004.PNG'][0]
        p.ok(0x100B, h, 0)
        check(not os.path.exists(os.path.join(root, 'DCIM/100APPLE/IMG_0004.PNG')), 'file not deleted')
        rc, _, _ = p.op(0x1008, h)
        check(rc == 0x2009, 'deleted handle still valid (%04x)' % rc)
        p.ok(0x100B, tree['DCIM/NEWALBUM'][0], 0)
        check(not os.path.exists(os.path.join(root, 'DCIM/NEWALBUM')), 'folder not deleted')
        check(os.path.exists(os.path.join(os.path.dirname(root), 'outside', 'secret.txt')), 'outside touched')
    test('DeleteObject file + folder', delete)

    def misc():
        rc, _, _ = p.op(0x9999)
        check(rc == 0x2005, 'unknown op -> %04x' % rc)
        rc, _, _ = p.op(0x1008, 123456)
        check(rc == 0x2009, 'bad handle -> %04x' % rc)
    test('unknown op / bad handle', misc)

    def cancel():
        album = tree['DCIM/100APPLE'][0]
        prm, _ = p.ok(0x100C, STORAGE, album, data_out=objectinfo_dataset('cancelled.bin', 4 << 20))
        p.tid += 1
        p.ep.send(struct.pack('<IHHI', 12, CMD, 0x100D, p.tid))
        p.ep.send(struct.pack('<IHHI', 12 + (4 << 20), DATA, 0x100D, p.tid) + b'x' * 100000)
        p.ep.signal(CANCEL)
        _, d = p.ok(0x1001)        # next transaction works
        check(parse_devinfo(d)['model'] == 'iPhone', 'after cancel')
        check(not os.path.exists(os.path.join(root, 'DCIM/100APPLE/cancelled.bin')), 'partial file kept')
    test('cancel during SendObject data phase', cancel)

    def reset():
        p.ep.signal(RESET)
        _, d = p.ok(0x1001)
        p.ok(0x1002, 7)
        _, d = p.ok(0x1007, STORAGE, 0, 0xffffffff)
        check(struct.unpack('<I', d[:4])[0] >= 1, 'listing after reset')
        p.ok(0x1003)
    test('bus reset, new session, CloseSession', reset)

    bad = [r for r in results if r[1]]
    print('ptp: %d/%d passed' % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
