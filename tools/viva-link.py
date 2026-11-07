#!/usr/bin/env python3
"""Host side of the viva USB link (kernel boot-arg vivausb=3, patches 0093 and 0182).

  viva-link.py [-o LOGFILE]            full-screen interface: command prompt + command output + phone log
                                       (needs textual; without it, the plain line mode of old)
  viva-link.py [-o LOGFILE] log        live console only
  viva-link.py cmd COMMAND...          send one command, print the reply, exit
  viva-link.py dump PA SIZE FILE       save physical memory (via peek) to FILE
  viva-link.py dumpdtb [FILE]          save LK's merged DTB (default viva-live.dtb)
  viva-link.py push LOCAL PATH [MODE]  copy a file to the phone (MODE octal, default 644;
                                       written next to PATH, renamed when its checksum matches)
  viva-link.py pull PATH [LOCAL]       copy a file from the phone
  viva-link.py run PROGRAM [ARGS..]    run it through vivad, print its output, exit with its status
                                       (PROGRAM without '/' is in /private/var/root/viva-tests)
  viva-link.py bootrec [hex|show]      edit the VIVABOOT block (next boot's boot-args in userdata, patch 0180)
                                       in $EDITOR (default vim; EDITOR=nano ...): fields and command line, or
                                       with `hex` the raw 4096 bytes; written when saved changed. Needs
                                       /private/var/root/viva-tests/vbootrec (xnu-kernel artifact, tools/vivad) and kernel 0180

The phone is either 1209:0002 "viva iOS link" (persona `link`, the link is interface 0) or, once vivausbd
switched it (patch 0182, docs/VIVA-USB-PERSONA.md), 05ac:12a8 (persona `std`: PTP, usbmux and the link as the
interface of class ff/56/01). Only the link interface is claimed; its bulk endpoints come from the descriptors.
The interactive and `log` modes wait for the phone and reattach by themselves when it re-enumerates (persona
switch, reboot); the one-shot modes exit when the link goes away. While attached, every mode sends the
"Viva Link active" heartbeat (vendor request 0x41/0x56 wValue 1, every second) and a bye (wValue 0) when the
interactive or log mode exits (one-shot commands just stop: the phone drops "active" 4 s later).

Every IN packet is a frame: 'V', type, u16 length, payload with type 'L' (console log), 'R' (command
reply), 'E' (end of reply), 'F' (file data of a pull) or 'O' (output of programs run by vivad).
Phone commands: see PHONE_COMMANDS (`help` on the phone has the arguments).

Interactive keys: Enter sends, Up/Down history (~/.viva-link-history), Tab completes, PgUp/PgDn scroll the
selected pane (Shift-Tab switches it), End resumes following, Ctrl-F filters the log, F2 layout, F3 maximise,
Ctrl-C/Ctrl-D quit. Local commands start with ':' (or '/'): :help lists them.

VIVA_LINK_FAKE=1 replaces the phone with a simulator (tools/viva_link_fake.py) for testing without USB.

Needs pyusb and libusb (macOS: brew install libusb; python3 -m pip install pyusb), textual for the interface.
"""
import argparse, collections, errno, os, re, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
FAKE = bool(os.environ.get('VIVA_LINK_FAKE'))
try:
    import usb.core, usb.util, usb.backend.libusb1
    USBError, USBTimeoutError = usb.core.USBError, usb.core.USBTimeoutError
except ImportError:
    if not FAKE:
        sys.exit('pyusb missing: python3 -m pip install pyusb (and brew install libusb)')
    from viva_link_fake import USBError, USBTimeoutError

VID, PID = 0x1209, 0x0002           # persona `link`: the link is interface 0
STD_VID, STD_PID = 0x05ac, 0x12a8   # persona `std`: the link is the interface of class ff/56/01
LINK_CLASS = (0xff, 0x56, 0x01)
EP_IN, EP_OUT = 0x81, 0x01          # only if the descriptors say nothing
PHONE_COMMANDS = ('ping', 'stat', 'help', 'peek', 'poke', 'pmic', 'i2c', 'i2cw', 'i2c8', 'i2c8w', 'bat', 'wifi',
                  'cpu', 'gpu', 'disp', 'push', 'pull', 'run', 'vivad', 'rewind', 'console', 'panic', 'reboot')
LINKS = []                          # every Link, for the bye at exit
BYE = True                          # one-shot modes leave without a bye (see main)

# dev: pyusb device (or the simulator's); ifn: link interface number; persona: 'link' or 'std'
Port = collections.namedtuple('Port', 'dev ifn ep_in ep_out persona')


def backend():
    for lib in ('/opt/homebrew/lib/libusb-1.0.dylib', '/usr/local/lib/libusb-1.0.dylib'):
        if os.path.exists(lib):
            return usb.backend.libusb1.get_backend(find_library=lambda _: lib)
    return None             # pyusb's own search (Linux)


def bulk_endpoints(intf):
    eps = {usb.util.endpoint_direction(e.bEndpointAddress): e.bEndpointAddress
           for e in intf if usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK}
    return eps.get(usb.util.ENDPOINT_IN, EP_IN), eps.get(usb.util.ENDPOINT_OUT, EP_OUT)


def find_link():
    """The phone's link interface in either persona (Port), or None. Descriptors only: nothing is opened."""
    if FAKE:
        import viva_link_fake
        return viva_link_fake.find_link(Port)
    be = backend()
    try:
        dev = usb.core.find(idVendor=VID, idProduct=PID, backend=be)
        if dev is not None:
            return Port(dev, 0, *bulk_endpoints(dev[0][(0, 0)]), 'link')
        # 05ac:12a8 is any iPhone: only ours has the link interface
        for dev in usb.core.find(find_all=True, idVendor=STD_VID, idProduct=STD_PID, backend=be):
            for cfg in dev:
                for intf in cfg:
                    if (intf.bInterfaceClass, intf.bInterfaceSubClass, intf.bInterfaceProtocol) == LINK_CLASS:
                        return Port(dev, intf.bInterfaceNumber, *bulk_endpoints(intf), 'std')
    except (USBError, IndexError, KeyError):     # device going away while we look
        pass
    return None


def stalled(e):
    return getattr(e, 'errno', None) == errno.EPIPE or getattr(e, 'backend_error_code', None) == -9


class Link:
    def __init__(self, logfile=None, show_log=True, reconnect=False):
        self.port = None                    # Port while attached
        self.state = 'waiting'              # waiting (never attached) / connected / lost (waiting again)
        self.reconnect = reconnect          # reattach when the phone goes away (else exit)
        self.info = ''                      # product, speed
        self.heartbeat = None               # None, 'ok' or 'unsupported' (old kernels stall the request)
        self.last_beat = 0.0
        self.closing = False
        self.attaches = 0
        self.log_bytes = 0
        self.on_log = self.on_output = None # set by the interface: called by the pump thread with the bytes
        self.log = open(logfile, 'ab') if logfile else None
        self.logfile = logfile
        self.show_log = show_log
        self.reply_done = threading.Event()
        self.reply = bytearray()
        self.file = bytearray()             # 'F' frames of a pull
        self.output = bytearray()           # 'O' frames (vivad)
        self.output_event = threading.Event()
        self.echo_output = True             # print 'O' frames (run_capture turns it off)
        self.lock = threading.Lock()
        LINKS.append(self)
        threading.Thread(target=self.beat, daemon=True).start()

    def note(self, msg, end='\n', err=True):
        """Progress and state messages, results with err=False (the interface replaces it)."""
        print(msg, end=end, file=sys.stderr if err else sys.stdout, flush=True)

    def connect(self):
        """Wait for the phone and claim its link interface."""
        shown = False
        while True:
            port = find_link()
            if port is not None:
                try:
                    self.claim(port)
                    break
                except USBError as e:       # just enumerated, or someone else has it: try again
                    if not shown:
                        self.note('cannot claim the link interface (%s), retrying' % e)
                        shown = True
            elif not shown:
                self.note('waiting for the phone (%04x:%04x or %04x:%04x)...' % (VID, PID, STD_VID, STD_PID))
                shown = True
            time.sleep(0.25)
        dev = port.dev
        product = 'viva iOS link'
        if not FAKE:
            try:
                product = usb.util.get_string(dev, dev.iProduct) or product
            except (USBError, ValueError):
                pass
        self.info = '%s, %s speed' % (product, {3: 'high', 2: 'full', 4: 'super'}.get(dev.speed, 'unknown'))
        self.hello(port)                    # active right away, not at the next beat
        self.port = port
        self.state = 'connected'
        self.attaches += 1
        self.note('connected: %s (persona %s, interface %d)' % (self.info, port.persona, port.ifn))

    def claim(self, port):
        if FAKE:
            return
        try:
            if port.dev.is_kernel_driver_active(port.ifn):
                port.dev.detach_kernel_driver(port.ifn)
        except (NotImplementedError, USBError):
            pass
        usb.util.claim_interface(port.dev, port.ifn)    # never PTP/usbmux: macOS daemons own those

    def lost(self, e):
        port, self.port = self.port, None
        self.state = 'lost'
        self.heartbeat = None
        self.reply_done.set()               # wake a command waiting for its reply
        self.output_event.set()
        if port and not FAKE:
            try:
                usb.util.dispose_resources(port.dev)
            except USBError:
                pass
        self.note('link lost: %s' % e)

    def beat(self):
        """'Viva Link active' heartbeat, every second while attached."""
        while True:
            self.hello(self.port)
            time.sleep(1)

    def hello(self, port):
        if port and not self.closing:
            try:
                port.dev.ctrl_transfer(0x41, 0x56, 1, port.ifn, None, timeout=500)
                self.heartbeat, self.last_beat = 'ok', time.time()
            except USBError as e:
                if stalled(e):
                    self.heartbeat = 'unsupported'

    def bye(self):
        self.closing = True                 # no heartbeat after the bye
        port = self.port
        if port:
            try:
                port.dev.ctrl_transfer(0x41, 0x56, 0, port.ifn, None, timeout=300)
            except USBError:
                pass

    def frames(self):
        """Yield (type, payload) until the device goes away."""
        port = self.port
        while True:
            try:
                # Several frames per read: a full 512-byte frame does not end the transfer, so the next packets
                # join it; a 512-byte buffer then fails with EOVERFLOW ("link lost: [Errno 84] Overflow").
                buf = bytes(port.dev.read(port.ep_in, 16384, timeout=0))
            except USBTimeoutError:
                continue
            at = 0
            while at + 4 <= len(buf) and buf[at] == ord('V'):
                n = buf[at + 2] | buf[at + 3] << 8
                yield chr(buf[at + 1]), buf[at + 4:at + 4 + n]
                at += 4 + n

    def write(self, data, timeout=2000):
        port = self.port
        if port is None:
            raise USBError('phone not connected')
        port.dev.write(port.ep_out, data, timeout=timeout)

    def pump(self):
        while True:
            if self.port is None:
                self.connect()
            try:
                for kind, data in self.frames():
                    self.frame(kind, data)
            except USBError as e:
                if not self.reconnect:
                    print('\nlink lost: %s' % e, file=sys.stderr)
                    os._exit(1)
                self.lost(e)
                time.sleep(0.1)

    def frame(self, kind, data):
        if kind == 'L':
            self.log_bytes += len(data)
            if self.log:
                self.log.write(data)
                self.log.flush()
            if self.on_log:
                self.on_log(data)
            elif self.show_log:
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
            if self.echo_output:
                if self.on_output:
                    self.on_output(data)
                else:
                    sys.stdout.buffer.write(data)
                    sys.stdout.flush()
            with self.lock:
                self.output += data
            self.output_event.set()
        elif kind == 'E':
            self.reply_done.set()

    def command(self, line, timeout=10):
        with self.lock:
            self.reply = bytearray()
        self.reply_done.clear()
        try:
            self.write(line.encode()[:511])
        except USBError as e:
            return '(not sent: %s)\n' % e
        if not self.reply_done.wait(timeout):
            return '(no reply in %d s)\n' % timeout
        if self.port is None:
            return '(link lost)\n'
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
        link.write(data[at:at + (1 << 16)], timeout=10000)
        link.note('\r%d / %d KiB' % (min(at + (1 << 16), len(data)) >> 10, len(data) >> 10), end='')
    link.note(' (%.1f s)' % (time.time() - t0))
    if not link.reply_done.wait(60):
        sys.exit('no final reply from the phone')
    with link.lock:
        text = link.reply.decode(errors='replace')
    link.note(text.strip(), err=False)
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
    link.note('%s: %d bytes from %s' % (local, len(data), path))


def run(link, args):
    with link.lock:
        link.output = bytearray()
    reply = link.command('run ' + ' '.join('"%s"' % a if ' ' in a else a for a in args))
    m = re.search(r'vivad: (\d+) queued', reply)
    if not m:
        sys.exit(reply.strip())
    end = re.compile(rb'\[%s\] (exit|signal) (-?\d+)' % m.group(1).encode())
    while True:
        link.output_event.wait()
        link.output_event.clear()
        if link.port is None:
            sys.exit('link lost')
        with link.lock:
            e = end.search(link.output)
        if e:
            sys.exit(int(e.group(2)) if e.group(1) == b'exit' else 128 + int(e.group(2)))


def run_capture(link, args):
    """Run through vivad; return (output without the "[id] " prefixes, exit status)."""
    with link.lock:
        link.output = bytearray()
    link.echo_output = False
    reply = link.command('run ' + ' '.join('"%s"' % a if ' ' in a else a for a in args))
    m = re.search(r'vivad: (\d+) queued', reply)
    if not m:
        sys.exit(reply.strip())
    tag = b'[%s] ' % m.group(1).encode()
    end = re.compile(rb'\[%s\] (exit|signal) (-?\d+)' % m.group(1).encode())
    while True:
        link.output_event.wait(30)
        link.output_event.clear()
        if link.port is None:
            sys.exit('link lost')
        with link.lock:
            out = bytes(link.output)
        e = end.search(out)
        if e:
            break
    lines = [l[len(tag):] for l in out.split(b'\n') if l.startswith(tag) and not end.match(l)]
    status = int(e.group(2)) if e.group(1) == b'exit' else 128 + int(e.group(2))
    return b'\n'.join(lines).decode(errors='replace') + '\n', status


TESTS = '/private/var/root/viva-tests'


def edit(text, suffix):
    """Open text in $EDITOR (default vim); return the saved text."""
    import shlex, subprocess, tempfile
    with tempfile.NamedTemporaryFile('w', suffix=suffix, delete=False) as f:
        f.write(text)
        path = f.name
    try:
        subprocess.call(shlex.split(os.environ.get('EDITOR') or 'vim') + [path])
        return open(path).read()
    finally:
        os.unlink(path)


def hexdump(data):
    return ''.join('%04x: %s  |%s|\n' % (o, ' '.join('%02x' % b for b in data[o:o + 16]),
                   ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[o:o + 16]))
                   for o in range(0, len(data), 16))


def unhexdump(text):
    out = bytearray()
    for line in text.splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        body = line.split(':', 1)[1].split('|', 1)[0]
        out += bytes(int(w, 16) for w in body.split())
    return bytes(out)


def bootrec(link, args):
    """The VIVABOOT record (shim/vivaboot.h) in an editor; written through vbootrec on the phone."""
    import tempfile
    mode = args[0] if args else 'edit'
    if mode in ('edit', 'show'):
        text, st = run_capture(link, ['vbootrec', 'text'])
        if st:
            sys.exit(text.strip() or 'vbootrec text: exit %d' % st)
        if mode == 'show':
            sys.stdout.write(text)
            return
        new = edit(text, '.vivaboot.txt')
        if new == text:
            print('no changes: nothing written')
            return
        local = tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False)
        local.write(new)
        local.close()
        push(link, local.name, TESTS + '/vbootrec-edit.txt', 0o600)
        os.unlink(local.name)
        out, st = run_capture(link, ['vbootrec', 'put-text', TESTS + '/vbootrec-edit.txt'])
    elif mode == 'hex':
        out, st = run_capture(link, ['vbootrec', 'raw', TESTS + '/vbootrec.bin'])
        if st:
            sys.exit(out.strip())
        local = tempfile.NamedTemporaryFile(suffix='.bin', delete=False)
        local.close()
        pull(link, TESTS + '/vbootrec.bin', local.name)
        data = open(local.name, 'rb').read()
        text = ('# VIVABOOT block, 4096 bytes (layout: fork/shim/vivaboot.h). Edit the hex bytes; the |ascii| column\n'
                '# is ignored. Written exactly as saved: line_hash (+0x20) is NOT recomputed here (use `bootrec`).\n'
                + hexdump(data))
        new = edit(text, '.vivaboot.hex')
        if new == text:
            os.unlink(local.name)
            print('no changes: nothing written')
            return
        raw = unhexdump(new)
        if len(raw) != 4096:
            os.unlink(local.name)
            sys.exit('%d bytes after editing, need 4096: nothing written' % len(raw))
        open(local.name, 'wb').write(raw)
        push(link, local.name, TESTS + '/vbootrec.bin', 0o600)
        os.unlink(local.name)
        out, st = run_capture(link, ['vbootrec', 'put-raw', TESTS + '/vbootrec.bin'])
    else:
        sys.exit('bootrec [hex|show]')
    sys.stdout.write(out)
    if st:
        sys.exit(st)
    text, _ = run_capture(link, ['vbootrec', 'text'])
    print('\nnow:')
    sys.stdout.write(''.join(l + '\n' for l in text.splitlines() if not l.startswith('# Save') and not l.startswith('# flags')))


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
                    choices=('interactive', 'log', 'cmd', 'dump', 'dumpdtb', 'push', 'pull', 'run', 'bootrec'))
    ap.add_argument('command', nargs='*')
    a = ap.parse_args()

    oneshot = a.mode not in ('interactive', 'log')
    # A bye after every one-shot command flips the phone's link state (island, charger input limit) at each command
    # of a script; that churn hung SpringBoard (2026-11-07). Without it the kernel drops "active" 4 s after the last
    # heartbeat, so a run of commands is one link session.
    global BYE
    BYE = not oneshot
    link = Link(a.output, show_log=not oneshot, reconnect=not oneshot)
    if a.mode == 'interactive':
        try:
            import viva_link_tui
        except ImportError as e:
            print('(plain mode, %s: python3 -m pip install textual for the full-screen interface)' % e, file=sys.stderr)
        else:
            viva_link_tui.run(link, sys.modules[__name__])     # attaches by itself, shows the wait
            return
    link.connect()
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
    if a.mode == 'bootrec':
        bootrec(link, a.command)
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
    plain_loop(link)


def plain_loop(link):
    """The line mode of old: log and replies interleaved on stdout."""
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
    for l in LINKS if BYE else []:
        l.bye()
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)
