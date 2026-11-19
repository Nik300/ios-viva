#!/usr/bin/env python3
"""
Patch iOS's D22 speaker tunings for viva (run on the Mac; pull/push with tools/viva-link.py):
  /Library/Audio/Tunings/D22/VAD/d22_speaker_*.austrip  -> without AUSpeakerProtectionV3.
VirtualAudio builds the default (not closed-loop) speaker chain on viva (VivaAudio.driver rebinds
MGCopyAnswer("InternalBuild") + pref RawModeDisablesSpeakerProcessing); the strip's own AUSpeakerProtectionV3 would
still want Apple's amp telemetry. Everything else (virtual bass, EQs, compressor) stays: VivaAudio.driver high-passes
the loudspeaker at 350 Hz instead. Apple's files are not kept in git: back up the originals before pushing.
Usage: patch-tunings.py IN.austrip... OUTDIR
"""
import os
import plistlib
import sys

if len(sys.argv) < 3:
    sys.exit(__doc__)
out = sys.argv[-1]
os.makedirs(out, exist_ok=True)
for p in sys.argv[1:-1]:
    d = plistlib.load(open(p, 'rb'))
    n = 0
    for st in d.get('strips', []):
        before = len(st.get('effects', []))
        st['effects'] = [e for e in st.get('effects', []) if 'SpeakerProtection' not in e.get('displayname', '')]
        n += before - len(st['effects'])
    plistlib.dump(d, open(os.path.join(out, os.path.basename(p)), 'wb'), fmt=plistlib.FMT_BINARY)
    print(os.path.basename(p), 'removed', n)
