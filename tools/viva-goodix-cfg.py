#!/usr/bin/env python3
"""Print the register map in a Goodix GTx8 config bin (goodix_cfg_group.bin from the phone's
vendor partition: tools/viva-super-extract.py SUPER vendor_a vendor.img, then
debugfs -R "dump /firmware/goodix_cfg_group.bin OUT" vendor.img). Layout: Xiaomi viva-r-oss
drivers/input/touchscreen/goodix_gtx8/goodix_cfg_bin.h.

  viva-goodix-cfg.py goodix_cfg_group.bin
"""
import struct, sys

REGS = ['cfg_send_flag', 'version_base', 'pid', 'vid', 'sensor_id', 'fw_mask', 'fw_status',
        'cfg_addr', 'esd', 'command', 'coor', 'gesture', 'fw_request', 'proximity']

d = open(sys.argv[1], 'rb').read()
bin_len, pkgs = struct.unpack('<I', d[:4])[0], d[9]
print('bin_len %d, %d package(s)' % (bin_len, pkgs))
for i in range(pkgs):
    off = struct.unpack('<H', d[16 + 2 * i:18 + 2 * i])[0]
    c = d[off:]
    ic = c[4:19].split(b'\0')[0].decode()
    print('package %d: len %d ic_type %s cfg_type %d sensor %d pid %r' % (
        i, struct.unpack('<I', c[:4])[0], ic, c[19], c[20], c[21:29].split(b'\0')[0]))
    base = 4 + 15 + 1 + 1 + 8 + 8 + 9 + 4 + 6
    for j, name in enumerate(REGS):
        addr, r1, _ = struct.unpack('<HBB', c[base + 4 * j:base + 4 * j + 4])
        print('  %-14s 0x%04x (%d)' % (name, addr, r1))
