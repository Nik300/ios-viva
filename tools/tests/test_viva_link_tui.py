#!/usr/bin/env python3
"""Headless test of viva-link.py's interface against the simulated phone (no USB).

  python3 tools/tests/test_viva_link_tui.py [SCREENSHOT_DIR]

Checks: typing while the log streams, a scrolled-back log pane that stays put (also across trimming),
command output and phone log in separate panes, run output, the log filter, reconnect in the status bar.
"""
import asyncio, importlib.util, os, sys, tempfile, time

os.environ['VIVA_LINK_FAKE'] = '1'
os.environ['VIVA_LINK_FAKE_SWITCH'] = '0'      # the test drops the phone itself
os.environ['HOME'] = tempfile.mkdtemp()         # history file
TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, TOOLS)
spec = importlib.util.spec_from_file_location('viva_link', os.path.join(TOOLS, 'viva-link.py'))
vl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vl)
import viva_link_fake, viva_link_tui                 # noqa: E402

SHOTS = sys.argv[1] if len(sys.argv) > 1 else tempfile.gettempdir()


def texts(pane):
    return [t for t, _, _ in pane.lines]


async def until(pilot, cond, what, timeout=8):
    t0 = time.time()
    while not cond():
        if time.time() - t0 > timeout:
            raise AssertionError('timeout waiting for ' + what)
        await pilot.pause(0.05)


def status(app):
    return ' '.join(str(app.query_one('#' + i).render()) for i in ('conn', 'persona', 'hb'))


async def main():
    link = vl.Link(None, reconnect=True)
    app = viva_link_tui.VivaLinkApp(link, vl, log_lines=800)
    async with app.run_test(size=(170, 46)) as pilot:
        out, log = app.outp, app.logp
        await until(pilot, lambda: link.state == 'connected', 'connection')
        await until(pilot, lambda: 'connesso' in status(app), 'status connesso')
        await until(pilot, lambda: len(log.lines) > 50, 'log lines')

        # typing while the log streams: the prompt keeps every key, the command goes to the output pane
        await pilot.press(*'ping')
        assert app.query_one('#cmd').value == 'ping', app.query_one('#cmd').value
        await pilot.press('enter')
        await until(pilot, lambda: 'pong' in texts(out), 'pong')
        assert ('ping', 'cmd') in [(t, g) for t, g, _ in out.lines]
        assert 'pong' not in texts(log), 'reply leaked into the log pane'
        assert not any(g in ('cmd', 'reply', 'ok') for _, g, _ in log.lines)
        assert any(t.startswith('✓') for t in texts(out))
        print('ok  command while streaming: reply only in the output pane')

        # scroll the log back: the view stays on the same lines while new ones (and bursts) arrive, also when
        # the buffer (800 lines here) drops its oldest lines
        await until(pilot, lambda: len(log.lines) > 500, '500 log lines', 20)
        await pilot.press('pageup', 'pageup')
        assert not log.follow, 'still following after PageUp'
        await pilot.pause(0.2)
        y0, top0 = log.scroll_y, log.rows[int(log.scroll_y)]
        n0 = log.base + len(log.lines)
        top_text = log.lines[top0[0] - log.base][0]
        await until(pilot, lambda: log.base > 0 and log.base + len(log.lines) > n0 + 200, 'trimming', 30)
        i, s, _ = log.rows[int(log.scroll_y)]
        assert i == top0[0] and s == top0[1], 'scrolled-back view moved (%r -> %r)' % (top0, log.rows[int(log.scroll_y)])
        assert log.lines[i - log.base][0] == top_text
        assert log.unseen > 0 and 'fermo' in str(log.border_subtitle)
        print('ok  scrolled back: top line unchanged after %d new lines (y %d -> %d, trimmed base %d)'
              % (log.base + len(log.lines) - n0, y0, log.scroll_y, log.base))
        app.save_screenshot(os.path.join(SHOTS, 'viva-link-paused.svg'))
        await pilot.press('end')
        await pilot.pause(0.1)
        assert log.follow and log.scroll_y == log.max_scroll_y
        print('ok  End resumes following')

        # :run waits for vivad's exit; its output lands in the output pane
        await pilot.press(*':run hello', 'enter')
        await until(pilot, lambda: any('uscita 0' in t for t in texts(out)), 'run exit')
        assert any(t.startswith('[') and 'step 8 of 8' in t for t in texts(out))
        assert not any('step 8 of 8' in t for t in texts(log))
        print('ok  :run output in the output pane, exit status shown')

        # Tab completion and history
        await pilot.press(*'pi', 'tab')
        assert app.query_one('#cmd').value == 'ping ', app.query_one('#cmd').value
        app.query_one('#cmd').value = ''
        await pilot.press(*'i2', 'tab')
        await pilot.pause(0.1)
        assert app.query_one('#cmd').value == 'i2c' and 'i2c8w' in str(app.query_one('#cmd').border_subtitle)
        app.query_one('#cmd').value = ''
        await pilot.press('up')
        assert app.query_one('#cmd').value == ':run hello'
        app.query_one('#cmd').value = ''
        print('ok  Tab completion and history')

        # log filter
        await pilot.press('ctrl+f', *'error')
        await pilot.pause(0.2)
        assert log.rows and all('error' in log.lines[i - log.base][0].lower() for i, _, _ in log.rows)
        await pilot.press('enter')
        assert app.focused is app.query_one('#cmd')
        app.save_screenshot(os.path.join(SHOTS, 'viva-link-filter.svg'))
        await pilot.press('ctrl+f', 'escape')
        assert not log.filter
        print('ok  Ctrl-F filter')

        # the phone goes away (persona switch): status bar shows it, then reattaches as `std`
        viva_link_fake.BUS.drop(1.0, 'std')
        await until(pilot, lambda: 'perso' in status(app), 'lost in the status bar')
        print('ok  status: %s' % status(app))
        app.save_screenshot(os.path.join(SHOTS, 'viva-link-lost.svg'))
        await until(pilot, lambda: 'connesso' in status(app) and 'std' in status(app), 'reconnect')
        assert link.port.ifn == 2
        await pilot.press(*'stat', 'enter')
        await until(pilot, lambda: any('persona std' in t for t in texts(out)), 'stat after reconnect')
        print('ok  reconnect: %s' % status(app))
        await pilot.pause(1.5)
        assert 'attivo' in status(app), status(app)

        # :pull and :push run in the worker (the prompt stays live) and report in the output pane
        local = os.path.join(tempfile.mkdtemp(), 'blob')
        await pilot.press(*(':pull /var/x ' + local), 'enter')
        await until(pilot, lambda: any('150000 bytes' in t for t in texts(out)), 'pull')
        await pilot.press(*('push %s /tmp/blob' % local), 'enter')
        await until(pilot, lambda: any('push /tmp/blob: ok' in t for t in texts(out)), 'push')
        print('ok  pull + push through the helpers')

        # old kernel: the heartbeat request stalls
        os.environ['VIVA_LINK_FAKE'] = 'stall'
        await until(pilot, lambda: 'non supportato' in status(app), 'heartbeat stall')
        os.environ['VIVA_LINK_FAKE'] = '1'
        await until(pilot, lambda: 'attivo' in status(app), 'heartbeat back')
        print('ok  heartbeat stall shown, then back')

        # layouts
        await pilot.press('f2')
        await pilot.pause(0.1)
        assert app.layout_ == 'stack'
        app.save_screenshot(os.path.join(SHOTS, 'viva-link-stack.svg'))
        await pilot.press('f2')
        await pilot.pause(0.3)
        app.save_screenshot(os.path.join(SHOTS, 'viva-link.svg'))
        print('screenshots in', SHOTS)

        phone = link.port.dev
        await pilot.press('ctrl+c')
        await pilot.pause(0.2)
        assert phone.active == 0, 'no bye on quit'
        print('ok  Ctrl-C quits and sends the bye')


asyncio.run(main())
print('all passed')
sys.stdout.flush()
os._exit(0)     # the pump thread is blocked in the simulator
