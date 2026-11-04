#!/bin/bash
# XNU kernel -> viva boot image (shim + SWEETXNU payload in the ramdisk).
#   tools/viva-mkboot.sh KERNEL STOCK_BOOT_IMG [STAGE]   -> shim/out/viva-boot.img
#
# STAGE (default ufs-ro):
#   nostorage  no UFS node: the kernel never touches storage, panics at root mount
#   ufs-ro     UFS node, writes refused (no ufsrw): finds "userdata" and tries to mount it
#   ufs-rw     iOS root in userdata, read-write; writes stay inside that partition
# Env: VIVA_CMDLINE replaces the whole command line; VIVA_EXTRA sets single
# boot-args on top of it (default "cpus=8 vivausb=3 vivagpu=2 vivascanout=1": all
# eight cores incl. the two A76s, USB link, GPU on, direct scanout of CA's
# framebuffers (0133: SwapWait retires them); VIVA_EXTRA= turns the defaults off).
# sweetfb_noconsole=1: no kernel text on the screen from the GUI's first frame
# (boot Apple) on; the USB link still gets the whole log, a panic still draws,
# link `console on` brings it back (or VIVA_EXTRA="sweetfb_noconsole=0").
# Every stage turns off sweet's Qualcomm drivers (keys, touch, USB, persistent
# log): their fixed MMIO addresses are other blocks on MT6781.
set -euo pipefail
cd "$(dirname "$0")/.."
KERNEL=${1:?usage: viva-mkboot.sh KERNEL STOCK_BOOT_IMG [nostorage|ufs-ro|ufs-rw]}
STOCK=${2:?usage: viva-mkboot.sh KERNEL STOCK_BOOT_IMG [nostorage|ufs-ro|ufs-rw]}
STAGE=${3:-ufs-ro}

BASE='debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 wdt=-1 wdt_test=no_user_checkin sweetkeys=0 sweettouch=0 sweetusb=0 sweetusblog=0 sweetlog=0 ufslun=2 sweetfb_noconsole=1'
case $STAGE in
nostorage) ADT_FLAGS=(--viva); CMDLINE=$BASE ;;
ufs-ro) ADT_FLAGS=(--viva --ufs); CMDLINE=$BASE ;;
ufs-rw) ADT_FLAGS=(--viva --ufs)
	CMDLINE="$BASE ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1" ;;
*) echo "unknown stage $STAGE" >&2; exit 2 ;;
esac
CMDLINE=${VIVA_CMDLINE:-$CMDLINE}
# VIVA_EXTRA="cpus=6 vivausb=3": set/replace single boot-args (XNU uses the first match)
for arg in ${VIVA_EXTRA-cpus=8 vivausb=3 vivagpu=2 vivascanout=1}; do
	key=${arg%%=*}
	CMDLINE="$(printf ' %s ' "$CMDLINE" | sed -E "s/ -?$key(=[^ ]*)? / /g; s/^ +//; s/ +$//") $arg"
done
echo "stage $STAGE: $CMDLINE"

mkdir -p shim/out
python3 tools/mkadt.py shim/out/viva.adt "${ADT_FLAGS[@]}"
python3 tools/mkpayload.py "$KERNEL" shim/out/viva.adt shim/out/viva-payload.bin --cmdline "$CMDLINE"
# LK loads the ramdisk at 0x47c80000 and the DTB at 0x4bc80000: 64 MiB of room.
size=$(stat -f %z shim/out/viva-payload.bin 2>/dev/null || stat -c %s shim/out/viva-payload.bin)
[ "$size" -lt $((0x4bc80000 - 0x47c80000)) ] || { echo "error: payload ($size bytes) would overrun LK's DTB" >&2; exit 1; }
shim/build-viva.sh "$STOCK" "$PWD/shim/out/viva-payload.bin"
