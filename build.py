"""build.py - build the f286dbg agent and the dead-man app for the Ruio F286.

    python build.py [--out DIR] [--sdk PATH_TO_MOCOR_SDK]

Outputs (default out/):
    f286dbg.bin        the agent for a manual start (Apps -> Facebook, or f286.py run); priority 1
    f286dbg_embed.bin  the same agent built with priority 0, for mocor-sdk's build.py --autostart
    deadman.bin        PMIC watchdog dead-man switch (read the README before using it)

The headers in sdk/ are copies from mocor-sdk (API v3). --sdk builds against a mocor-sdk
checkout instead (its include/, rt/rt.c and ld/app.ld).
Needs: python -m pip install ziglang pyelftools
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP_MAGIC = 0x50414632
# zig cc ignores -mno-unaligned-access; this LLVM feature is what actually stops unaligned
# loads/stores, and the kernel resets the phone on one (SCTLR.A is set).
STRICT_ALIGN = ["-Xclang", "-target-feature", "-Xclang", "+strict-align"]
CFLAGS = ["-target", "thumb-freestanding-eabi", "-mcpu=cortex_a7", "-mthumb", "-Os",
          "-ffreestanding", "-fno-builtin", "-nostdlib", "-fno-stack-protector",
          "-mno-unaligned-access", "-ffunction-sections", "-fdata-sections", "-fno-unwind-tables",
          "-fno-asynchronous-unwind-tables", "-Wall", "-Wextra", "-Werror", "-g", "-fropi"] + STRICT_ALIGN
# (name, source, extra flags)
TARGETS = [("f286dbg", "f286dbg.c", []),
           ("f286dbg_embed", "f286dbg.c", ["-DF286_PRIORITY=0u"]),
           ("deadman", "deadman.c", [])]


def zig(*args):
    r = subprocess.run([sys.executable, "-m", "ziglang", *args], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"FAILED: {' '.join(args)}\n{r.stdout}{r.stderr}")


def entry_of(elf):
    from elftools.elf.elffile import ELFFile
    with open(elf, "rb") as f:
        for sec in ELFFile(f).iter_sections():
            if sec.header.sh_type == "SHT_SYMTAB":
                for s in sec.get_symbol_by_name("f286_main") or []:
                    return s["st_value"] & ~1
    sys.exit(f"f286_main not found in {elf}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(HERE, "out"))
    ap.add_argument("--sdk", help="mocor-sdk checkout to take the headers, rt.c and app.ld from")
    a = ap.parse_args()
    if a.sdk:
        inc, rt, ld = (os.path.join(a.sdk, "include"), os.path.join(a.sdk, "rt", "rt.c"),
                       os.path.join(a.sdk, "ld", "app.ld"))
    else:
        sdk = os.path.join(HERE, "sdk")
        inc, rt, ld = sdk, os.path.join(sdk, "rt.c"), os.path.join(sdk, "app.ld")
    os.makedirs(a.out, exist_ok=True)
    for name, src, extra in TARGETS:
        elf = os.path.join(a.out, name + ".elf")
        binf = os.path.join(a.out, name + ".bin")
        zig("cc", *CFLAGS, "-I", inc, *extra, "-T", ld, "-Wl,--gc-sections", "-Wl,-e,f286_main",
            "-o", elf, os.path.join(HERE, "src", src), rt)
        zig("objcopy", "-O", "binary", elf, binf)
        blob = open(binf, "rb").read()
        if entry_of(elf) != 8 or struct.unpack_from("<II", blob) != (APP_MAGIC, 8):
            sys.exit(f"{name}: header/entry wrong")
        print(f"{name}.bin  {len(blob)} bytes  sha256 {hashlib.sha256(blob).hexdigest()[:16]}")


if __name__ == "__main__":
    main()
