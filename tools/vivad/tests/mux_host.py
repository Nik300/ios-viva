#!/usr/bin/env python3
"""usbmux host emulator for vivausbd --test-sock: speaks what usbmuxd (src/device.c) sends.

Run by run_tests.py, which starts the plist server, the daemon (--test-sock DIR
--map 62078=127.0.0.1:<plist server>) and calls main().
Checks, besides the payloads: one mux packet per IN transfer, <= 32 KiB, data packets never
a multiple of 512, device tx_seq +1 per packet, flags exactly ACK on data, the device never
exceeds the window the host advertised, and the host never exceeds the device's window.
"""
import os
import plistlib
import random
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
from vtest import Endpoint, check, Fail, RESET  # noqa: E402

VERSION, CONTROL, SETUP, TCP = 0, 1, 2, 6
FIN, SYN, RST, ACK = 1, 2, 4, 0x10
MAGIC = 0xfeedface
USB_MTU = 3 * 16384
TCP_HDR = 20


class Conn:
    def __init__(self, host, sport, dport, win):
        self.host = host
        self.sport, self.dport = sport, dport
        self.state = 'connecting'
        self.tx_seq = 0          # host's next seq
        self.tx_ack = 0          # host's ack (device bytes received + 1)
        self.rx_ack = 0          # device's ack of our bytes
        self.rx_win = 0          # device window
        self.win = win           # our window (bytes)
        self.acked_sent = 0
        self.win_sent = win
        self.inbuf = bytearray()
        self.rst = False
        self.violations = []


class MuxHost:
    def __init__(self, sockdir, win=131072):
        self.ep = Endpoint(os.path.join(sockdir, 'mux.sock'), timeout=None)
        self.version = 0
        self.tx_seq = 0
        self.rx_seq = 0xffff
        self.dev_seq = None
        self.cv = threading.Condition()
        self.conns = {}
        self.orphans = []
        self.next_sport = 1
        self.default_win = win
        self.errors = []
        self.got_version = None
        self.closed = False
        self.t = threading.Thread(target=self._rx, daemon=True)
        self.t.start()

    # ---- packets
    def _packet(self, proto, payload, v1=False):
        if v1 or self.version < 2:
            hdr = struct.pack('>II', proto, 8 + len(payload))
        else:
            if proto == SETUP:
                self.tx_seq, self.rx_seq = 0, 0xffff
            hdr = struct.pack('>IIIHH', proto, 16 + len(payload), MAGIC, self.tx_seq, self.rx_seq)
            self.tx_seq = (self.tx_seq + 1) & 0xffff
        return hdr + payload

    def send_packet(self, proto, payload):
        with self.cv:
            pkt = self._packet(proto, payload)
        self.ep.send(pkt)

    def tcp(self, c, flags, data=b''):
        th = struct.pack('>HHIIBBHHH', c.sport, c.dport, c.tx_seq, c.tx_ack, 0x50, flags,
                         min(c.win >> 8, 0xffff), 0, 0)
        with self.cv:
            pkt = self._packet(TCP, th + data)
            if flags & ACK:
                c.acked_sent, c.win_sent = c.tx_ack, c.win
        self.ep.send(pkt)

    def handshake(self):
        self.ep.send(self._packet(VERSION, struct.pack('>III', 2, 0, 0), v1=True))
        with self.cv:
            check(self.cv.wait_for(lambda: self.got_version is not None, 5), 'no VERSION reply')
        major, minor = self.got_version
        check(major == 2 and minor == 0, 'version reply %d.%d' % (major, minor))
        self.version = 2
        self.dev_seq = None
        self.send_packet(SETUP, b'\x07')

    # ---- receive thread
    def _rx(self):
        try:
            while True:
                f = self.ep.recv()
                self._input(f)
        except (EOFError, OSError):
            with self.cv:
                self.closed = True
                self.cv.notify_all()

    def _err(self, msg):
        self.errors.append(msg)

    def _input(self, f):
        if len(f) > 32768:
            self._err('IN transfer of %d bytes > 32 KiB' % len(f))
        proto, length = struct.unpack('>II', f[:8])
        if length != len(f):
            self._err('packet length %d != transfer %d' % (length, len(f)))
        with self.cv:
            if proto == VERSION:
                major, minor, _ = struct.unpack('>III', f[8:20])
                check_len = len(f) == 20
                if not check_len:
                    self._err('VERSION reply is %d bytes' % len(f))
                self.got_version = (major, minor)
                self.cv.notify_all()
                return
            magic, txs, rxs = struct.unpack('>IHH', f[8:16])
            if magic != MAGIC:
                self._err('bad magic %x' % magic)
            if self.dev_seq is not None and txs != ((self.dev_seq + 1) & 0xffff):
                self._err('device tx_seq %d after %d' % (txs, self.dev_seq))
            self.dev_seq = txs
            self.rx_seq = txs
            if proto != TCP:
                self._err('unexpected device protocol %d' % proto)
                return
            p = f[16:]
            dport_dev, sport_host, seq, ack, off, flags, win = struct.unpack('>HHIIBBH', p[:16])
            data = p[(off >> 4) * 4:]
            if data and len(f) % 512 == 0:
                self._err('data packet of %d bytes (multiple of 512)' % len(f))
            c = self.conns.get(sport_host)
            if c is None or c.dport != dport_dev:
                self.orphans.append((sport_host, dport_dev, flags))
                self.cv.notify_all()
                return
            c.rx_ack, c.rx_win = ack, win << 8
            if c.state == 'connecting':
                if flags == SYN | ACK:
                    c.state = 'connected'
                    c.tx_seq += 1
                    c.tx_ack += 1
                    c.ack_needed = True
                else:
                    c.state = 'refused'
                    c.rst = bool(flags & RST)
            elif c.state == 'connected':
                if flags != ACK:
                    if flags & ~(RST | ACK):
                        self._err('device flags %#x on connected socket' % flags)
                    c.state = 'reset'
                    c.rst = True
                else:
                    if data:
                        end = seq + len(data)
                        if (end - c.acked_sent) & 0xffffffff > c.win_sent:
                            c.violations.append((end - c.acked_sent, c.win_sent))
                        c.inbuf += data
                        c.tx_ack = (c.tx_ack + len(data)) & 0xffffffff
            self.cv.notify_all()
        if c.state == 'connected' and getattr(c, 'ack_needed', False):
            c.ack_needed = False
            self.tcp(c, ACK)

    # ---- client API (what a usbmuxd client gets)
    def connect(self, dport, win=None):
        with self.cv:
            sport = self.next_sport
            self.next_sport += 1
            c = Conn(self, sport, dport, win or self.default_win)
            self.conns[sport] = c
        self.tcp(c, SYN)
        with self.cv:
            self.cv.wait_for(lambda: c.state != 'connecting', 5)
        return c

    def send(self, c, data):
        off = 0
        while off < len(data):
            with self.cv:
                ok = self.cv.wait_for(lambda: c.state != 'connected' or
                                      c.rx_win - ((c.tx_seq - c.rx_ack) & 0xffffffff) > 0, 10)
                check(ok, 'device window never opened (sent %d)' % off)
                check(c.state == 'connected', 'connection %s while sending' % c.state)
                sendable = c.rx_win - ((c.tx_seq - c.rx_ack) & 0xffffffff)
                n = min(sendable, USB_MTU - 16 - TCP_HDR, len(data) - off)
            self.tcp(c, ACK, data[off:off + n])
            with self.cv:
                c.tx_seq = (c.tx_seq + n) & 0xffffffff
            off += n

    def recv(self, c, n, timeout=10):
        """Exactly n bytes (less only if the connection ends); ACKs whatever it consumes."""
        got = bytearray()
        deadline = time.time() + timeout
        while len(got) < n:
            with self.cv:
                self.cv.wait_for(lambda: c.inbuf or c.state != 'connected', max(0, deadline - time.time()))
                k = min(len(c.inbuf), n - len(got))
                got += c.inbuf[:k]
                del c.inbuf[:k]
                state = c.state
            if k:
                self.tcp(c, ACK)       # consumed: window update
            elif state != 'connected' or time.time() >= deadline:
                break
        check(len(got) == n or c.state != 'connected', 'timeout receiving (%d of %d)' % (len(got), n))
        return bytes(got)

    def close(self, c):
        self.tcp(c, RST)
        with self.cv:
            c.state = 'closed'
            self.conns.pop(c.sport, None)


# ------------------------------------------------------------------------- tests

def echo_server():
    """TCP echo server; records connections closed by the peer."""
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', 0))
    ls.listen(16)
    st = {'closed': 0, 'opened': 0}

    def serve(s):
        st['opened'] += 1
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                s.sendall(d)
        except OSError:
            pass
        st['closed'] += 1
        s.close()

    def acc():
        while True:
            s, _ = ls.accept()
            threading.Thread(target=serve, args=(s,), daemon=True).start()
    threading.Thread(target=acc, daemon=True).start()
    return ls.getsockname()[1], st


def closer_server():
    """Sends 'bye\\n' and closes: the device must forward it and then RST."""
    ls = socket.socket()
    ls.bind(('127.0.0.1', 0))
    ls.listen(4)

    def acc():
        while True:
            s, _ = ls.accept()
            s.sendall(b'bye\n')
            s.close()
    threading.Thread(target=acc, daemon=True).start()
    return ls.getsockname()[1]


def lockdown_query(h):
    c = h.connect(62078)
    check(c.state == 'connected', 'lockdown connect: %s' % c.state)
    req = plistlib.dumps({'Label': 'mux_host', 'Request': 'QueryType'})
    h.send(c, struct.pack('>I', len(req)) + req)
    (n,) = struct.unpack('>I', h.recv(c, 4))
    rep = plistlib.loads(h.recv(c, n))
    check(rep.get('Type') == 'com.apple.mobile.lockdown', 'QueryType reply %r' % rep)
    h.close(c)


def main(sockdir, echo_srv, closer_port):
    echo_port = echo_srv[0]
    results = []

    def test(name, fn):
        try:
            fn()
            results.append((name, None))
            print('PASS mux', name, flush=True)
        except Fail as e:
            results.append((name, str(e)))
            print('FAIL mux', name, '-', e, flush=True)

    h = MuxHost(sockdir)

    test('version + setup', h.handshake)
    test('lockdown QueryType (62078 -> plist server)', lambda: lockdown_query(h))

    def big_echo():
        c = h.connect(echo_port, win=16384)      # small host window: device must honour it
        check(c.state == 'connected', 'echo connect %s' % c.state)
        data = os.urandom(3 * 1024 * 1024 + 123)
        got = bytearray()
        t = threading.Thread(target=lambda: h.send(c, data))
        t.start()
        while len(got) < len(data):
            got += h.recv(c, min(65536, len(data) - len(got)))
        t.join()
        check(bytes(got) == data, 'echo payload differs')
        check(not c.violations, 'device exceeded the host window: %r' % c.violations[:3])
        h.close(c)
    test('3 MiB echo through a 16 KiB host window', big_echo)

    def concurrent():
        port = echo_port
        errs = []

        def one(i):
            try:
                c = h.connect(port)
                check(c.state == 'connected', 'connect %d' % i)
                data = bytes(random.getrandbits(8) for _ in range(1000)) * 600
                t = threading.Thread(target=lambda: h.send(c, data))
                t.start()
                got = h.recv(c, len(data), timeout=30)
                t.join()
                check(got == data, 'conn %d payload differs' % i)
                h.close(c)
            except Exception as e:  # noqa: BLE001
                errs.append(str(e))
        ts = [threading.Thread(target=one, args=(i,)) for i in range(6)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        check(not errs, '; '.join(errs))
    test('6 concurrent connections x 600 KB', concurrent)

    def refused():
        s = socket.socket()
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
        s.close()
        c = h.connect(port)
        check(c.state == 'refused' and c.rst, 'closed port gave %s' % c.state)
    test('SYN to a closed port -> RST', refused)

    def host_rst():
        _, st = echo_srv
        before = st['closed']
        c = h.connect(echo_port)
        h.send(c, b'hello')
        check(h.recv(c, 5) == b'hello', 'echo')
        h.close(c)
        deadline = time.time() + 3
        while st['closed'] == before and time.time() < deadline:
            time.sleep(0.02)
        check(st['closed'] > before, 'service socket not closed after host RST')
    test('host RST closes the service socket', host_rst)

    def service_close():
        c = h.connect(closer_port)
        check(c.state == 'connected', 'connect')
        with h.cv:
            h.cv.wait_for(lambda: c.state != 'connected', 5)
        check(c.state == 'reset' and c.rst, 'no RST after service close (%s)' % c.state)
        check(bytes(c.inbuf) == b'bye\n', 'data before RST: %r' % bytes(c.inbuf))
        h.conns.pop(c.sport, None)
    test('service close -> data then RST', service_close)

    def coalesced():
        # two packets in one transfer, then one packet split over two transfers
        c1 = h.connect(echo_port)
        c2 = h.connect(echo_port)
        th = lambda c, d: struct.pack('>HHIIBBHHH', c.sport, c.dport, c.tx_seq, c.tx_ack, 0x50, ACK,
                                      c.win >> 8, 0, 0) + d
        with h.cv:
            p1 = h._packet(TCP, th(c1, b'one'))
            p2 = h._packet(TCP, th(c2, b'two'))
            c1.tx_seq += 3
            c2.tx_seq += 3
        h.ep.send(p1 + p2)
        check(h.recv(c1, 3) == b'one' and h.recv(c2, 3) == b'two', 'coalesced packets')
        with h.cv:
            p3 = h._packet(TCP, th(c1, b'split-packet'))
            c1.tx_seq += 12
        h.ep.send(p3[:10])
        time.sleep(0.05)
        h.ep.send(p3[10:])
        check(h.recv(c1, 12) == b'split-packet', 'split packet')
        h.close(c1)
        h.close(c2)
    test('coalesced and split OUT transfers', coalesced)

    def unknown_conn():
        c = Conn(h, 4000, echo_port, 131072)
        c.tx_seq = c.tx_ack = 1
        h.tcp(c, ACK, b'data for nobody')
        with h.cv:
            ok = h.cv.wait_for(lambda: any(o[0] == 4000 and o[2] & RST for o in h.orphans), 3)
        check(ok, 'no RST for an unknown connection')
    test('data for an unknown connection -> RST', unknown_conn)

    def bus_reset():
        _, st = echo_srv
        c = h.connect(echo_port)
        h.send(c, b'x')
        check(h.recv(c, 1) == b'x', 'echo before reset')
        before = st['closed']
        h.ep.signal(RESET)
        deadline = time.time() + 3
        while st['closed'] == before and time.time() < deadline:
            time.sleep(0.02)
        check(st['closed'] > before, 'connections not dropped on ECONNRESET')
        with h.cv:
            h.conns.clear()
            h.version = 0
            h.got_version = None
        h.handshake()
        lockdown_query(h)
    test('bus reset drops everything, renegotiation works', bus_reset)

    def host_restart():
        # usbmuxd restarted without a bus reset: VERSION again on the live link
        c = h.connect(echo_port)
        with h.cv:
            h.conns.clear()
            h.version = 0
            h.got_version = None
        h.handshake()
        lockdown_query(h)
    test('VERSION again (usbmuxd restart) resets state', host_restart)

    test('no protocol violations seen by the host', lambda: check(not h.errors, '; '.join(h.errors[:5])))
    bad = [r for r in results if r[1]]
    print('mux: %d/%d passed' % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == '__main__':
    # standalone: the daemon must already run with --test-sock SOCKDIR --map 62078=127.0.0.1:PLISTPORT
    sys.exit(main(sys.argv[1], echo_server(), closer_server()))
