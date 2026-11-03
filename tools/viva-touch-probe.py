#!/usr/bin/env python3
"""Read-only state of the touch path on viva, over the USB link (vivausb=3): clock gates of
I2C0, and mode/direction/output/input of the GT9896 pins (13 irq, 17 reset, 73 dvdd, 166 avdd).
Only `peek` (reads); I2C0 registers are read only if its clock gate is open.

Layouts from Xiaomi viva-r-oss: clk-mt6781.c (infra1 CG sta 0x10001094 bit 29 = infra_imp_iic,
imp_iic_wrap 0x11017000 CG sta 0xe00 bit 0 = i2c0; 1 = gated), pinctrl-mt6781.c (GPIO
0x10005000: DIR 0x000, DOUT 0x100, DIN 0x200 + 0x10 per 32 pins, MODE 0x300 + 0x10 per 8 pins,
4 bits per pin)."""
import importlib.util, os, threading

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('vl', os.path.join(here, 'viva-link.py'))
vl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vl)

GPIO = 0x10005000


def word(link, pa):
    reply = link.command('peek 0x%x 1' % pa, 5)
    return int(reply.split(':', 1)[1].split()[0], 16)


def main():
    link = vl.Link(None, show_log=False)
    threading.Thread(target=link.pump, daemon=True).start()
    infra = word(link, 0x10001094)
    imp = word(link, 0x11017e00)
    print('infra cg sta1 0x%08x: imp_iic %s' % (infra, 'GATED' if infra >> 29 & 1 else 'on'))
    print('imp_iic_wrap cg sta 0x%08x: i2c0 %s' % (imp, 'GATED' if imp & 1 else 'on'))
    for pin, name in ((13, 'irq'), (17, 'reset'), (73, 'dvdd'), (166, 'avdd')):
        bank, bit = pin // 32, pin % 32
        mode = word(link, GPIO + 0x300 + 0x10 * (pin // 8)) >> (4 * (pin % 8)) & 0xf
        d = word(link, GPIO + 0x000 + 0x10 * bank) >> bit & 1
        o = word(link, GPIO + 0x100 + 0x10 * bank) >> bit & 1
        i = word(link, GPIO + 0x200 + 0x10 * bank) >> bit & 1
        print('GPIO%-3d %-5s mode %d dir %s dout %d din %d' % (pin, name, mode, 'out' if d else 'in', o, i))
    if not (infra >> 29 & 1) and not (imp & 1):
        print('i2c0 @0x11007000:', link.command('peek 0x11007000 32', 5).strip())


if __name__ == '__main__':
    main()
