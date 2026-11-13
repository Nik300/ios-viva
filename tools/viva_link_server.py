"""Link server for viva-link.py: one process owns the phone's USB link, every other tool talks to it.

Only one process can claim the link interface. Without a server, a long `log`, the interactive interface or a
`watch` kept every one-shot command out (and two agents or the user plus an agent could not share the phone).
Now `viva-link.py serve` owns the link and listens on a Unix socket (~/.viva-link/link.sock, $VIVA_LINK_SOCK);
every mode connects to it and starts one in the background when there is none (VIVA_LINK_DIRECT=1: old direct
USB behaviour). The server also keeps a journal of everything with timestamps, one file per day:

  ~/.viva-link/journal/YYYY-MM-DD.log     HH:MM:SS.mmm KIND text
      L kernel console line     O vivad output line     E phone event (vevents)
      C command and reply       S link state / server notes

Phone events come from tools/vivad/vevents.c: the server starts it through vivad (`run vevents`) while a client
is attached and turns its "EV ..." lines into E events (app, touch, key, lock, display, screenshot, crash).
`viva-link.py watch` prints them as they happen and can wait for one (--until), so a test can wait for the user
to do something on the phone instead of asking for a "done".

Heartbeat ("Viva Link active", patch 0182): sent while at least one client is connected; when the last one
leaves the phone drops "active" 4 s later (the interactive interface still says bye when it quits).

Socket protocol: one JSON object per line both ways. Requests carry an "id"; the answer is {"t": "res", "id"}.
  {"op": "hello", "name": N}                   -> res with the server version and state
  {"op": "cmd", "line": L, "timeout": S}       -> res {"text"}
  {"op": "push", "local", "path", "mode"}      -> notes, then res {"code", "msg"} (the server reads the local file)
  {"op": "pull", "path", "local"}              -> notes, then res {"code", "msg"} (the server writes the local file)
  {"op": "bye"}, {"op": "shutdown"}
Pushed to every client: {"t": "L"|"O", "d": raw text}, {"t": "E", "d": event}, {"t": "S", ...state},
{"t": "note", "msg", "end", "err"} (progress of the client's own push/pull).
Raw bytes travel as UTF-8 with surrogateescape (JSON keeps the lone surrogates, so the bytes come back exact).
"""
import collections, json, os, re, socket, subprocess, sys, threading, time

HOME = os.path.expanduser(os.environ.get('VIVA_LINK_HOME') or
                          ('~/.viva-link-fake' if os.environ.get('VIVA_LINK_FAKE') else '~/.viva-link'))
SOCK = os.environ.get('VIVA_LINK_SOCK') or os.path.join(HOME, 'link.sock')
JOURNAL = os.path.join(HOME, 'journal')
EV_LINE = re.compile(r'^\[(\w+)\] EV (.*)$')
HERE = os.path.dirname(os.path.abspath(__file__))


def version():
    """Changes whenever the tool's code does: an older server is replaced by the next client."""
    return '%d' % max(os.stat(os.path.join(HERE, f)).st_mtime_ns // 1000000
                      for f in ('viva-link.py', 'viva_link_server.py'))


def enc(b):
    return b.decode('utf-8', 'surrogateescape')


def dec(s):
    return s.encode('utf-8', 'surrogateescape')


def stamp(t=None):
    t = time.time() if t is None else t
    return time.strftime('%H:%M:%S', time.localtime(t)) + '.%03d' % (int(t * 1000) % 1000)


class Journal:
    def __init__(self):
        os.makedirs(JOURNAL, exist_ok=True)
        self.lock = threading.Lock()
        self.day = self.f = None

    def path(self):
        return os.path.join(JOURNAL, time.strftime('%Y-%m-%d') + '.log')

    def write(self, kind, text, t=None):
        with self.lock:
            day = time.strftime('%Y-%m-%d')
            if day != self.day:
                if self.f:
                    self.f.close()
                self.f, self.day = open(self.path(), 'a', encoding='utf-8', errors='replace'), day
            for line in text.split('\n'):
                self.f.write('%s %s %s\n' % (stamp(t), kind, line))
            self.f.flush()


class Lines:
    """Splits a byte stream into lines; a partial line is let out after `hold` seconds."""
    def __init__(self, hold=1.0):
        self.carry, self.t, self.hold = b'', 0.0, hold

    def feed(self, data):
        if not self.carry:
            self.t = time.time()
        lines = (self.carry + data).split(b'\n')
        self.carry = lines.pop()
        return lines

    def flush(self, force=False):
        if self.carry and (force or time.time() - self.t > self.hold):
            line, self.carry = self.carry, b''
            return [line]
        return []


class Client:
    def __init__(self, sock):
        self.sock = sock
        self.name = '?'
        self.lock = threading.Lock()
        self.alive = True

    def send(self, obj):
        data = (json.dumps(obj) + '\n').encode()
        with self.lock:
            if not self.alive:
                return
            try:
                self.sock.sendall(data)
            except OSError:
                self.alive = False


class Server:
    def __init__(self, vl):
        self.vl = vl
        self.version = version()
        self.journal = Journal()
        self.clients = set()
        self.clients_lock = threading.Lock()
        self.txn = threading.RLock()          # one phone transaction at a time (command, push, pull)
        self.tls = threading.local()
        self.log_lines, self.out_lines = Lines(), Lines()
        self.vevents_at = None                 # (attaches) when vevents was last started
        self.vevents_ok = True
        link = self.link = vl.Link(None, show_log=False, reconnect=True)
        link.on_log = self.on_log
        link.on_output = self.on_output
        link.note = self.note
        link.want_beat = lambda: bool(self.clients)

    # phone -> clients
    def broadcast(self, obj):
        with self.clients_lock:
            clients = list(self.clients)
        for c in clients:
            c.send(obj)

    def on_log(self, data):
        self.broadcast({'t': 'L', 'd': enc(data)})
        for line in self.log_lines.feed(data):
            self.journal.write('L', enc(line))

    def on_output(self, data):
        self.broadcast({'t': 'O', 'd': enc(data)})
        for line in self.out_lines.feed(data):
            self.output_line(enc(line))

    def output_line(self, line):
        m = EV_LINE.match(line)
        if not m:
            self.journal.write('O', line)
            return
        ev = m.group(2)
        self.journal.write('E', ev)
        self.broadcast({'t': 'E', 'd': ev, 'ts': time.time()})
        if ev.startswith('bye host gone'):
            self.vevents_at = None             # started again with the next client

    def note(self, msg, end='\n', err=True):
        c = getattr(self.tls, 'client', None)
        if c is not None:
            c.send({'t': 'note', 'msg': msg, 'end': end, 'err': err})
        else:
            self.journal.write('S', msg.strip('\r'))

    def state(self):
        link, port = self.link, self.link.port
        return {'t': 'S', 'state': link.state, 'info': link.info, 'heartbeat': link.heartbeat,
                'last_beat': link.last_beat, 'attaches': link.attaches, 'log_bytes': link.log_bytes,
                'persona': port.persona if port else None, 'ifn': port.ifn if port else None,
                'journal': self.journal.path(), 'clients': len(self.clients)}

    def watcher(self):
        """State changes to clients and the journal; partial lines out; vevents started when needed."""
        seen = None
        while True:
            time.sleep(0.25)
            link = self.link
            for line in self.log_lines.flush():
                self.journal.write('L', enc(line))
            for line in self.out_lines.flush():
                self.output_line(enc(line))
            now = (link.state, link.attaches, link.heartbeat)
            if now != seen:
                if seen is None or now[:2] != seen[:2]:
                    self.journal.write('S', 'link %s%s' % (link.state, (' (%s, persona %s)' % (
                        link.info, link.port.persona)) if link.state == 'connected' and link.port else ''))
                seen = now
                self.broadcast(self.state())
            if (link.state == 'connected' and self.clients and self.vevents_ok and
                    self.vevents_at != link.attaches and time.time() - link.connected_at > 1.5):
                self.vevents_at = link.attaches
                threading.Thread(target=self.start_vevents, daemon=True).start()

    def start_vevents(self):
        with self.txn:
            reply = self.link.command('run vevents')
        self.journal.write('S', 'vevents: ' + reply.strip())
        if 'queued' not in reply:
            # old vivad / no vevents on the phone: say it once per attach, keep the link usable
            self.broadcast({'t': 'E', 'd': 'vevents unavailable: ' + reply.strip(), 'ts': time.time()})

    # clients -> phone
    def serve_client(self, c):
        with self.clients_lock:
            self.clients.add(c)
        buf = b''
        try:
            while True:
                data = c.sock.recv(65536)
                if not data:
                    break
                buf += data
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    if line.strip():
                        self.request(c, json.loads(line))
        except (OSError, ValueError):
            pass
        finally:
            c.alive = False
            with self.clients_lock:
                self.clients.discard(c)
            try:
                c.sock.close()
            except OSError:
                pass
            self.journal.write('S', 'client %s left' % c.name)

    def request(self, c, r):
        op, rid = r.get('op'), r.get('id')
        if op == 'hello':
            c.name = r.get('name', '?')
            self.journal.write('S', 'client %s' % c.name)
            c.send(dict(self.state(), t='res', id=rid, version=self.version, pid=os.getpid()))
        elif op == 'cmd':
            threading.Thread(target=self.do_cmd, args=(c, rid, r), daemon=True).start()
        elif op in ('push', 'pull'):
            threading.Thread(target=self.do_file, args=(c, rid, op, r), daemon=True).start()
        elif op == 'bye':
            with self.clients_lock:
                others = len(self.clients) - 1
            if others <= 0:
                self.bye()
            c.send({'t': 'res', 'id': rid})
        elif op == 'shutdown':
            self.journal.write('S', 'server shut down by %s' % c.name)
            c.send({'t': 'res', 'id': rid})
            self.bye()
            try:
                os.unlink(SOCK)
            except OSError:
                pass
            os._exit(0)

    def bye(self):
        port = self.link.port
        if port:
            try:
                port.dev.ctrl_transfer(0x41, 0x56, 0, port.ifn, None, timeout=300)
            except Exception:
                pass

    def do_cmd(self, c, rid, r):
        line = r.get('line', '')
        with self.txn:
            text = self.link.command(line, r.get('timeout', 10))
        if line.split(' ', 1)[0] not in ('ping', 'rewind'):
            self.journal.write('C', '> %s (%s)\n%s' % (line, c.name, text.rstrip('\n')))
        c.send({'t': 'res', 'id': rid, 'text': text})

    def do_file(self, c, rid, op, r):
        code, msg = 0, ''
        with self.txn:
            self.tls.client = c
            try:
                if op == 'push':
                    self.vl.push(self.link, r['local'], r['path'], r.get('mode', 0o644))
                else:
                    self.vl.pull(self.link, r['path'], r['local'])
            except SystemExit as e:
                code = e.code if isinstance(e.code, int) else 1
                msg = e.code if isinstance(e.code, str) else ''
            except Exception as e:
                code, msg = 1, '%s: %s' % (op, e)
            finally:
                self.tls.client = None
        self.journal.write('C', '> %s %s (%s): %s' % (op, r.get('path'), c.name, msg or ('ok' if not code else code)))
        c.send({'t': 'res', 'id': rid, 'code': code, 'msg': msg})

    def run(self):
        os.makedirs(HOME, exist_ok=True)
        if alive():
            sys.exit('a link server is already running (%s)' % SOCK)
        try:
            os.unlink(SOCK)
        except OSError:
            pass
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(SOCK)
        os.chmod(SOCK, 0o600)
        srv.listen(16)
        self.journal.write('S', 'server %d started (version %s)' % (os.getpid(), self.version))
        threading.Thread(target=self.link.pump, daemon=True).start()
        threading.Thread(target=self.watcher, daemon=True).start()
        while True:
            s, _ = srv.accept()
            threading.Thread(target=self.serve_client, args=(Client(s),), daemon=True).start()


def alive():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(SOCK)
        return True
    except OSError:
        return False
    finally:
        s.close()


def spawn(script):
    """Start `viva-link.py serve` in the background (own session: it outlives the client)."""
    os.makedirs(HOME, exist_ok=True)
    log = open(os.path.join(HOME, 'server.log'), 'ab')
    subprocess.Popen([sys.executable, script, 'serve'], stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                     start_new_session=True, close_fds=True)
    for _ in range(100):
        if alive():
            return True
        time.sleep(0.05)
    return False


Port = collections.namedtuple('Port', 'persona ifn')


class RemoteLink:
    """viva-link.py's Link, through the server: same attributes for the modes and the interface."""
    remote = True

    def __init__(self, script, name, show_log=True, reconnect=False):
        self.script, self.name = script, name
        self.show_log, self.reconnect = show_log, reconnect
        self.port = None
        self.state, self.info, self.heartbeat, self.last_beat = 'waiting', '', None, 0.0
        self.attaches = self.log_bytes = 0
        self.logfile = None
        self.log = None                         # the server journals commands itself
        self.on_log = self.on_output = self.on_event = None
        self.lock = threading.Lock()
        self.reply_done = threading.Event()
        self.reply = bytearray()
        self.output = bytearray()
        self.output_event = threading.Event()
        self.echo_output = True
        self.sock = None
        self.ids = 0
        self.waiting = {}                       # id -> [Event, result]
        self.send_lock = threading.Lock()
        self.connected = threading.Event()
        self.server_version = None

    def note(self, msg, end='\n', err=True):
        print(msg, end=end, file=sys.stderr if err else sys.stdout, flush=True)

    # server connection
    def open(self):
        """Connect to the server (started if needed, replaced if older than this code); returns hello's answer."""
        for attempt in range(3):
            if not alive() and not spawn(self.script):
                raise SystemExit('cannot start the link server (see %s)' % os.path.join(HOME, 'server.log'))
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(SOCK)
            except OSError:
                time.sleep(0.2)
                continue
            self.sock, self.buf = s, b''
            hello = self.request_raw({'op': 'hello', 'name': self.name})
            if hello.get('version') == version() or os.environ.get('VIVA_LINK_ANY_SERVER'):
                self.server_version = hello.get('version')
                self.apply_state(hello)
                return hello
            # an older server: replace it (its other clients reconnect by themselves)
            self.request_raw({'op': 'shutdown'})
            s.close()
            for _ in range(50):
                if not alive():
                    break
                time.sleep(0.05)
        raise SystemExit('cannot reach the link server at %s' % SOCK)

    def request_raw(self, obj):
        """Before the reader thread runs: send and read lines until the answer (other messages applied)."""
        self.ids += 1
        obj['id'] = self.ids
        self.sock.sendall((json.dumps(obj) + '\n').encode())
        while True:
            while b'\n' not in self.buf:
                data = self.sock.recv(65536)
                if not data:
                    return {}
                self.buf += data
            line, self.buf = self.buf.split(b'\n', 1)
            m = json.loads(line)
            if m.get('t') == 'res' and m.get('id') == obj['id']:
                return m
            self.dispatch(m)

    def request(self, obj, timeout=None):
        with self.send_lock:
            self.ids += 1
            rid = obj['id'] = self.ids
            slot = self.waiting[rid] = [threading.Event(), {}]
            try:
                self.sock.sendall((json.dumps(obj) + '\n').encode())
            except (OSError, AttributeError):
                del self.waiting[rid]
                return None
        if not slot[0].wait(timeout):
            self.waiting.pop(rid, None)
            return None
        return slot[1]

    def apply_state(self, m):
        self.state = m.get('state', self.state)
        self.info = m.get('info', self.info)
        self.heartbeat = m.get('heartbeat')
        self.last_beat = m.get('last_beat', 0.0)
        self.attaches = m.get('attaches', self.attaches)
        self.logfile = m.get('journal', self.logfile)
        self.port = Port(m.get('persona'), m.get('ifn')) if self.state == 'connected' else None
        if self.state == 'connected':
            self.connected.set()
        else:
            self.connected.clear()
            self.reply_done.set()               # like Link.lost: wake a waiting command
            self.output_event.set()

    def dispatch(self, m):
        t = m.get('t')
        if t == 'L':
            data = dec(m['d'])
            self.log_bytes += len(data)
            if self.log:
                self.log.write(data)
                self.log.flush()
            if self.on_log:
                self.on_log(data)
            elif self.show_log:
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
        elif t == 'O':
            data = dec(m['d'])
            if self.echo_output:
                if self.on_output:
                    self.on_output(data)
                elif self.show_log:
                    sys.stdout.buffer.write(data)
                    sys.stdout.flush()
            with self.lock:
                self.output += data
            self.output_event.set()
        elif t == 'E':
            if self.on_event:
                self.on_event(m)
        elif t == 'S':
            self.apply_state(m)
        elif t == 'note':
            self.note(m['msg'], end=m.get('end', '\n'), err=m.get('err', True))
        elif t == 'res':
            slot = self.waiting.pop(m.get('id'), None)
            if slot:
                slot[1] = m
                slot[0].set()

    def reader(self):
        buf = self.buf
        while True:
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                if line.strip():
                    self.dispatch(json.loads(line))
            try:
                data = self.sock.recv(65536)
            except OSError:
                data = b''
            if not data:
                return
            buf += data

    # Link's interface
    def connect(self):
        """Attach to the server and wait until it has the phone (like Link.connect)."""
        if self.sock is None:
            self.open()
            threading.Thread(target=self.serve_forever, daemon=True).start()
        if not self.connected.is_set():
            self.note('waiting for the phone (link server %s)...' % SOCK)
        self.connected.wait()
        self.note('connected: %s (persona %s, interface %s, via the link server)' % (
            self.info, self.port.persona if self.port else '?', self.port.ifn if self.port else '?'))

    def serve_forever(self):
        while True:
            self.reader()
            # server gone (replaced, killed): fail what is waiting, then reconnect or leave
            for slot in list(self.waiting.values()):
                slot[0].set()
            self.waiting.clear()
            self.state, self.port = 'lost', None
            self.connected.clear()
            self.reply_done.set()
            self.output_event.set()
            if not self.reconnect:
                print('\nlink server gone', file=sys.stderr)
                os._exit(1)
            while True:
                time.sleep(0.5)
                try:
                    self.sock.close()
                    self.sock = None
                    self.open()
                    break
                except SystemExit:
                    continue

    def pump(self):
        """For the interface's thread: everything happens in serve_forever."""
        if self.sock is None:
            self.open()
        self.serve_forever()

    def command(self, line, timeout=10):
        m = self.request({'op': 'cmd', 'line': line[:511], 'timeout': timeout}, timeout + 5)
        if m is None:
            return '(no reply in %d s)\n' % timeout if self.port else '(link lost)\n'
        text = m.get('text', '(link lost)\n')
        with self.lock:
            self.reply = bytearray(text.encode())
        return text

    def file_op(self, op, **kw):
        m = self.request(dict(op=op, **kw))
        if m is None:
            raise SystemExit('%s: link server gone' % op)
        if m.get('code'):
            raise SystemExit(m.get('msg') or m['code'])

    def bye(self):
        if self.sock is not None:
            self.request({'op': 'bye'}, 2)

    def write(self, data, timeout=2000):
        raise RuntimeError('raw writes go through the server (push)')
