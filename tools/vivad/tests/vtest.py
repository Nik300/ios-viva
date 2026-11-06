"""Shared pieces of the vivausbd tests: the framed test-socket endpoint (vivausbd --test-sock)."""
import socket
import threading
import struct
import time

RESET = 1
CANCEL = 2


class Endpoint:
    """Host side of one USB function: each frame is one bulk transfer (u32 LE length + bytes)."""

    def __init__(self, path, timeout=10.0):
        deadline = time.time() + 5
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                self.s.close()
                if time.time() > deadline:
                    raise
                time.sleep(0.05)
        self.s.settimeout(timeout)
        self.wlock = threading.Lock()

    def _recvn(self, n):
        b = bytearray()
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise EOFError('device endpoint closed')
            b += c
        return bytes(b)

    def send(self, data):
        """One OUT transfer."""
        with self.wlock:
            self.s.sendall(struct.pack('<I', len(data)) + data)

    def signal(self, code):
        """Bus reset (RESET) or PTP class cancel (CANCEL) as seen by read() on the node."""
        with self.wlock:
            self.s.sendall(struct.pack('<II', 0xffffffff, code))

    def recv(self):
        """One IN transfer (one device write())."""
        (n,) = struct.unpack('<I', self._recvn(4))
        return self._recvn(n)

    def close(self):
        self.s.close()


class Fail(Exception):
    pass


def check(cond, what):
    if not cond:
        raise Fail(what)
