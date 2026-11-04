#!/usr/bin/env python3
"""Host side of the viva USB link (kernel boot-arg vivausb=3, patch 0093).

  viva-link.py [-o LOGFILE]            live kernel console + interactive commands
  viva-link.py [-o LOGFILE] log        live console only
  viva-link.py cmd COMMAND...          send one command, print the reply, exit
  viva-link.py dump PA SIZE FILE       save physical memory (via peek) to FILE
  viva-link.py dumpdtb [FILE]          save LK's merged DTB (default viva-live.dtb)
  viva-link.py push LOCAL PATH [MODE]  copy a file to the phone (MODE octal, default 644;
                                       written next to PATH, renamed when its checksum matches)
  viva-link.py pull PATH [LOCAL]       copy a file from the phone
  viva-link.py run PROGRAM [ARGS..]    run it through vivad, print its output, exit with its status
                                       (PROGRAM without '/' is in /private/var/root/viva-tests)

The phone shows up as 1209:0002 "viva iOS link" (vendor class, bulk 0x81 IN,
0x01 OUT). Every IN packet is a frame: 'V', type, u16 length, payload with
type 'L' (console log), 'R' (command reply), 'E' (end of reply), 'F' (file
data of a pull) or 'O' (output of programs run by vivad).
Phone commands: ping, stat, help, peek PA [WORDS], poke PA VALUE,
rewind [BYTES], panic [TEXT], reboot.

Needs pyusb and libusb (macOS: brew install libusb; python3 -m pip install pyusb).
"""
import argparse, os, re, sys, threading, time

try:
    import usb.core, usb.util, usb.backend.libusb1
except ImportError:
    sys.exit('pyusb missing: python3 -m pip install pyusb (and brew install libusb)')

VID, PID = 0x1209, 0x0002
EP_IN, EP_OUT = 0x81, 0x01


def backend():
    for lib in ('/opt/homebrew/lib/libusb-1.0.dylib', '/usr/local/lib/libusb-1.0.dylib'):
        if os.path.exists(lib):
            return usb.backend.libusb1.get_backend(find_library=lambda _: lib)
    return None             # pyusb's own search (Linux)


def wait_device():
    shown = False
    while True:
        dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend())
        if dev is not None:
            return dev
        if not shown:
            print('waiting for the phone (%04x:%04x)...' % (VID, PID), file=sys.stderr)
            shown = True
        time.sleep(0.5)


def open_device():
    dev = wait_device()
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except (NotImplementedError, usb.core.USBError):
        pass
    usb.util.claim_interface(dev, 0)
    print('connected: %s, %s speed' % (usb.util.get_string(dev, dev.iProduct),
          {3: 'high', 2: 'full'}.get(dev.speed, 'unknown')), file=sys.stderr)
    return dev


class Link:
    def __init__(self, logfile=None, show_log=True):
        self.dev = open_device()
        self.log = open(logfile, 'ab') if logfile else None
        self.show_log = show_log
        self.reply_done = threading.Event()
        self.reply = bytearray()
        self.file = bytearray()             # 'F' frames of a pull
        self.output = bytearray()           # 'O' frames (vivad)
        self.output_event = threading.Event()
        self.lock = threading.Lock()

    def frames(self):
        """Yield (type, payload) until the device goes away."""
        while True:
            try:
                pkt = bytes(self.dev.read(EP_IN, 512, timeout=0))
            except usb.core.USBTimeoutError:
                continue
            if len(pkt) < 4 or pkt[0] != ord('V'):
                continue
            n = pkt[2] | pkt[3] << 8
            yield chr(pkt[1]), pkt[4:4 + n]

    def pump(self):
        try:
            for kind, data in self.frames():
                if kind == 'L':
                    if self.log:
                        self.log.write(data)
                        self.log.flush()
                    if self.show_log:
                        sys.stdout.buffer.write(data)
                        sys.stdout.flush()
                elif kind == 'R':
                    with self.lock:
                        self.reply += data
                elif kind == 'F':
                    with self.lock:
                        self.file += data
                elif kind == 'O':
                    if self.log:
                        self.log.write(data)
                        self.log.flush()
                    sys.stdout.buffer.write(data)
                    sys.stdout.flush()
                    with self.lock:
                        self.output += data
                    self.output_event.set()
                elif kind == 'E':
                    self.reply_done.set()
        except usb.core.USBError as e:
            print('\nlink lost: %s' % e, file=sys.stderr)
            os._exit(1)

    def command(self, line, timeout=10):
        with self.lock:
            self.reply = bytearray()
        self.reply_done.clear()
        self.dev.write(EP_OUT, line.encode()[:511], timeout=2000)
        if not self.reply_done.wait(timeout):
            return '(no reply in %d s)\n' % timeout
        with self.lock:
            return self.reply.decode(errors='replace')


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h


def push(link, local, path, mode):
    data = open(local, 'rb').read()
    h = fnv1a(data)
    reply = link.command('push %d %d %d %s' % (len(data), mode, h, path))
    if 'ready' not in reply:
        sys.exit(reply.strip())
    with link.lock:
        link.reply = bytearray()
    link.reply_done.clear()
    t0 = time.time()
    for at in range(0, len(data), 1 << 16):
        link.dev.write(EP_OUT, data[at:at + (1 << 16)], timeout=10000)
        print('\r%d / %d KiB' % (min(at + (1 << 16), len(data)) >> 10, len(data) >> 10), end='', file=sys.stderr)
    print(' (%.1f s)' % (time.time() - t0), file=sys.stderr)
    if not link.reply_done.wait(60):
        sys.exit('no final reply from the phone')
    with link.lock:
        text = link.reply.decode(errors='replace')
    print(text.strip())
    if 'ok' not in text or ('0x%08x' % h) not in text:
        sys.exit(1)


def pull(link, path, local):
    with link.lock:
        link.file = bytearray()
    reply = link.command('pull %s' % path, timeout=600)
    with link.lock:
        data = bytes(link.file)
    m = re.search(r'pull .*: (\d+) bytes, fnv1a 0x([0-9a-f]{8})', reply)
    if not m or int(m.group(1)) != len(data) or int(m.group(2), 16) != fnv1a(data):
        sys.exit('pull failed: %s (got %d bytes, fnv1a 0x%08x)' % (reply.strip(), len(data), fnv1a(data)))
    open(local, 'wb').write(data)
    print('%s: %d bytes from %s' % (local, len(data), path), file=sys.stderr)


def run(link, args):
    reply = link.command('run ' + ' '.join('"%s"' % a if ' ' in a else a for a in args))
    m = re.search(r'vivad: (\d+) queued', reply)
    if not m:
        sys.exit(reply.strip())
    end = re.compile(rb'\[%s\] (exit|signal) (-?\d+)' % m.group(1).encode())
    while True:
        link.output_event.wait()
        link.output_event.clear()
        with link.lock:
            e = end.search(link.output)
        if e:
            sys.exit(int(e.group(2)) if e.group(1) == b'exit' else 128 + int(e.group(2)))


def dump(link, pa, size):
    """Read physical memory with peek (64 words per command)."""
    out = bytearray()
    start = pa & ~3
    end = pa + size
    for at in range(start, end, 256):
        words = min(64, (end - at + 3) // 4)
        reply = link.command('peek 0x%x %d' % (at, words))
        vals = [w for line in reply.splitlines() if ':' in line for w in line.split(':', 1)[1].split()]
        if len(vals) != words:
            sys.exit('bad peek reply at 0x%x: %r' % (at, reply[:200]))
        for v in vals:
            out += int(v, 16).to_bytes(4, 'little')
        if (at - start) % 0x4000 == 0:
            print('\r%d / %d KiB' % ((at - start) >> 10, size >> 10), end='', file=sys.stderr)
    print(file=sys.stderr)
    return bytes(out[pa - start:pa - start + size])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-o', '--output', help='append the console log to this file')
    ap.add_argument('mode', nargs='?', default='interactive',
                    choices=('interactive', 'log', 'cmd', 'dump', 'dumpdtb', 'push', 'pull', 'run'))
    ap.add_argument('command', nargs='*')
    a = ap.parse_args()

    link = Link(a.output, show_log=(a.mode not in ('cmd', 'dump', 'dumpdtb', 'push', 'pull', 'run')))
    threading.Thread(target=link.pump, daemon=True).start()
    if a.mode == 'push':
        if len(a.command) < 2:
            sys.exit('push LOCAL PATH [MODE]')
        push(link, a.command[0], a.command[1], int(a.command[2], 8) if len(a.command) > 2 else 0o644)
        return
    if a.mode == 'pull':
        if not a.command:
            sys.exit('pull PATH [LOCAL]')
        pull(link, a.command[0], a.command[1] if len(a.command) > 1 else os.path.basename(a.command[0]))
        return
    if a.mode == 'run':
        if not a.command:
            sys.exit('run PROGRAM [ARGS..]')
        run(link, a.command)
        return
    if a.mode in ('dump', 'dumpdtb'):
        if a.mode == 'dumpdtb':     # LK's merged DTB (base + DTBO), still in RAM
            pa, out = 0x4bc80000, (a.command or ['viva-live.dtb'])[0]
            hdr = dump(link, pa, 8)
            if hdr[:4] != b'\xd0\x0d\xfe\xed':
                sys.exit('no FDT at 0x%x (%s)' % (pa, hdr.hex()))
            size = int.from_bytes(hdr[4:8], 'big')
        else:
            pa, size, out = int(a.command[0], 0), int(a.command[1], 0), a.command[2]
        data = dump(link, pa, size)
        open(out, 'wb').write(data)
        print('%s: %d bytes from 0x%x' % (out, len(data), pa), file=sys.stderr)
        return
    if a.mode == 'cmd':
        sys.stdout.write(link.command(' '.join(a.command)))
        return
    if a.mode == 'log':
        threading.Event().wait()
    print('type a phone command and Enter (help for the list, Ctrl-D quits)', file=sys.stderr)
    for line in sys.stdin:
        line = line.strip()
        if line:
            text = '\n> %s\n%s' % (line, link.command(line))
            sys.stdout.write(text)
            sys.stdout.flush()
            if link.log:        # commands and replies go to the log file too
                link.log.write(text.encode())
                link.log.flush()


if __name__ == '__main__':
    # The log pump is a daemon thread blocked in libusb: a normal interpreter shutdown tears libusb down under it
    # and segfaults on macOS (seen after "cmd wifi"). Leave with os._exit once the output is flushed.
    code = 0
    try:
        main()
    except KeyboardInterrupt:
        pass
    except SystemExit as e:
        if isinstance(e.code, str):
            print(e.code, file=sys.stderr)
            code = 1
        else:
            code = e.code or 0
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)
