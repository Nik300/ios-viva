#!/bin/bash
# Builds vivad (tools/vivad/vivad.c) and its companions for the phone: iOS 16 arm64, ad-hoc signed.
# Uses Xcode's iOS SDK when present (the xnu CI job builds them into the xnu-kernel artifact).
# Without it (Command Line Tools only) it compiles against the macOS SDK headers with
# -target arm64-apple-ios16.0 and links against a libSystem .tbd generated from the objects'
# undefined symbols (everything these tools use lives in libSystem).
# Usage: tools/vivad/build.sh [OUT]   (OUT = path of vivad; the others go next to it)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out/vivad}"
DIR="$(dirname "$OUT")"
mkdir -p "$DIR"

if SDK="$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null)" && [ -n "$SDK" ]; then
  build() {  # build OUTPUT SOURCE
    xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -O2 -Wall -o "$1" "$2"
    codesign -s - -f "$1"
  }
else
  echo "build.sh: no iOS SDK, using the macOS SDK headers + a generated libSystem.tbd" >&2
  MSDK="$(xcrun --show-sdk-path)"
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  build() {
    local obj="$TMP/$(basename "$1").o"
    clang -target arm64-apple-ios16.0 -isysroot "$MSDK" -Wno-incompatible-sysroot -O2 -Wall -c -o "$obj" "$2"
    mkdir -p "$TMP/sdk/usr/lib"
    {
      echo '--- !tapi-tbd'
      echo 'tbd-version: 4'
      echo 'targets: [ arm64-ios ]'
      echo "install-name: '/usr/lib/libSystem.B.dylib'"
      echo 'current-version: 1311'
      echo 'exports:'
      echo '  - targets: [ arm64-ios ]'
      echo "    symbols: [ $(nm -u "$obj" | grep -v '^dyld_stub_binder$' | paste -sd, - | sed 's/,/, /g') ]"
      echo '...'
    } > "$TMP/sdk/usr/lib/libSystem.tbd"
    clang -target arm64-apple-ios16.0 -Wno-incompatible-sysroot -nostdlib -Wl,-syslibroot,"$TMP/sdk" \
      -L"$TMP/sdk/usr/lib" -lSystem -o "$1" "$obj"
    codesign -s - -f "$1"
  }
fi

build "$OUT" "$HERE/vivad.c"
file "$OUT"
# vln (symbolic links: iOS has no ln), next to vivad
build "$DIR/vln" "$HERE/vln.c"
# vbootrec (raw VIVABOOT record, tools/viva-link.py bootrec), next to vivad
build "$DIR/vbootrec" "$HERE/vbootrec.c"
# vivausbd (PTP + usbmux of the std USB persona, docs/VIVA-USB-PERSONA.md), next to vivad
build "$DIR/vivausbd" "$HERE/vivausbd.c"
file "$DIR/vivausbd"
