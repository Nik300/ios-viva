#!/usr/bin/env python3
"""Link server against the simulated phone (no USB): several clients at once, the interface through the server,
`watch --until`, the journal.

  python3 tools/tests/test_viva_link_server.py
"""
import asyncio, importlib.util, os, subprocess, sys, tempfile, time

os.environ['VIVA_LINK_FAKE'] = '1'
os.environ['VIVA_LINK_FAKE_SWITCH'] = '0'
os.environ['VIVA_LINK_HOME'] = tempfile.mkdtemp()
os.environ['HOME'] = tempfile.mkdtemp()         # history file of the interface
TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SCRIPT = os.path.join(TOOLS, 'viva-link.py')
sys.path.insert(0, TOOLS)
spec = importlib.util.spec_from_file_location('viva_link', SCRIPT)
vl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vl)
import viva_link_server as srv, viva_link_tui      # noqa: E402


def tool(*args, timeout=30):
    return subprocess.run([sys.executable, SCRIPT] + list(args), capture_output=True, text=True, timeout=timeout)


def main():
    try:
        r = tool('cmd', 'ping')
        assert 'pong' in r.stdout, r
        assert srv.alive(), 'no server after the first command'
        print('ok  first command starts the server')

        # a long watch and one-shot commands at the same time
        w = subprocess.Popen([sys.executable, SCRIPT, 'watch', '--until', 'error', '--count', '3', '--timeout', '20',
                              '--bare'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        for _ in range(3):
            r = tool('cmd', 'stat')
            assert 'uptime' in r.stdout, r
        r = tool('run', 'vctl', 'ls', '/')
        assert r.returncode == 0 and '[' in r.stdout, r
        out, err = w.communicate(timeout=30)
        assert w.returncode == 0, (w.returncode, out, err)
        assert out.count('error') >= 3 and out.startswith('S link connected'), out
        print('ok  watch --until while one-shot commands run')

        journal = open(os.path.join(srv.JOURNAL, time.strftime('%Y-%m-%d') + '.log')).read()
        assert ' C > stat (cmd stat)' in journal and ' L ' in journal and ' S client ' in journal, journal[-2000:]
        print('ok  journal has commands, kernel lines and clients')

        asyncio.run(tui())
        print('ok  interface through the server: log, command, state')
    finally:
        tool('stop')
    assert not srv.alive()
    print('ok  stop')


async def tui():
    link = srv.RemoteLink(SCRIPT, 'test-tui', reconnect=True)
    app = viva_link_tui.VivaLinkApp(link, vl, log_lines=800)
    async with app.run_test(size=(170, 46)) as pilot:
        t0 = time.time()
        while link.state != 'connected' or len(app.logp.lines) < 20:
            assert time.time() - t0 < 10, 'interface never connected'
            await pilot.pause(0.05)
        await pilot.press(*'ping', 'enter')
        t0 = time.time()
        while 'pong' not in [t for t, _, _ in app.outp.lines]:
            assert time.time() - t0 < 10, 'no pong'
            await pilot.pause(0.05)
        await app.action_quit() if asyncio.iscoroutinefunction(app.action_quit) else app.action_quit()


if __name__ == '__main__':
    main()
