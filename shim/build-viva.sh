#!/bin/bash
# Build the viva shim with clang + lld and wrap it into the stock viva boot.img.
#   shim/build-viva.sh STOCK_BOOT_IMG [PAYLOAD]   -> shim/out/viva-boot.img
# Without PAYLOAD the stock ramdisk stays and the shim stops at "no SWEETXNU payload".
set -euo pipefail
cd "$(dirname "$0")"
STOCK=${1:?usage: build-viva.sh STOCK_BOOT_IMG [PAYLOAD]}
CFLAGS="--target=aarch64-none-elf -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -DVIVA"
mkdir -p out/viva
OBJS=()
for f in start.S viva.c console.c fdt.c loader.c; do
	o=out/viva/${f%.*}.o
	clang $CFLAGS -c $f -o $o
	OBJS+=($o)
done
# LK puts the Image wherever it likes and nothing relocates it: no absolute
# pointers may appear in the C objects (start.o's image_size is the exception).
python3 ../tools/viva-check-abs.py out/viva/viva.o out/viva/console.o out/viva/fdt.o out/viva/loader.o
ld.lld -T shim.ld --build-id=none -N "${OBJS[@]}" -o out/viva/shim.elf
ld.lld -T shim.ld --build-id=none -N "${OBJS[@]}" --oformat=binary -o out/viva/Image
size=$(python3 ../tools/viva-check-abs.py --symbol _end out/viva/shim.elf)
python3 -c "f=open('out/viva/Image','r+b'); f.truncate($size)"
# viva's LK only boots a gzip kernel; a raw Image is rejected (reboot after ~10 s).
gzip -9 -n -c out/viva/Image > out/viva/Image.gz
python3 ../tools/viva-bootimg.py repack "$STOCK" out/viva/Image.gz out/viva-boot.img ${2:+--ramdisk "$2"}
ls -l out/viva/Image out/viva/Image.gz out/viva-boot.img
