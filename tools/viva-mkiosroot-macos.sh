#!/bin/bash
# macOS port of tools/mkiosroot.sh: build the iOS root image for viva's
# userdata partition straight from the IPSW, with macOS's own APFS and HFS+.
#
#   sudo tools/viva-mkiosroot-macos.sh IPSW OUT.img [SIZE]      (SIZE default 16g)
#
# Produces OUT.img (raw, sparse file: one case-sensitive journaled HFS+ volume,
# no partition map) with the OS root, both cryptexes, the same links and
# keybag as mkiosroot.sh, and tools/sweet-root-tweaks.py applied. Then
# tools/viva-img2simg.py turns it into an Android sparse image for fastboot.
#
# Files are copied with GNU rsync (Homebrew): it reads through macOS's APFS
# driver, which decompresses LZBITMAP/LZFSE files, and copies no BSD flags or
# xattrs, so nothing on the HFS+ volume needs AppleFSCompression (the viva
# kernel has no kexts). Owners and modes are kept (ownership enabled, root).
set -euo pipefail

[[ $# -ge 2 ]] || { sed -n '2,13p' "$0"; exit 2; }
[[ $EUID == 0 ]] || { echo "run with sudo (owners must be preserved)" >&2; exit 1; }
ipsw=$(cd "$(dirname "$1")" && pwd)/$(basename "$1") out=$2 size=${3:-16g}
here=$(cd "$(dirname "$0")" && pwd)
RSYNC=/opt/homebrew/bin/rsync
[[ -x $RSYNC ]] || { echo "need GNU rsync: brew install rsync" >&2; exit 1; }
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }

work=$(mktemp -d /tmp/viva-iosroot.XXXXXX)
# macOS /bin/bash is 3.2: with set -u an empty array counts as unset, so
# arrays are only expanded through their length.
attached=()
cleanup() {
	local i
	for ((i = ${#attached[@]} - 1; i >= 0; i--)); do
		hdiutil detach -force "${attached[$i]}" >/dev/null 2>&1 || true
	done
	rm -f "$work"/*.dmg
	rmdir "$work"/* "$work" 2>/dev/null || true
}
trap cleanup EXIT

# OS, SystemOS and AppOS images named by the BuildManifest (first identity).
read -r os_dmg sys_dmg app_dmg < <(python3 - "$ipsw" <<'EOF'
import plistlib, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
m = plistlib.loads(z.read('BuildManifest.plist'))
ident = m['BuildIdentities'][0]['Manifest']
print(ident['OS']['Info']['Path'], ident['Cryptex1,SystemOS']['Info']['Path'],
      ident['Cryptex1,AppOS']['Info']['Path'])
EOF
)
echo "OS $os_dmg, SystemOS $sys_dmg, AppOS $app_dmg"
for f in "$os_dmg" "$sys_dmg" "$app_dmg"; do
	echo "extracting $f"
	unzip -q -o "$ipsw" "$f" -d "$work"
done

# Attach read-only at a fixed mount point; cryptexes are raw APFS (no UDIF).
attach() {	# image mountpoint [raw]
	mkdir -p "$2"
	local dev
	if [[ ${3:-} == raw ]]; then
		dev=$(hdiutil attach -nobrowse -readonly -noverify -owners on \
			-imagekey diskimage-class=CRawDiskImage -mountpoint "$2" "$1" | awk 'NR==1{print $1}')
	else
		dev=$(hdiutil attach -nobrowse -readonly -noverify -owners on -mountpoint "$2" "$1" |
			awk 'NR==1{print $1}')
	fi
	attached+=("$dev")
}
attach "$work/$os_dmg" "$work/os"
attach "$work/$sys_dmg" "$work/sysos" raw
attach "$work/$app_dmg" "$work/appos" raw

# Blank sparse raw image -> HFS+ (case-sensitive, journaled), mounted with owners.
mkfile -n "$size" "$out"
dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$out" | awk 'NR==1{print $1}')
attached+=("$dev")
newfs_hfs -s -J -v iOSRoot "/dev/r${dev#/dev/}"
mkdir -p "$work/root"
mount_hfs -o nobrowse "$dev" "$work/root"
# Keep Spotlight and fseventsd from writing their stores into the iOS root.
mkdir -p "$work/root/.fseventsd"
: > "$work/root/.fseventsd/no_log"
: > "$work/root/.metadata_never_index"

copy() {
	echo "copying $1 -> ${2#"$work/root"}"
	mkdir -p "$2"
	"$RSYNC" -aH --numeric-ids "$1/" "$2/"
}
copy "$work/os" "$work/root"
copy "$work/sysos" "$work/root/private/preboot/Cryptexes/OS"
copy "$work/appos" "$work/root/private/preboot/Cryptexes/App"

# Shared cache where dyld looks at ignition level 1: hard links, no extra space.
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
mkdir -p "$work/root/System/Library/Caches/com.apple.dyld"
for f in "$work/root/$dsc"/*; do
	ln "$f" "$work/root/System/Library/Caches/com.apple.dyld/${f##*/}"
done

# Cryptex files with no counterpart in the OS root get a link at their root path.
for c in OS App; do
	(cd "$work/root/private/preboot/Cryptexes/$c" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if [[ ! -e "$work/root/$f" && ! -L "$work/root/$f" ]]; then
			mkdir -p "$work/root/$(dirname "$f")"
			src="$work/root/private/preboot/Cryptexes/$c/$f"
			if [[ -L $src ]]; then cp -P "$src" "$work/root/$f"; else ln "$src" "$work/root/$f"; fi
		fi
	done
done

# logd's stores (owner _logd 272, .migrated markers), as mkiosroot.sh.
db="$work/root/private/var/db"
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	: > "$db/$d/.migrated"
	chown 272:272 "$db/$d/.migrated"
	chmod 0644 "$db/$d/.migrated"
done

# Plain-plist system keybag for the built-in AppleKeyStore (no passcode).
install -d -m 0700 -o 0 -g 0 "$work/root/private/var/keybags"
kb="$work/root/private/var/keybags/systembag.kb"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' > "$kb"
chown 0:0 "$kb"
chmod 0600 "$kb"

python3 "$here/sweet-root-tweaks.py" "$work/root"

# Nothing on the volume may still be HFS-compressed.
left=$(find "$work/root" -flags +compressed -print -quit 2>/dev/null || true)
[[ -z $left ]] || { echo "error: compressed file left: $left" >&2; exit 1; }

rm -rf "$work/root/.Spotlight-V100" "$work/root/.Trashes"
df -h "$work/root"
umount "$work/root"
fsck_hfs -f -n "/dev/r${dev#/dev/}"
chown "${SUDO_UID:-0}:${SUDO_GID:-0}" "$out"
echo "done: $out (convert with tools/viva-img2simg.py)"
