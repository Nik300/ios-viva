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
  echo "vivaaudio/build.sh: no iOS SDK, syntax check only" >&2
  clang "${FLAGS[@]}" -isysroot "$MSDK" -Wno-incompatible-sysroot -fsyntax-only "$HERE/VivaAudio.c"
fi
