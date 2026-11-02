#!/bin/bash
# XNU kernel -> viva boot image (shim + SWEETXNU payload in the ramdisk).
#   tools/viva-mkboot.sh KERNEL STOCK_BOOT_IMG   -> shim/out/viva-boot.img
set -euo pipefail
cd "$(dirname "$0")/.."
KERNEL=${1:?usage: viva-mkboot.sh KERNEL STOCK_BOOT_IMG}
STOCK=${2:?usage: viva-mkboot.sh KERNEL STOCK_BOOT_IMG}

# First boot: one core, framebuffer console, no storage (no /sweet-ufs node in
# the --viva ADT) and every sweet Qualcomm driver off: their fixed MMIO
# addresses are other blocks on MT6781.
CMDLINE=${VIVA_CMDLINE:-'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 wdt=-1 wdt_test=no_user_checkin sweetkeys=0 sweettouch=0 sweetusb=0 sweetusblog=0 sweetlog=0'}

mkdir -p shim/out
python3 tools/mkadt.py shim/out/viva.adt --viva
python3 tools/mkpayload.py "$KERNEL" shim/out/viva.adt shim/out/viva-payload.bin --cmdline "$CMDLINE"
# LK loads the ramdisk at 0x47c80000 and the DTB at 0x4bc80000: 64 MiB of room.
size=$(stat -f %z shim/out/viva-payload.bin 2>/dev/null || stat -c %s shim/out/viva-payload.bin)
[ "$size" -lt $((0x4bc80000 - 0x47c80000)) ] || { echo "error: payload ($size bytes) would overrun LK's DTB" >&2; exit 1; }
shim/build-viva.sh "$STOCK" shim/out/viva-payload.bin
