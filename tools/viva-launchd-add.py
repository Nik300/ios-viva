#!/usr/bin/env python3
"""Add the vivad LaunchDaemon (tools/vivad) to iOS's launchd job cache.

  viva-launchd-add.py IN.plist OUT.plist [PROGRAM [LABEL]]

IN is /System/Library/xpc/launchd.plist (binary; read by launchd when booted
with launchd_unsecure_cache=1), e.g. fetched from the phone with
  tools/viva-link.py pull /System/Library/xpc/launchd.plist launchd.plist
OUT gets a root job com.viva.vivad (PROGRAM, default /usr/libexec/vivad),
started at boot and restarted when it exits, stderr on the console. Push
both back and reboot:
  tools/viva-link.py push tools/vivad/out/vivad /usr/libexec/vivad 755
  tools/viva-link.py push launchd-vivad.plist /System/Library/xpc/launchd.plist 644

LABEL (default com.viva.vivad) adds another daemon the same way, e.g. the std USB
persona daemon (tools/vivad/vivausbd.c), on top of a plist that already has vivad:
  tools/viva-launchd-add.py launchd-vivad.plist launchd-usbd.plist \
      /usr/libexec/vivausbd com.viva.vivausbd
  tools/viva-link.py push tools/vivad/out/vivausbd /usr/libexec/vivausbd 755
  tools/viva-link.py push launchd-usbd.plist /System/Library/xpc/launchd.plist 644
"""
import plistlib, sys

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    program = sys.argv[3] if len(sys.argv) > 3 else '/usr/libexec/vivad'
    LABEL = sys.argv[4] if len(sys.argv) > 4 else 'com.viva.vivad'
    with open(src, 'rb') as f:
        plist = plistlib.load(f)
    daemons = plist['LaunchDaemons']
    for key in [k for k, job in daemons.items() if job.get('Label') == LABEL]:
        del daemons[key]
    daemons['/System/Library/LaunchDaemons/%s.plist' % LABEL] = {
        'Label': LABEL,
        'Program': program,
        'ProgramArguments': [program],
        'RunAtLoad': True,
        'KeepAlive': True,
        'UserName': 'root',
        'StandardErrorPath': '/dev/console',
        'StandardOutPath': '/dev/console',
    }
    with open(dst, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    with open(dst, 'rb') as f:
        assert plistlib.load(f)['LaunchDaemons']['/System/Library/LaunchDaemons/%s.plist' % LABEL]['Label'] == LABEL
    print('%s: %d jobs, %s -> %s' % (dst, len(daemons), LABEL, program))


if __name__ == '__main__':
    main()
