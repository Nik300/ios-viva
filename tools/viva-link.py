#!/usr/bin/env python3
"""Host side of the viva USB link (kernel boot-arg vivausb=3, patch 0093).

  viva-link.py [-o LOGFILE]            live kernel console + interactive commands
  viva-link.py [-o LOGFILE] log        live console only
  viva-link.py cmd COMMAND...          send one command, print the reply, exit

The phone shows up as 1209:0002 "viva iOS link" (vendor class, bulk 0x81 IN,
0x01 OUT). Every IN packet is a frame: 'V', type, u16 length, payload with
type 'L' (console log), 'R' (command reply) or 'E' (end of reply).
Phone commands: ping, stat, help, peek PA [WORDS], poke PA VALUE,
rewind [BYTES], panic [TEXT], reboot.

Needs pyusb and libusb (macOS: brew install libusb; python3 -m pip install pyusb).
"""
import argparse, os, sys, threading, time

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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-o', '--output', help='append the console log to this file')
    ap.add_argument('mode', nargs='?', default='interactive', choices=('interactive', 'log', 'cmd'))
    ap.add_argument('command', nargs='*')
    a = ap.parse_args()

    link = Link(a.output, show_log=(a.mode != 'cmd'))
    threading.Thread(target=link.pump, daemon=True).start()
    if a.mode == 'cmd':
        sys.stdout.write(link.command(' '.join(a.command)))
        return
    if a.mode == 'log':
        threading.Event().wait()
    print('type a phone command and Enter (help for the list, Ctrl-D quits)', file=sys.stderr)
    for line in sys.stdin:
        line = line.strip()
        if line:
            sys.stdout.write('\n> %s\n%s' % (line, link.command(line)))
            sys.stdout.flush()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
