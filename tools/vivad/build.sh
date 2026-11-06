#!/bin/bash
# Builds vivad (tools/vivad/vivad.c) for the phone: iOS 16 arm64, ad-hoc signed. Needs Xcode's iOS SDK
# (the xnu CI job builds it into the xnu-kernel artifact).  Usage: tools/vivad/build.sh [OUT]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out/vivad}"
mkdir -p "$(dirname "$OUT")"
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -O2 -Wall -o "$OUT" "$HERE/vivad.c"
codesign -s - -f "$OUT"
file "$OUT"
# vln (symbolic links: iOS has no ln), next to vivad
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -O2 -Wall -o "$(dirname "$OUT")/vln" "$HERE/vln.c"
codesign -s - -f "$(dirname "$OUT")/vln"
# vbootrec (raw VIVABOOT record, tools/viva-link.py bootrec), next to vivad
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -O2 -Wall -o "$(dirname "$OUT")/vbootrec" "$HERE/vbootrec.c"
codesign -s - -f "$(dirname "$OUT")/vbootrec"
