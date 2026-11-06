#!/usr/bin/env python3
"""Build vivausbd for macOS and run the usbmux + PTP host emulators against it.

  python3 tools/vivad/tests/run_tests.py [--keep] [-v]
  VIVAUSBD_SAN=thread python3 tools/vivad/tests/run_tests.py   (default: address,undefined)

Everything runs in a temporary directory: a fake Media storage (with symlinks that point
out of it), a lockdownd stand-in (length-prefixed plist QueryType server) mapped to 62078,
and the daemon in --test-sock mode. Exit status 0 = all green.
"""
import os
import plistlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mux_host  # noqa: E402
import ptp_host  # noqa: E402

UDID = '00008030-001A2B3C4D5E6F70'


def plist_server():
    ls = socket.socket()
    ls.bind(('127.0.0.1', 0))
    ls.listen(8)

    def serve(s):
        try:
            while True:
                h = s.recv(4, socket.MSG_WAITALL)
                if len(h) < 4:
                    break
                (n,) = struct.unpack('>I', h)
                req = plistlib.loads(s.recv(n, socket.MSG_WAITALL))
                rep = {'Request': req.get('Request'), 'Type': 'com.apple.mobile.lockdown'}
                b = plistlib.dumps(rep)
                s.sendall(struct.pack('>I', len(b)) + b)
        except OSError:
            pass
        s.close()

    def acc():
        while True:
            s, _ = ls.accept()
            threading.Thread(target=serve, args=(s,), daemon=True).start()
    threading.Thread(target=acc, daemon=True).start()
    return ls.getsockname()[1]


def make_storage(base):
    root = os.path.join(base, 'Media')
    alb = os.path.join(root, 'DCIM', '100APPLE')
    os.makedirs(alb)
    os.makedirs(os.path.join(root, 'Downloads'))
    os.makedirs(os.path.join(root, 'PhotoData', 'Thumbs', 'V2'))
    files = {
        'IMG_0001.JPG': os.urandom(200000),
        'IMG_0002.HEIC': os.urandom(5 * 1024 * 1024 + 4321),
        'IMG_0003.MOV': os.urandom(131072),     # exactly 2 x 64 KiB of payload
        'IMG_0004.PNG': b'\x89PNG' + os.urandom(1000),
    }
    for n, d in files.items():
        with open(os.path.join(alb, n), 'wb') as f:
            f.write(d)
    with open(os.path.join(root, 'ünïcødé 📷.txt'), 'w') as f:
        f.write('hello')
    open(os.path.join(root, 'PhotoData', 'Thumbs', 'V2', 'empty'), 'w').close()
    os.makedirs(os.path.join(base, 'outside'))
    with open(os.path.join(base, 'outside', 'secret.txt'), 'w') as f:
        f.write('must not be visible')
    os.symlink('../outside', os.path.join(root, 'escape'))
    os.symlink('../../outside/secret.txt', os.path.join(root, 'DCIM', 'escape2.txt'))
    return root


def main():
    keep = '--keep' in sys.argv
    verbose = '-v' in sys.argv
    base = tempfile.mkdtemp(prefix='vusb')
    binpath = os.path.join(base, 'vivausbd')
    src = os.path.join(os.path.dirname(HERE), 'vivausbd.c')
    subprocess.check_call(['clang', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Werror',
                           '-fsanitize=' + os.environ.get('VIVAUSBD_SAN', 'address,undefined'), '-g',
                           '-o', binpath, src])
    root = make_storage(base)
    sockdir = os.path.join(base, 's')
    os.makedirs(sockdir)
    lport = plist_server()
    args = [binpath, '--test-sock', sockdir, '--root', root, '--udid', UDID,
            '--map', '62078=127.0.0.1:%d' % lport, '--owner', '%d:%d' % (os.getuid(), os.getgid())]
    if verbose:
        args.append('-v')
    log = open(os.path.join(base, 'daemon.log'), 'w')
    d = subprocess.Popen(args, stderr=log, stdout=log)
    try:
        time.sleep(0.3)
        rc_ptp = ptp_host.main(sockdir, root, UDID)
        rc_mux = mux_host.main(sockdir, mux_host.echo_server(), mux_host.closer_server())
        time.sleep(0.2)
        alive = d.poll() is None
        print('daemon still running: %s' % alive)
    finally:
        d.terminate()
        d.wait()
        log.close()
    with open(os.path.join(base, 'daemon.log')) as f:
        text = f.read()
    if 'Sanitizer' in text or 'runtime error' in text:
        print(text[-4000:])
        print('FAIL sanitizer report')
        rc_ptp = 1
    elif verbose:
        print(text[-3000:])
    if not keep:
        shutil.rmtree(base, ignore_errors=True)
    else:
        print('kept', base)
    ok = rc_ptp == 0 and rc_mux == 0 and alive
    print('ALL GREEN' if ok else 'FAILURES')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
