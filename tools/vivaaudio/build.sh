#!/bin/bash
# Builds VivaAudio.driver (the HAL AudioServerPlugIn, VivaAudio.c) for the phone: iOS 16 arm64, ad-hoc signed.
# CoreAudio's plug-in headers are macOS-only: they come from the macOS SDK (-F of a copy), everything else from the
# iOS SDK. Without Xcode's iOS SDK (Command Line Tools only) it only syntax-checks against the macOS SDK.
# Usage: tools/vivaaudio/build.sh [OUTDIR]   (OUTDIR/VivaAudio.driver)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
MSDK="$(xcrun --sdk macosx --show-sdk-path)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/fw/CoreAudio.framework"
cp -RL "$MSDK/System/Library/Frameworks/CoreAudio.framework/Headers" "$TMP/fw/CoreAudio.framework/Headers"
FLAGS=(-target arm64-apple-ios16.0 -O2 -Wall -Wno-unused-parameter -F "$TMP/fw" -I "$HERE")
if SDK="$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null)" && [ -n "$SDK" ]; then
  B="$OUT/VivaAudio.driver"
  rm -rf "$B"
  mkdir -p "$B"
  xcrun --sdk iphoneos clang "${FLAGS[@]}" -isysroot "$SDK" -bundle -o "$B/VivaAudio" "$HERE/VivaAudio.c" \
    -framework CoreFoundation -framework IOKit
  cp "$HERE/Info.plist" "$B/Info.plist"
  plutil -convert binary1 "$B/Info.plist"
  codesign -s - -f "$B"
  file "$B/VivaAudio"
else
  # Command Line Tools only: compile against the macOS SDK headers and link against .tbd stubs generated from the
  # object's undefined symbols (CF* -> CoreFoundation, IO* -> IOKit, the rest -> libSystem), as tools/vivad does.
  echo "vivaaudio/build.sh: no iOS SDK, macOS headers + generated stubs" >&2
  O="$TMP/VivaAudio.o"
  clang "${FLAGS[@]}" -isysroot "$MSDK" -Wno-incompatible-sysroot -c -o "$O" "$HERE/VivaAudio.c"
  mkdir -p "$TMP/sdk/usr/lib" "$TMP/sdk/System/Library/Frameworks/CoreFoundation.framework" \
    "$TMP/sdk/System/Library/Frameworks/IOKit.framework"
  tbd() {  # tbd FILE INSTALLNAME SYMBOLS...
    local f="$1" n="$2"; shift 2
    { echo '--- !tapi-tbd'; echo 'tbd-version: 4'; echo 'targets: [ arm64-ios ]'; echo "install-name: '$n'"
      echo 'current-version: 1'; echo 'exports:'; echo '  - targets: [ arm64-ios ]'
      echo "    symbols: [ $(printf '%s\n' "$@" | paste -sd, - | sed 's/,/, /g') ]"; echo '...'; } > "$f"
  }
  U=$(nm -u "$O" | grep -vE '^(dyld_stub_binder|___dso_handle)$')   # both come from the linker
  CF=$(echo "$U" | grep -E '^_+(CF|kCF)' || true)
  IK=$(echo "$U" | grep -E '^_(IO|kIO|io_)' || true)
  SY=$(echo "$U" | grep -vE '^_+(CF|kCF)|^_(IO|kIO|io_)' || true)
  tbd "$TMP/sdk/usr/lib/libSystem.tbd" /usr/lib/libSystem.B.dylib $SY
  tbd "$TMP/sdk/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation.tbd" \
    /System/Library/Frameworks/CoreFoundation.framework/CoreFoundation $CF
  tbd "$TMP/sdk/System/Library/Frameworks/IOKit.framework/IOKit.tbd" \
    /System/Library/Frameworks/IOKit.framework/Versions/A/IOKit $IK
  B="$OUT/VivaAudio.driver"
  rm -rf "$B"
  mkdir -p "$B"
  clang -target arm64-apple-ios16.0 -nostdlib -bundle -Wl,-syslibroot,"$TMP/sdk" -L"$TMP/sdk/usr/lib" -lSystem \
    -F"$TMP/sdk/System/Library/Frameworks" -framework CoreFoundation -framework IOKit -o "$B/VivaAudio" "$O"
  cp "$HERE/Info.plist" "$B/Info.plist"
  plutil -convert binary1 "$B/Info.plist"
  codesign -s - -f "$B"
  file "$B/VivaAudio"
fi
