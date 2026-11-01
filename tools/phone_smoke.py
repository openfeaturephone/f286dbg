"""phone_smoke.py - non-destructive checks of a running f286dbg agent over USB.

    python phone_smoke.py [--kernel kernel.bin] [--mmi-app mmi_app.bin] [--shot shot.png]

Reads are compared against your own partition dumps when given (kernel.bin is loaded at VA
0x80a05e00, mmi_app at 0x88000000). Only memory the agent itself allocates is written, and it
is freed again. It also calls GUI_IntersectRect once and takes a screenshot if --shot is set.
"""
import argparse
import struct
import time

from f286 import Phone

KERNEL_VA = 0x80a05e00
MMI_VA = 0x88000000
GUI_INTERSECT_RECT = 0x887eab16      # BOOLEAN (GUI_RECT_T *out, GUI_RECT_T a, GUI_RECT_T b), Thumb


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel", help="your kernel partition dump, to compare reads against")
    ap.add_argument("--mmi-app", help="your mmi_app dump (stock or patched), to compare reads against")
    ap.add_argument("--shot", help="also save a screenshot to this PNG (needs numpy + Pillow)")
    a = ap.parse_args()

    p = Phone("usb")
    print("ping", p.ping())
    if a.kernel:
        kb = open(a.kernel, "rb").read()
        t = time.time()
        d = p.read(KERNEL_VA + 0x1a200, 0x10000)
        dt = time.time() - t
        diff = sum(x != y for x, y in zip(d, kb[0x1a200:0x1a200 + 0x10000]))
        print(f"kernel 64 KB @{KERNEL_VA + 0x1a200:#x}: {diff} bytes differ, {dt:.2f} s = {64 / dt:.0f} KB/s")
    if a.mmi_app:
        mb = open(a.mmi_app, "rb").read()
        d = p.read(MMI_VA + 0x100000, 0x800)
        print("mmi_app 2 KB @0x88100000 match:", d == mb[0x100000:0x100800])
    print("api, base, size", [hex(x) for x in p.getapi()])
    buf = p.alloc(4096)
    print("alloc", hex(buf))
    pat = bytes(range(256)) * 16
    p.write(buf, pat)
    print("write/read 4 KB:", p.read(buf, 4096) == pat)
    out = p.alloc(16)
    # the rects go by value: a = r1:r2, b = r3 + one stack word, hence call8
    a0, a1 = struct.unpack("<II", struct.pack("<4h", 0, 0, 100, 100))
    b0, b1 = struct.unpack("<II", struct.pack("<4h", 50, 60, 200, 200))
    rv = p.call8(GUI_INTERSECT_RECT | 1, out, a0, a1, b0, b1)[0]
    print("IntersectRect ->", rv, struct.unpack("<4h", p.read(out, 8)), "(expect 1 (50, 60, 100, 100))")
    for x in (buf, out):
        p.free(x)
    if a.shot:
        p.screenshot(a.shot)
        print("screenshot ->", a.shot)
    print("ping again", p.ping())


if __name__ == "__main__":
    main()
