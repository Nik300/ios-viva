"""Simulated phone for viva-link.py (VIVA_LINK_FAKE=1): no USB involved.

The phone enumerates as persona `link`, streams console lines ('L', ~20 lines/s with bursts of a few hundred),
answers commands ('R'/'E'), runs programs for `run` ('O'), takes push/pull, and after VIVA_LINK_FAKE_SWITCH
seconds (default 12, 0 = never) switches to persona `std` like vivausbd does: gone for ~300 ms, then back.
`panic` and `reboot` drop it for a few seconds. VIVA_LINK_FAKE=stall makes the heartbeat request stall
(old kernel). Tests call BUS.drop() to simulate a disconnect.
"""
import errno, os, queue, random, threading, time

try:
    from usb.core import USBError, USBTimeoutError
except ImportError:
    class USBError(IOError):
        def __init__(self, strerror, error_code=None, errno=None):
            IOError.__init__(self, errno, strerror)
            self.backend_error_code = error_code

    class USBTimeoutError(USBError):
        pass


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h


SUBSYS = ('AppleSweetDisplay', 'VivaBattery', 'viva_usb', 'AGXViva', 'IOSurface', 'launchd', 'SpringBoard',
          'backboardd', 'AppleQcomI2C', 'VivaWifi', 'AMFI', 'Sandbox', 'mDNSResponder', 'kernel')
LINES = (
    '{s}: vsync {n} frames, {f:.1f} fps',
    '{s}: job 0x{h:x} done in {f:.2f} ms',
    '{s}: IOSurface 0x{h:x} mapped, {n} pages',
    '{s}: charger IINLIM {n} mA, VBUS 5.0{m} V',
    '{s}: warning: retry {m} on bus 3 addr 0x6a',
    '{s}: error: transfer timeout (status 0x{h:x})',
    'Sandbox: {s}({n}) deny(1) file-read-data /private/var/mobile/Library/Preferences/com.apple.{s}.plist',
    '{s}[{n}]: com.apple.{s}.agent: service exited with abnormal code: 1',
    'VIVA LINK: heartbeat ok, host active',
    '{s}: dart map iova 0x{h:x} len 0x4000 ' + 'flags 0x3 ' * 18,
    '{s}: mem free {n} pages, wired {m}{n} pages, compressor {f:.1f}%',
)
HELP = ('commands: ping, stat, help, peek PA [WORDS], poke PA VALUE, pmic, i2c, i2cw, i2c8, i2c8w, bat, wifi, '
        'cpu, gpu, disp, push, pull, run, vivad, rewind [BYTES], console, panic [TEXT], reboot\n')


class Bus:
    """One simulated phone that re-enumerates (persona switch, reboot, panic, drop())."""
    def __init__(self):
        self.phone = None
        self.persona = 'link'
        self.back_at = time.time() + 0.8
        self.lock = threading.Lock()

    def find(self):
        with self.lock:
            if self.phone is None and time.time() >= self.back_at:
                self.phone = Phone(self, self.persona)
            return self.phone

    def drop(self, gap=0.3, persona=None):
        with self.lock:
            if self.phone:
                self.phone.gone = True
            self.phone = None
            self.persona = persona or self.persona
            self.back_at = time.time() + gap


BUS = Bus()


def find_link(Port):
    phone = BUS.find()
    if phone is None:
        return None
    return Port(phone, 0 if phone.persona == 'link' else 2, 0x81, 0x01, phone.persona)


class Phone:
    speed = 3
    iProduct = 2

    def __init__(self, bus, persona):
        self.bus, self.persona = bus, persona
        self.q = queue.Queue()
        self.gone = False
        self.pushing = None             # [size, hash, path, bytearray]
        self.jobs = 0
        self.active = None
        self.born = time.time()
        self.seq = 0
        threading.Thread(target=self.console, daemon=True).start()

    # pyusb device interface
    def read(self, ep, size, timeout=0):
        t0 = time.time()
        while True:
            if self.gone:
                raise USBError('No such device (it may have been disconnected)', -4, errno.ENODEV)
            try:
                return self.q.get(timeout=0.05)
            except queue.Empty:
                if timeout and time.time() - t0 > timeout / 1000:
                    raise USBTimeoutError('Operation timed out', -7, errno.ETIMEDOUT)

    def write(self, ep, data, timeout=None):
        if self.gone:
            raise USBError('No such device (it may have been disconnected)', -4, errno.ENODEV)
        data = bytes(data)
        if self.pushing:
            p = self.pushing
            p[3] += data
            if len(p[3]) >= p[0]:
                self.pushing = None
                h = fnv1a(bytes(p[3][:p[0]]))
                ok = 'ok' if h == p[1] else 'BAD checksum'
                self.reply('push %s: %s, %d bytes, fnv1a 0x%08x\n' % (p[2], ok, p[0], h))
        else:
            self.command(data.decode(errors='replace').strip())
        return len(data)

    def ctrl_transfer(self, bm, req, value, index, data=None, timeout=None):
        if self.gone:
            raise USBError('No such device (it may have been disconnected)', -4, errno.ENODEV)
        if os.environ.get('VIVA_LINK_FAKE') == 'stall':
            raise USBError('Pipe error', -9, errno.EPIPE)
        self.active = value             # 1 heartbeat, 0 bye
        return 0

    # frames
    def frame(self, kind, data):
        for at in range(0, max(len(data), 1), 508):
            chunk = data[at:at + 508]
            self.q.put(b'V' + kind.encode() + len(chunk).to_bytes(2, 'little') + chunk)

    def log(self, text):
        self.frame('L', text.encode())

    def reply(self, text):
        self.frame('R', text.encode())
        self.frame('E', b'')

    def console(self):
        r = random.Random()
        switch = float(os.environ.get('VIVA_LINK_FAKE_SWITCH', '12'))
        self.log('viva_usb: configured (persona %s), Viva Link waiting for the host tool\n' % self.persona)
        burst_at = time.time() + 4
        while not self.gone:
            if self.persona == 'link' and switch and time.time() - self.born > switch:
                self.log('vivausbd: VIVAUSB_ATTACH, switching to the std persona\n')
                time.sleep(0.05)
                self.bus.drop(0.3, 'std')
                return
            n = 1
            if time.time() > burst_at:          # a long burst, like launchd starting a pile of daemons
                n = r.randint(200, 600)
                burst_at = time.time() + r.uniform(5, 9)
            text = ''
            for _ in range(n):
                self.seq += 1
                text += r.choice(LINES).format(s=r.choice(SUBSYS), n=r.randint(1, 99999), m=r.randint(0, 9),
                                               h=r.getrandbits(32), f=r.uniform(0, 120)) + '\n'
            self.log(text)
            time.sleep(r.uniform(0.02, 0.09))

    def command(self, line):
        words = line.split()
        if not words:
            return self.reply('')
        c, args = words[0], words[1:]
        if c == 'ping':
            self.reply('pong\n')
        elif c == 'help':
            self.reply(HELP)
        elif c == 'stat':
            self.reply('uptime %d s, persona %s, log ring 512 KiB (%d lines sent)\nusb: high speed, %s\n'
                       % (time.time() - self.born, self.persona, self.seq,
                          'Viva Link active' if os.environ.get('VIVA_LINK_FAKE') != 'stall' else 'no heartbeat'))
        elif c == 'peek' and args:
            pa, words_ = int(args[0], 0) & ~3, int(args[1], 0) if len(args) > 1 else 1
            out = ''
            for at in range(pa, pa + 4 * words_, 16):
                out += '0x%x: %s\n' % (at, ' '.join('%08x' % ((a * 2654435761) & 0xffffffff)
                                                    for a in range(at, min(at + 16, pa + 4 * words_), 4)))
            self.reply(out)
        elif c in ('bat', 'cpu', 'gpu', 'disp', 'wifi', 'pmic'):
            self.reply('%s: fake %s status\n  value 1: %d\n  value 2: 0x%x\n' % (c, c, random.randint(0, 100),
                                                                                random.getrandbits(16)))
        elif c == 'push' and len(args) >= 4:
            self.pushing = [int(args[0]), int(args[2]), args[3], bytearray()]
            self.reply('ready\n')
        elif c == 'pull' and args:
            data = bytes((i * 7) & 0xff for i in range(150000))
            self.frame('F', data)
            self.reply('pull %s: %d bytes, fnv1a 0x%08x\n' % (args[0], len(data), fnv1a(data)))
        elif c == 'run' and args:
            self.jobs += 1
            self.reply('vivad: %d queued\n' % self.jobs)
            threading.Thread(target=self.job, args=(self.jobs, args), daemon=True).start()
        elif c == 'panic':
            self.reply('panicking\n')
            self.log('panic(cpu 0 caller 0xfffffe0007a1c2d4): "%s"\nDebugger message: panic\n'
                     % (' '.join(args) or 'viva-link panic command'))
            time.sleep(0.2)
            self.bus.drop(3)
        elif c == 'reboot':
            self.reply('rebooting\n')
            time.sleep(0.2)
            self.bus.drop(2, 'link')
        else:
            self.reply('unknown command: %s (help)\n' % c)

    def job(self, n, args):
        time.sleep(0.2)
        for i in range(8):
            self.frame('O', ('[%d] %s: step %d of 8\n' % (n, args[0], i + 1)).encode())
            time.sleep(0.1)
        self.frame('O', ('[%d] exit %d\n' % (n, 0 if args[0] != 'false' else 1)).encode())
