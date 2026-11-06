"""Full-screen interface of viva-link.py (Textual): command prompt, command output pane, phone log pane.

viva-link.py imports it for the interactive mode and calls run(link, module); everything that touches USB
stays in viva-link.py. The pump thread hands log/output bytes over through deques, a 20 Hz timer moves them
into the panes, so bursts of log never block the prompt.
"""
import bisect, glob, os, queue, re, shlex, subprocess, threading, time
from collections import deque

from rich.style import Style
from rich.text import Text
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal
from textual.geometry import Size
from textual.scroll_view import ScrollView
from textual.strip import Strip
from textual.suggester import Suggester
from textual.widgets import Footer, Input, Label

HISTORY = os.path.expanduser('~/.viva-link-history')
LOG_LINES, OUT_LINES = 50_000, 20_000
LOCAL = ('help', 'quit', 'clear', 'save', 'push', 'pull', 'run', 'filter', 'layout')
SPIN = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'

# Tokyo Night
BG, BG_DARK, SURFACE, HIGHLIGHT, BORDER = '#1a1b26', '#16161e', '#1f2335', '#292e42', '#3b4261'
COMMENT, FG, FG_DIM = '#565f89', '#c0caf5', '#a9b1d6'
BLUE, CYAN, MAGENTA, GREEN, YELLOW, ORANGE, RED, TEAL = ('#7aa2f7', '#7dcfff', '#bb9af7', '#9ece6a',
                                                         '#e0af68', '#ff9e64', '#f7768e', '#73daca')

PANIC = re.compile(r'panic|Debugger message|Kernel trap|\bfatal\b', re.I)
ERROR = re.compile(r'\berror\b|\bfail(ed|ure|s)?\b|deny\(|abnormal|timed? ?out|\bBAD\b|\bcrash', re.I)
WARN = re.compile(r'\bwarn(ing)?\b|\bretry\b|\bslow\b', re.I)
PREFIX = re.compile(r'^[\w.\-\[\]()]{2,40}?:\s')
HEX = re.compile(r'\b0x[0-9a-fA-F]+\b')
ANSI = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]|[\x00-\x08\x0b-\x1f\x7f]')


def clean(line):
    return ANSI.sub('', line.replace('\r', '').expandtabs(4))


def style_log(text, tag):
    """A console line: whole-line colour by severity, subsystem prefix and hex numbers accented."""
    if tag == 'mark':
        return Text(text, style='bold %s' % (GREEN if '●' in text else RED if '✕' in text else YELLOW))
    if 'VIVA LINK:' in text:
        return Text(text, style='bold ' + MAGENTA)
    if PANIC.search(text):
        return Text(text, style='bold %s on #3b1d2a' % RED)
    t = Text(text, style=RED if ERROR.search(text) else YELLOW if WARN.search(text) else FG_DIM)
    if t.style == FG_DIM:
        m = PREFIX.match(text)
        if m:
            t.stylize(BLUE, 0, m.end() - 1)
        for m in HEX.finditer(text):
            t.stylize(MAGENTA, m.start(), m.end())
    return t


OUT_STYLE = {'cmd': 'bold ' + FG, 'reply': FG, 'out': TEAL, 'ok': GREEN, 'err': RED, 'info': 'italic ' + COMMENT,
             'progress': YELLOW, 'mark': 'bold ' + YELLOW}
GUTTER = {'cmd': ('❯ ', 'bold ' + BLUE), 'reply': ('│ ', BORDER), 'out': ('┃ ', '#2e5b57'), 'ok': ('╰ ', BORDER),
          'err': ('╰ ', RED), 'info': ('· ', COMMENT), 'progress': ('╰ ', YELLOW), 'mark': ('» ', YELLOW)}


def style_out(text, tag):
    if tag == 'mark':
        return style_log(text, tag)
    t = Text(text, style=OUT_STYLE.get(tag, FG))
    if tag == 'out':                        # vivad's "[id] " prefix
        m = re.match(r'\[\d+\] ', text)
        if m:
            t.stylize(COMMENT, 0, m.end())
    return t


class Pane(ScrollView, can_focus=False):
    """Bounded, wrapped, filterable line view that follows the end until scrolled back."""
    DEFAULT_CSS = 'Pane { overflow-x: hidden; overflow-y: auto; scrollbar-gutter: stable; }'

    def __init__(self, styler, max_lines, gut=0, **kw):
        super().__init__(**kw)
        self.styler, self.max_lines, self.gut = styler, max_lines, gut
        self.lines = []             # (text, tag, right): right = dim text at the right edge of the first row
        self.base = 0               # absolute index of lines[0] (older lines are trimmed)
        self.rows = []              # (absolute line index, start, end) of the shown lines, wrapped
        self.width_ = 0
        self.filter = ''            # lower case
        self.follow = True
        self.unseen = 0             # lines arrived while scrolled back
        self.cache = {}
        self._auto = False          # our own scrolling: not the user leaving the end

    # content
    def fits(self, text):
        return not self.filter or self.filter in text.lower()

    def wrap(self, i, text):
        w = max(8, self.width_ - self.gut)
        end = min(len(text), w)
        rows = [(i, 0, end)]
        while end < len(text):      # continuation rows are indented by 2
            rows.append((i, end, min(len(text), end + w - 2)))
            end = rows[-1][2]
        return rows

    def append(self, items):
        rows = []
        for text, tag, right in items:
            i = self.base + len(self.lines)
            self.lines.append((text, tag, right))
            if self.fits(text):
                rows += self.wrap(i, text)
        self.rows += rows
        if not self.follow and rows:
            self.unseen += len(items)
        self.sync(self.trim())

    def replace_last(self, text, tag, right=''):
        if not self.lines:
            return self.append([(text, tag, right)])
        i = self.base + len(self.lines) - 1
        self.lines[-1] = (text, tag, right)
        del self.rows[bisect.bisect_left(self.rows, (i,)):]
        if self.fits(text):
            self.rows += self.wrap(i, text)
        self.cache.clear()
        self.sync()

    def last_tag(self):
        return self.lines[-1][1] if self.lines else None

    def trim(self):
        excess = len(self.lines) - self.max_lines
        if excess < max(1, self.max_lines // 20):       # in chunks: list deletes are O(n)
            return 0
        del self.lines[:excess]
        self.base += excess
        k = bisect.bisect_left(self.rows, (self.base,))
        del self.rows[:k]
        self.cache.clear()
        return k

    def clear(self):
        self.base += len(self.lines)
        self.lines, self.rows = [], []
        self.cache.clear()
        self.follow, self.unseen = True, 0
        self.sync()

    def reflow(self):
        """Rebuild the rows (width or filter changed), keeping the top line when scrolled back."""
        y = int(self.scroll_y)
        top = self.rows[y][0] if not self.follow and y < len(self.rows) else None
        self.rows = [r for k, (text, _, _) in enumerate(self.lines) if self.fits(text)
                     for r in self.wrap(self.base + k, text)]
        self.cache.clear()
        self.sync()
        if top is not None:
            self._auto = True
            self.scroll_to(y=bisect.bisect_left(self.rows, (top,)), animate=False, immediate=True)
            self._auto = False

    def set_filter(self, text):
        self.filter = text.lower()
        self.reflow()

    def plain(self):
        out = []
        for text, tag, right in self.lines:
            out.append(('\n> %s   [%s]' % (text, right)) if tag == 'cmd' else text)
        return '\n'.join(out) + '\n'

    # scrolling
    def sync(self, shift=0):
        self._auto = True
        self.virtual_size = Size(self.width_, len(self.rows))
        if self.follow:
            self.scroll_end(animate=False, immediate=True, x_axis=False)
        elif shift:                 # trimmed above a scrolled-back view: stay on the same lines
            self.scroll_to(y=max(0, self.scroll_y - shift), animate=False, immediate=True)
        self._auto = False
        self.refresh()

    def watch_scroll_y(self, old, new):
        super().watch_scroll_y(old, new)
        if self._auto:
            return
        if new < old and new < self.max_scroll_y:
            self.follow = False
        elif new >= self.max_scroll_y:
            self.follow, self.unseen = True, 0

    def resume(self):
        self.follow, self.unseen = True, 0
        self.sync()

    def on_resize(self, event):
        w = self.scrollable_content_region.width
        if w != self.width_:
            self.width_ = w
            self.reflow()

    # rendering
    def styled(self, i):
        t = self.cache.get(i)
        if t is None:
            text, tag, _ = self.lines[i - self.base]
            t = self.cache[i] = self.styler(text, tag)
            if len(self.cache) > 4000:
                self.cache.clear()
        return t

    def render_line(self, y):
        r = int(self.scroll_y) + y
        width = self.scrollable_content_region.width
        if r >= len(self.rows):
            return Strip.blank(width, self.rich_style)
        i, s, e = self.rows[r]
        text, tag, right = self.lines[i - self.base]
        seg = self.styled(i)[s:e]
        line = Text(style=self.rich_style)
        if self.gut:
            g, gs = GUTTER.get(tag, ('  ', ''))
            line.append(g if s == 0 or tag != 'cmd' else '  ', gs)
        if s:
            line.append('↳ ', COMMENT)
        at = len(line)
        line.append_text(seg)
        if self.filter:
            low, f = text[s:e].lower(), self.filter
            k = low.find(f)
            while k >= 0:
                line.stylize('bold %s on %s' % (BG_DARK, YELLOW), at + k, at + k + len(f))
                k = low.find(f, k + 1)
        if right and s == 0 and line.cell_len + len(right) + 2 <= width:
            line.append(' ' * (width - line.cell_len - len(right) - 1))
            line.append(right, COMMENT)
        sel = self.text_selection
        if sel is not None:
            span = sel.get_span(r)
            if span:
                a, b = span
                line.stylize(self.screen.get_component_rich_style('screen--selection'),
                             a, len(line) if b == -1 else b)
        if tag == 'cmd':
            line.stylize('on ' + HIGHLIGHT)
        strip = Strip(list(line.render(self.app.console)), line.cell_len)
        style = self.rich_style + Style(bgcolor=HIGHLIGHT) if tag == 'cmd' else self.rich_style
        return strip.crop_extend(0, width, style).apply_offsets(0, r)

    def get_selection(self, selection):
        rows = []
        for i, s, e in self.rows:
            text, tag, _ = self.lines[i - self.base]
            rows.append(' ' * self.gut + ('  ' if s else '') + text[s:e])
        picked = selection.extract('\n'.join(rows)).split('\n')
        return '\n'.join(l[self.gut:] if l[:self.gut].isspace() else l for l in picked), '\n'

    def selection_updated(self, selection):
        self.refresh()

    def on_click(self, event):
        self.app.select_pane(self)


class HistorySuggester(Suggester):
    """Ghost text from the history (newest first), then from the command names. Right arrow accepts it."""
    def __init__(self, app):
        super().__init__(use_cache=False, case_sensitive=True)
        self.app_ = app

    async def get_suggestion(self, value):
        for h in reversed(self.app_.history):
            if h.startswith(value) and h != value:
                return h
        names = [value[0] + n for n in LOCAL] if value[:1] in ':/' else self.app_.vl.PHONE_COMMANDS
        return next((n for n in names if n.startswith(value) and n != value), None)


class CmdInput(Input):
    BINDINGS = [Binding('up', 'history(-1)', show=False), Binding('down', 'history(1)', show=False),
                Binding('tab', 'complete', show=False)]

    def __init__(self, **kw):
        super().__init__(**kw)
        self.hpos = None
        self.completed = None       # value set by Tab: keeps the list of candidates shown

    def action_history(self, step):
        """Up/Down: older/newer lines starting with what was typed."""
        h = self.app.history
        if self.hpos is None:
            self.draft, self.hpos = self.value, len(h)
        k = self.hpos + step
        while 0 <= k < len(h) and not h[k].startswith(self.draft):
            k += step
        if k < 0:
            return
        self.hpos = min(k, len(h))
        self.value = h[k] if k < len(h) else self.draft
        self.cursor_position = len(self.value)

    def action_complete(self):
        """Tab: command names (phone, or local after ':'), file names for the local paths."""
        v, rest = self.value[:self.cursor_position], self.value[self.cursor_position:]
        word = v.rsplit(' ', 1)[-1]
        head = v[:len(v) - len(word)]
        if not head.strip():
            if word[:1] in ':/':
                cands = [word[0] + n for n in LOCAL if n.startswith(word[1:])]
            else:
                cands = [n for n in self.app.vl.PHONE_COMMANDS if n.startswith(word)]
        else:
            cands = [p + ('/' if os.path.isdir(p) else '') for p in glob.glob(os.path.expanduser(word) + '*')]
            if word.startswith('~'):
                home = os.path.expanduser('~')
                cands = ['~' + c[len(home):] for c in cands]
        cands.sort()
        if not cands:
            self.app.bell()
            return
        common = os.path.commonprefix(cands)
        if len(cands) == 1 and not common.endswith('/'):
            common += ' '
        self.value = self.completed = head + common + rest
        self.cursor_position = len(head + common)
        self.app.hint(('  '.join(os.path.basename(c.rstrip('/')) or c for c in cands[:12])
                       + ('  …' if len(cands) > 12 else '')) if len(cands) > 1 else '')

    def reset(self):
        self.hpos = None


class VivaLinkApp(App):
    TITLE = 'viva-link'
    CSS = """
    Screen { background: %(BG)s; }
    #status { height: 1; background: %(BG_DARK)s; }
    #status Label { padding: 0 1; height: 1; }
    #brand { background: %(BLUE)s; color: %(BG_DARK)s; text-style: bold; }
    #conn.waiting { color: %(YELLOW)s; } #conn.connected { color: %(GREEN)s; text-style: bold; }
    #conn.lost { background: #3b1d2a; color: %(RED)s; text-style: bold; }
    #persona { color: %(CYAN)s; } #speed { color: %(FG_DIM)s; }
    #hb.ok { color: %(MAGENTA)s; } #hb.unsupported { color: %(YELLOW)s; } #hb.off { color: %(COMMENT)s; }
    #spacer { width: 1fr; }
    #bytes { color: %(FG_DIM)s; } #file { color: %(TEAL)s; }
    #panes { height: 1fr; layout: horizontal; padding: 0 0; }
    #panes.stack { layout: vertical; }
    Pane { border: round %(BORDER)s; border-title-align: left; border-subtitle-align: right;
           background: %(BG)s; scrollbar-color: %(BORDER)s; scrollbar-color-hover: %(COMMENT)s;
           scrollbar-background: %(BG)s; scrollbar-background-hover: %(BG)s; scrollbar-size-vertical: 1; }
    #out { width: 2fr; border-title-color: %(BLUE)s; }
    #log { width: 3fr; border-title-color: %(MAGENTA)s; }
    #panes.stack #out { width: 1fr; height: 2fr; } #panes.stack #log { width: 1fr; height: 3fr; }
    #out.selected { border: round %(BLUE)s; } #log.selected { border: round %(MAGENTA)s; }
    Pane.hidden, #filterbar.hidden { display: none; }
    #filterbar { height: 3; }
    #filter { border: round %(YELLOW)s; background: %(BG)s; border-title-color: %(YELLOW)s; }
    #cmd { border: round %(BORDER)s; background: %(SURFACE)s; border-title-color: %(BLUE)s;
           border-subtitle-color: %(COMMENT)s; border-title-style: bold; }
    #cmd:focus { border: round %(BLUE)s; }
    #cmd > .input--suggestion { color: %(COMMENT)s; }
    #cmd > .input--placeholder { color: %(COMMENT)s; }
    Footer { background: %(BG_DARK)s; }
    """ % globals()

    BINDINGS = [
        Binding('ctrl+c', 'ctrl_c', 'esci', show=False, priority=True),
        Binding('ctrl+d', 'quit_link', 'esci', show=False, priority=True),
        Binding('ctrl+q', 'quit_link', 'esci', priority=True),
        Binding('f2', 'layout', 'layout', priority=True),
        Binding('f3', 'maximize_pane', 'massimizza', priority=True),
        Binding('shift+tab', 'switch_pane', 'pannello', key_display='⇧⇥', priority=True),
        Binding('ctrl+f', 'filter', 'filtro log', key_display='^F', priority=True),
        Binding('pageup', 'scroll_pane(-1)', 'scorri', key_display='PgUp/PgDn', priority=True),
        Binding('pagedown', 'scroll_pane(1)', show=False, priority=True),
        Binding('shift+up', 'scroll_line(-1)', show=False, priority=True),
        Binding('shift+down', 'scroll_line(1)', show=False, priority=True),
        Binding('end', 'end', 'segui', key_display='End', priority=True),
        Binding('escape', 'escape', show=False),
        Binding('ctrl+l', 'clear_pane', 'pulisci', key_display='^L', priority=True),
    ]

    def __init__(self, link, vl, log_lines=LOG_LINES):
        super().__init__()
        self.link, self.vl = link, vl
        self.log_lines = log_lines
        self.log_q, self.out_q, self.ops = deque(), deque(), deque()   # filled by other threads
        self.log_carry, self.out_carry, self.carry_t = b'', b'', 0.0
        self.jobs = queue.Queue()
        self.busy = None            # command being run by the worker
        self.progress = None
        self.hint_text = ''
        self.history = []
        self.seen = (None, 0)       # (state, attaches) last shown
        self.persona = ''
        self.layout_ = None
        self.ticks = 0
        try:
            with open(HISTORY, errors='replace') as f:
                self.history = [l.rstrip('\n') for l in f if l.strip()][-1000:]
        except OSError:
            pass

    def compose(self) -> ComposeResult:
        with Horizontal(id='status'):
            yield Label(' ◆ VIVA LINK ', id='brand')
            yield Label('', id='conn')
            yield Label('', id='persona')
            yield Label('', id='speed')
            yield Label('', id='hb')
            yield Label('', id='spacer')
            yield Label('', id='bytes')
            yield Label('', id='file')
        with Horizontal(id='panes'):
            yield Pane(style_out, OUT_LINES, gut=2, id='out')
            yield Pane(style_log, self.log_lines, id='log')
        with Horizontal(id='filterbar', classes='hidden'):
            yield Input(placeholder='testo da cercare nel log (Invio applica, Esc toglie)', id='filter')
        yield CmdInput(placeholder='comando del telefono (help) o :help per i comandi locali',
                       suggester=HistorySuggester(self), id='cmd')
        yield Footer(compact=True, show_command_palette=False)

    def on_mount(self):
        self.theme = 'tokyo-night'
        self.outp, self.logp = self.query_one('#out', Pane), self.query_one('#log', Pane)
        self.outp.border_title, self.logp.border_title = ' Output comandi ', ' Log telefono '
        self.query_one('#filter').border_title = ' filtro log '
        cmd = self.query_one('#cmd', CmdInput)
        cmd.border_title = ' ❯ comando '
        self.selected = self.logp
        self.logp.add_class('selected')
        self.set_layout('side' if self.size.width >= 150 else 'stack')
        self.outp.append([('comandi del telefono: help · comandi locali: :help', 'info', ''),
                          ('PgUp/PgDn scorrono · ⇧⇥ cambia pannello · End segue', 'info', '')])
        link = self.link
        link.on_log = self.log_q.append
        link.on_output = self.out_q.append
        link.note = self.note
        self.pump = threading.Thread(target=link.pump, daemon=True)
        self.pump.start()
        threading.Thread(target=self.worker, daemon=True).start()
        self.set_interval(0.05, self.tick)
        cmd.focus()

    # threads -> UI
    def note(self, msg, end='\n', err=True):
        if threading.current_thread() is self.pump and msg.startswith(('connected', 'link lost', 'waiting')):
            return                  # shown from the link state
        if msg.startswith('\r'):
            self.progress = msg[1:]
            self.ops.append(('progress', self.progress, 'progress'))
        elif self.progress is not None and end == '\n' and msg.startswith(' '):    # push's " (1.2 s)"
            self.ops.append(('progress', self.progress + msg, 'info'))
            self.progress = None
        else:
            self.progress = None
            self.ops.append(('out', [(clean(l), 'reply' if not err else 'info', '') for l in msg.split('\n')]))

    def split(self, carry, chunks, force):
        data = carry + b''.join(chunks)
        lines = data.split(b'\n')
        carry = lines.pop()
        if carry and force:             # a line without its newline for a while: show it anyway
            lines.append(carry)
            carry = b''
        return carry, [clean(l.decode('utf-8', 'replace')) for l in lines]

    def drain(self, q):
        chunks = []
        while q:
            chunks.append(q.popleft())
        return chunks

    def tick(self):
        self.ticks += 1
        chunks = self.drain(self.log_q)
        now = time.time()
        if chunks or self.log_carry:
            if not self.log_carry:
                self.carry_t = now
            self.log_carry, lines = self.split(self.log_carry, chunks, now - self.carry_t > 0.5)
            if lines:
                self.logp.append([(l, '', '') for l in lines])
            if self.log_carry:
                self.carry_t = self.carry_t if not lines else now
        chunks = self.drain(self.out_q)
        if chunks:
            self.out_carry, lines = self.split(self.out_carry, chunks, False)
            if lines:
                self.outp.append([(l, 'out', '') for l in lines])
        while self.ops:
            op = self.ops.popleft()
            if op[0] == 'out':
                self.outp.append(op[1])
            elif op[0] == 'progress':
                if self.outp.last_tag() == 'progress':
                    self.outp.replace_last(op[1], op[2])
                else:
                    self.outp.append([(op[1], op[2], '')])
        self.watch_link()
        if self.ticks % 4 == 0 or chunks:
            self.update_status()

    def watch_link(self):
        link = self.link
        now = (link.state, link.attaches)
        if now == self.seen:
            return
        self.seen = now
        t = time.strftime('%H:%M:%S')
        if link.state == 'connected':
            port = link.port
            self.persona = '%s %s · if%d' % (port.persona, '1209:0002' if port.persona == 'link' else '05ac:12a8',
                                             port.ifn) if port else ''
            mark = '──── ● connesso · persona %s · %s ────' % (port.persona if port else '?', t)
            if link.attaches > 1:
                self.notify('telefono di nuovo connesso (persona %s)' % (port.persona if port else '?'),
                            title='Viva Link', timeout=3)
        elif link.state == 'lost':
            mark = '──── ✕ link perso · %s · in attesa che il telefono torni ────' % t
            self.notify('link perso: riconnessione automatica', title='Viva Link', severity='warning', timeout=3)
        else:
            mark = '──── ◌ in attesa del telefono · %s ────' % t
        self.logp.append([(mark, 'mark', '')])
        self.outp.append([(mark.strip('─ '), 'mark', '')])

    def update_status(self):
        link = self.link
        spin = SPIN[self.ticks // 2 % len(SPIN)]
        conn = self.query_one('#conn', Label)
        conn.set_classes(link.state)
        conn.update({'connected': '● connesso', 'waiting': '%s in attesa del telefono' % spin,
                     'lost': '%s link perso · riconnessione' % spin}[link.state])
        self.query_one('#persona', Label).update(self.persona if link.state == 'connected' else
                                                 ('ultimo: ' + self.persona.split(' ')[0]) if self.persona else '')
        speed = link.info.split(', ')[-1] if link.info and link.state == 'connected' else ''
        self.query_one('#speed').display = self.size.width >= 130     # narrow terminal: the essentials only
        self.query_one('#file').display = self.size.width >= 120
        self.query_one('#speed', Label).update(speed.replace(' speed', '-speed USB'))
        hb = self.query_one('#hb', Label)
        if link.state != 'connected' or link.heartbeat is None:
            hb.set_classes('off')
            hb.update('♡ Viva Link inattivo')
        elif link.heartbeat == 'unsupported':
            hb.set_classes('unsupported')
            hb.update('⚠ heartbeat non supportato')
        else:
            hb.set_classes('ok')
            fresh = time.time() - link.last_beat
            hb.update(('♥' if fresh < 0.35 else '♡') + ' Viva Link attivo' + ('' if fresh < 3 else ' (?)'))
        self.query_one('#bytes', Label).update('log %s · %s righe' % (human(link.log_bytes),
                                                                     '{:,}'.format(self.logp.base + len(self.logp.lines))
                                                                     .replace(',', '.')))
        self.query_one('#file', Label).update(('⤓ ' + os.path.basename(link.logfile)) if link.logfile else '')
        for p in (self.outp, self.logp):
            if p.follow:
                s = '[%s]● live[/]' % GREEN
            else:
                s = '[%s]⏸ fermo[/]' % YELLOW + ('  [b]+%d[/b] nuove' % p.unseen if p.unseen else '') + \
                    '  [%s]End per seguire[/]' % COMMENT
            if p is self.logp and p.filter:
                s = '[%s]filtro «%s» %d righe[/]  ' % (YELLOW, p.filter, len(p.rows)) + s
            p.border_subtitle = ' %s ' % s
        cmd = self.query_one('#cmd', CmdInput)
        waiting = self.jobs.qsize()
        if self.busy:
            cmd.border_subtitle = ' %s %s%s ' % (spin, self.busy[:40], ('  · in coda %d' % waiting) if waiting else '')
        elif not self.hint_text:
            cmd.border_subtitle = ' Tab completa · ↑↓ cronologia · → accetta suggerimento '

    def hint(self, text):
        self.hint_text = text
        if text:
            self.query_one('#cmd', CmdInput).border_subtitle = ' %s ' % text

    # commands
    def on_input_changed(self, event):
        if event.input.id == 'filter':
            self.logp.set_filter(event.value)
        elif self.hint_text and event.value != event.input.completed:
            self.hint('')

    def on_input_submitted(self, event):
        if event.input.id == 'filter':
            if not event.value:
                self.query_one('#filterbar').add_class('hidden')
            self.query_one('#cmd').focus()
            return
        cmd = event.input
        line = event.value.strip()
        cmd.value = ''
        cmd.reset()
        self.hint('')
        if not line:
            return
        if not self.history or self.history[-1] != line:
            self.history.append(line)
            try:
                with open(HISTORY, 'a') as f:
                    f.write(line + '\n')
            except OSError:
                pass
        self.outp.resume()
        words = line.split()
        if line[0] in ':/':
            self.local(line[1:].strip())
        elif words[0] in ('push', 'pull'):      # the raw protocol is not for typing: use the helpers
            self.local(line)
        else:
            self.jobs.put(('cmd', line))

    def local(self, line):
        try:
            words = shlex.split(line)
        except ValueError as e:
            return self.say(line, [(str(e), 'err', '')])
        c, args = (words[0], words[1:]) if words else ('help', [])
        if c in ('q', 'quit', 'esci', 'exit'):
            self.action_quit_link()
        elif c in ('help', 'aiuto', 'h', '?'):
            self.say(':' + line, [(l, 'reply', '') for l in (
                ':push LOCALE PERCORSO [MODO]   copia un file sul telefono (anche senza :)',
                ':pull PERCORSO [LOCALE]        copia un file dal telefono (anche senza :)',
                ':run PROGRAMMA [ARG..]         esegue con vivad e aspetta lo stato di uscita',
                '                               (run senza : ritorna subito, l\'output arriva qui)',
                ':clear [log|out|tutto]         svuota il pannello selezionato (o quello indicato)',
                ':save [log|out] [FILE]         salva il pannello in un file',
                ':filter [TESTO]                filtra il log (vuoto = toglie il filtro)',
                ':layout                        affiancati / impilati (F2)',
                ':quit                          esce (anche Ctrl-C, Ctrl-D, Ctrl-Q)',
                'tasti: PgUp/PgDn ⇧↑/⇧↓ scorrono · ⇧⇥ cambia pannello · End segue · Esc segue entrambi',
                '       F2 layout · F3 massimizza · ^F filtro · ^L pulisce · mouse seleziona + Ctrl-C copia')])
        elif c == 'clear':
            which = args[0] if args else ('log' if self.selected is self.logp else 'out')
            for p in ((self.outp, self.logp) if which in ('tutto', 'all') else
                      (self.logp,) if which == 'log' else (self.outp,)):
                p.clear()
        elif c == 'save':
            pane = self.logp
            if args and args[0] in ('log', 'out'):
                pane = self.logp if args.pop(0) == 'log' else self.outp
            name = args[0] if args else time.strftime('viva-%s-%%Y%%m%%d-%%H%%M%%S.txt' %
                                                      ('log' if pane is self.logp else 'out'))
            try:
                with open(os.path.expanduser(name), 'w') as f:
                    f.write(pane.plain())
                self.say(':' + line, [('%d righe in %s' % (len(pane.lines), os.path.abspath(name)), 'ok', '')])
            except OSError as e:
                self.say(':' + line, [(str(e), 'err', '')])
        elif c == 'filter':
            self.query_one('#filter', Input).value = ' '.join(args)
            self.query_one('#filterbar').set_class(not args, 'hidden')
            self.logp.set_filter(' '.join(args))
        elif c == 'layout':
            self.action_layout()
        elif c in ('push', 'pull', 'run'):
            self.jobs.put((c, args))
        else:
            self.say(':' + line, [('comando locale sconosciuto (:help)', 'err', '')])

    def say(self, head, items):
        self.outp.append(self.block(head) + items)

    def block(self, head):
        return ([('', '', '')] if self.outp.lines else []) + [(head, 'cmd', time.strftime('%H:%M:%S'))]

    def worker(self):
        """Runs the queued commands one at a time (they share the link's reply buffer)."""
        vl, link = self.vl, self.link
        while True:
            kind, arg = self.jobs.get()
            head = arg if kind == 'cmd' else ':%s %s' % (kind, ' '.join(arg))
            self.busy = head
            self.ops.append(('out', self.block(head)))
            t0 = time.time()
            if kind == 'cmd':
                reply = link.command(arg)
                lines = reply.rstrip('\n').split('\n') if reply.strip() else []
                bad = reply.startswith('(no reply') or reply.startswith('(not sent') or reply.startswith('(link')
                items = [(clean(l), 'err' if bad else 'reply', '') for l in lines]
                items.append(('%s %s' % ('✗' if bad else '✓', ms(t0)), 'err' if bad else 'ok', ''))
                self.ops.append(('out', items))
                if link.log:            # commands and replies go to the log file too
                    link.log.write(('\n> %s\n%s' % (arg, reply)).encode())
                    link.log.flush()
            else:
                self.helper(kind, arg, t0)
            self.busy = None

    def helper(self, kind, args, t0):
        vl, link = self.vl, self.link
        status = 0
        try:
            if kind == 'push':
                if len(args) < 2:
                    raise SystemExit('push LOCALE PERCORSO [MODO]')
                vl.push(link, os.path.expanduser(args[0]), args[1], int(args[2], 8) if len(args) > 2 else 0o644)
            elif kind == 'pull':
                if not args:
                    raise SystemExit('pull PERCORSO [LOCALE]')
                vl.pull(link, args[0], os.path.expanduser(args[1]) if len(args) > 1 else os.path.basename(args[0]))
            elif kind == 'run':
                if not args:
                    raise SystemExit('run PROGRAMMA [ARG..]')
                vl.run(link, args)
        except SystemExit as e:
            status = e.code
        except (OSError, ValueError, vl.USBError) as e:
            status = str(e)
        if isinstance(status, str):
            self.ops.append(('out', [(status, 'err', ''), ('✗ %s' % ms(t0), 'err', '')]))
        elif status:
            self.ops.append(('out', [('✗ uscita %d · %s' % (status, ms(t0)), 'err', '')]))
        else:
            self.ops.append(('out', [('✓ %s%s' % ('uscita 0 · ' if kind == 'run' else '', ms(t0)), 'ok', '')]))

    # keys
    def select_pane(self, pane):
        self.selected.remove_class('selected')
        self.selected = pane
        pane.add_class('selected')

    def action_switch_pane(self):
        self.select_pane(self.outp if self.selected is self.logp else self.logp)

    def set_layout(self, mode):
        self.layout_ = mode
        self.query_one('#panes').set_class(mode == 'stack', 'stack')

    def action_layout(self):
        self.set_layout('side' if self.layout_ == 'stack' else 'stack')

    def action_maximize_pane(self):
        other = self.outp if self.selected is self.logp else self.logp
        if other.has_class('hidden') or self.selected.has_class('hidden'):
            self.outp.remove_class('hidden')
            self.logp.remove_class('hidden')
        else:
            other.add_class('hidden')

    def action_scroll_pane(self, direction):
        p = self.selected
        p.scroll_to(y=p.scroll_y + direction * max(1, p.scrollable_content_region.height - 2), animate=False)

    def action_scroll_line(self, direction):
        self.selected.scroll_to(y=self.selected.scroll_y + direction * 3, animate=False)

    def action_end(self):
        cmd = self.query_one('#cmd', CmdInput)
        if self.focused is cmd and cmd.cursor_position < len(cmd.value):
            cmd.action_end()
        else:
            self.selected.resume()

    def action_escape(self):
        if self.focused is self.query_one('#filter'):
            self.query_one('#filter', Input).value = ''
            self.logp.set_filter('')
            self.query_one('#filterbar').add_class('hidden')
            self.query_one('#cmd').focus()
        else:
            self.outp.resume()
            self.logp.resume()

    def action_filter(self):
        bar = self.query_one('#filterbar')
        f = self.query_one('#filter', Input)
        if not bar.has_class('hidden') and self.focused is f:
            self.query_one('#cmd').focus()
            return
        bar.remove_class('hidden')
        f.focus()

    def action_clear_pane(self):
        self.selected.clear()

    def action_ctrl_c(self):
        text = self.screen.get_selected_text()
        if text:
            self.copy_to_clipboard(text)
            try:
                subprocess.run(['pbcopy'], input=text.encode(), timeout=2)
            except (OSError, subprocess.SubprocessError):
                pass
            self.screen.clear_selection()
            self.notify('%d caratteri copiati' % len(text), timeout=2)
        else:
            self.action_quit_link()

    def action_quit_link(self):
        self.link.bye()
        self.exit()


def human(n):
    for unit in ('B', 'KB', 'MB', 'GB'):
        if n < 1024 or unit == 'GB':
            return ('%d %s' if unit == 'B' else '%.1f %s') % (n, unit)
        n /= 1024


def ms(t0):
    d = time.time() - t0
    return '%d ms' % (d * 1000) if d < 10 else '%.1f s' % d


def run(link, vl):
    VivaLinkApp(link, vl).run()
