#!/usr/bin/env python3
"""Inspect an Android boot.img (header v0-v4) or swap its kernel, keeping everything else.

  bootimg.py info  stock-boot.img
  bootimg.py repack stock-boot.img new-Image out-boot.img [--ramdisk FILE]
"""
import struct
import sys

MTK_MAGIC = 0x58881688
FDT_MAGIC = b"\xd0\x0d\xfe\xed"


def align(n, page):
    return (n + page - 1) // page * page


def parse(data):
    if data[:8] != b"ANDROID!":
        sys.exit("not an Android boot image")
    version, = struct.unpack_from("<I", data, 40)
    h = {"version": version}
    if version >= 3:
        h["page"] = 4096
        h["kernel_size"], h["ramdisk_size"], h["os_version"], h["header_size"] = struct.unpack_from("<4I", data, 8)
        h["cmdline"] = data[44:44 + 1536].split(b"\0")[0]
        sizes = [("kernel", h["kernel_size"]), ("ramdisk", h["ramdisk_size"])]
        if version == 4:
            sizes.append(("signature", struct.unpack_from("<I", data, 1580)[0]))
    else:
        (h["kernel_size"], h["kernel_addr"], h["ramdisk_size"], h["ramdisk_addr"], h["second_size"],
         h["second_addr"], h["tags_addr"], h["page"]) = struct.unpack_from("<8I", data, 8)
        h["cmdline"] = data[64:64 + 512].split(b"\0")[0] + data[608:608 + 1024].split(b"\0")[0]
        sizes = [("kernel", h["kernel_size"]), ("ramdisk", h["ramdisk_size"]), ("second", h["second_size"])]
        if version >= 1:
            sizes.append(("recovery_dtbo", struct.unpack_from("<I", data, 1632)[0]))
        if version >= 2:
            dtb_size, dtb_addr = struct.unpack_from("<IQ", data, 1648)
            h["dtb_addr"] = dtb_addr
            sizes.append(("dtb", dtb_size))
    sections, off = [], h["page"]
    for name, size in sizes:
        sections.append((name, off, size))
        off += align(size, h["page"])
    h["sections"] = sections
    return h


def section(data, h, name):
    for n, off, size in h["sections"]:
        if n == name:
            return data[off:off + size]
    return b""


def describe_kernel(k):
    notes = []
    if len(k) >= 512 and struct.unpack_from("<I", k, 0)[0] == MTK_MAGIC:
        notes.append("MTK 512-byte header '%s'" % k[8:40].split(b"\0")[0].decode(errors="replace"))
        k = k[512:]
    if k[:2] == b"\x1f\x8b":
        notes.append("gzip")
    elif k[:4] == b"\x02\x21\x4c\x18":
        notes.append("lz4")
    elif k[56:60] == b"ARM\x64":
        notes.append("raw arm64 Image")
    fdt = k.find(FDT_MAGIC)
    if fdt > 0:
        notes.append("appended DTB at +0x%x" % fdt)
    return notes


def info(path):
    data = open(path, "rb").read()
    h = parse(data)
    print("header v%d, page %d" % (h["version"], h["page"]))
    for key in ("kernel_addr", "ramdisk_addr", "second_addr", "tags_addr", "dtb_addr"):
        if key in h:
            print("  %-13s 0x%08x" % (key, h[key]))
    for name, off, size in h["sections"]:
        print("  %-13s off 0x%08x size %d" % (name, off, size))
    print("  cmdline       %r" % h["cmdline"].decode(errors="replace"))
    print("  kernel        %s" % (", ".join(describe_kernel(section(data, h, "kernel"))) or "unknown format"))


def repack(stock, image, out, ramdisk=None):
    data = open(stock, "rb").read()
    h = parse(data)
    new_kernel = open(image, "rb").read()
    old_kernel = section(data, h, "kernel")

    if struct.unpack_from("<I", old_kernel, 0)[0] == MTK_MAGIC:
        hdr = bytearray(old_kernel[:512])
        struct.pack_into("<I", hdr, 4, len(new_kernel))
        new_kernel = bytes(hdr) + new_kernel
        print("kept MTK kernel header")

    # Before header v2 the DTB rides behind the kernel; LK still needs it.
    if h["version"] < 2:
        fdt = old_kernel.find(FDT_MAGIC)
        if fdt > 0:
            new_kernel += old_kernel[fdt:]
            print("re-appended stock DTB (%d bytes)" % (len(old_kernel) - fdt))

    page = h["page"]
    header = bytearray(data[:page])
    struct.pack_into("<I", header, 8, len(new_kernel))
    if h["version"] < 3:
        header[576:576 + 32] = bytes(32)

    if ramdisk is not None:
        if h["version"] >= 3:
            sys.exit("--ramdisk is only supported for header v0-v2")
        rd = open(ramdisk, "rb").read()
        struct.pack_into("<I", header, 16, len(rd))
        print("replaced ramdisk (%d bytes)" % len(rd))

    body = new_kernel + bytes(align(len(new_kernel), page) - len(new_kernel))
    for name, off, size in h["sections"][1:]:
        chunk = rd if name == "ramdisk" and ramdisk is not None else data[off:off + size]
        size = len(chunk)
        body += chunk + bytes(align(size, page) - size)
    open(out, "wb").write(bytes(header) + body)
    print("wrote %s (%d bytes)" % (out, page + len(body)))


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "info":
        info(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == "repack":
        repack(*sys.argv[2:])
    elif len(sys.argv) == 7 and sys.argv[1] == "repack" and sys.argv[5] == "--ramdisk":
        repack(*sys.argv[2:5], ramdisk=sys.argv[6])
    else:
        sys.exit(__doc__)
