"""f286.py - PC client for the on-phone debug agent (src/f286dbg.c) on the Ruio F286.

Protocol (little endian), see PROTOCOL.md:
  request  'F' 'D' cmd seq len16 payload[len]
  reply    'F' 'D' cmd|0x80 seq len16 status payload     (len counts status + payload)
  log      'F' 'D' 0x7f 0 len16 text                      (unsolicited)

  python f286.py [--emu HOST:PORT] ping
  python f286.py read ADDR LEN            hexdump
  python f286.py write ADDR HEX
  python f286.py call FN [R0 R1 R2 R3]    FN with its Thumb bit as it should be called
  python f286.py alloc LEN | free ADDR | cache ADDR LEN | key WIN MSG | getapi | exit
  python f286.py run APP.bin              upload to heap, flush caches, call its entry like the loader
  python f286.py shot out.png             compose the MMI layers read from RAM; `layers` lists them
  python f286.py ls "D:\\*" | get PATH OUT | put SRC PATH | rm PATH | mkdir PATH
  python f286.py press KEY | keypad KEY down|up | wake | reboot | deadman on|off|status
Transports: default 'usb' (pyusb/WinUSB, 1782:000B interface 0, EP 0x02 OUT / 0x81 IN, after CDC
SET_LINE_CODING + SET_CONTROL_LINE_STATE(3)); --emu HOST:PORT = the same frames over TCP
(for an emulator or a bridge that forwards UCOM channel 0).
"""
from pathlib import Path
import argparse
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
VID, PID = 0x1782, 0x000B
EP_OUT, EP_IN = 0x02, 0x81
MAX_PAY = 0x7F0
CHUNK = 0x7E0
CMD = dict(PING=1, READ=2, WRITE=3, CALL=4, ALLOC=5, FREE=6, CACHE=7, KEY=8, EXIT=9, GETAPI=10,
           CALL8=11, WAKE=12, KEYPAD=13, USBMODE=14, LOG=0x7F)
# SCI_VK codes for CMD_KEYPAD/WAKE (the keymap the KPD driver uses). RED/power is not in the
# matrix (it is an EIC line, not KPD) and can't be injected this way.
VK = {"UP": 4, "DOWN": 5, "LEFT": 6, "RIGHT": 7, "GREEN": 1, "CALL": 1,
      "LSOFT": 8, "RSOFT": 9, "OK": 0x0D, "STAR": 0x2A, "*": 0x2A, "HASH": 0x23, "#": 0x23}
VK.update({str(d): 0x30 + d for d in range(10)})
SFS_CREATE, SFS_READ, SFS_WRITE, SFS_CLOSE = 0x88085b16, 0x88086562, 0x880865ba, 0x88086530   # mocor-sdk docs/api-addresses.md
SFS_FIND_FIRST, SFS_FIND_NEXT, SFS_FIND_CLOSE = 0x88086754, 0x88086a54, 0x88086a92
FIND_DATA_SZ = 0x400   # 0x220 overflowed (long name + short name after it): heap corruption -> reset
SFS_DELETE, SFS_MKDIR = 0x88085daa, 0x88085cc8   # SFS_DeleteFileInternal(path, ov), SFS_CreateDirectory (named by matching a Nokia build)
MMK_IS_OPEN_WIN, MMK_CLOSE_WIN = 0x883fc3a4, 0x883f9ab2
DEADMAN_WIN = 0x00f28640                          # src/deadman.c WIN_ID
# deadman.bin as built by ../build.py; override with the F286_DEADMAN_BIN environment variable
DEADMAN_BIN = os.environ.get("F286_DEADMAN_BIN", str(Path(HERE).parent / "out" / "deadman.bin"))
PMIC_WDG_CONFIG = 0x8136f3f0   # kernel PMIC watchdog config(cfg*{run, mode, load}), Thumb; see reboot()
MAIN_LCD_BUF_PTR = 0x817ce2c8   # GUI main LCD layer: buffer pointer (found via IDLE2 RAM scan + phone read)
# MMI layer manager (mmi_app, see PROTOCOL.md "Screenshots"): 16 z-order slots of 16 bytes, each naming a
# layer id; layer objects are 0x60 bytes in a static table.  Persistent (the kernel's per-flush copies
# at 0x818fd108 and the LCDC registers are transient).
LAYER_SLOTS = 0x818ac6b4       # 16 x {u32 inuse, u32 ?, u32 layer id (0xffff = none), u32 ?}, bottom -> top
LAYER_OBJ0 = 0x818ac874        # object of id 0 (main layer); id N (6..0x68) is at LAYER_OBJ0 + 0x60 * (N - 5)
LAYER_OBJ_SZ = 0x60


class AgentError(Exception):
    pass


def ram_layers(read):
    """Enumerate the MMI layers from RAM only. `read(addr, n) -> bytes`. Returns a list of dicts, bottom to top
    (slot order), including invisible ones (vis False) so callers can print them.
    Object fields: +0x18 source buffer (the layer the UI draws into), +0x1c bytes, +0x20/+0x22 x/y,
    +0x24/+0x26 w/h, +0x28 format (0 or 9 = ARGB8888, 2..8 = RGB565; the UI uses 3), +0x2c flags (bit1 =
    shown), +0x2e used, +0x34 hw copy buffer (+0xff & ~0xff), +0x40 layer alpha."""
    slots = struct.unpack("<64I", read(LAYER_SLOTS, 256))
    out = []
    for i in range(16):
        inuse, _, lid, _ = slots[i * 4:i * 4 + 4]
        if lid == 0xFFFF:
            continue
        if lid == 0:
            oa = LAYER_OBJ0
        elif 5 < lid < 0x69:
            oa = LAYER_OBJ0 + LAYER_OBJ_SZ * (lid - 5)
        else:
            continue
        o = read(oa, LAYER_OBJ_SZ)
        w = struct.unpack("<24I", o)
        src, nbytes, xy, wh, fmt, fl = w[6], w[7], w[8], w[9], w[10], w[11]
        L = dict(slot=i, id=lid, obj=oa, inuse=inuse, src=src, bytes=nbytes, x=xy & 0xFFFF, y=xy >> 16,
                 w=wh & 0xFFFF, h=wh >> 16, fmt=fmt, flags=fl & 0xFF, used=(fl >> 16) & 0xFF,
                 hw=(w[13] + 0xFF) & ~0xFF, alpha=w[16] & 0xFF)
        L["bpp"] = 4 if fmt in (0, 9) else (2 if 2 <= fmt <= 8 else (4 if nbytes >= L["w"] * L["h"] * 4 else 0))
        L["vis"] = bool(inuse and src and (L["flags"] & 2) and 0 < L["w"] <= 1024 and 0 < L["h"] <= 1024
                        and L["bpp"] and src >= 0x80000000)
        out.append(L)
    return out


def ram_compose(read, layers, W=240, H=320):
    """Compose visible layers (bottom to top) the way the LCDC does: RGB565 opaque, ARGB8888
    alpha-blended (pixel alpha x layer alpha). -> (H, W, 3) uint8 ndarray."""
    import numpy as np
    canvas = np.zeros((H, W, 3), np.float32)
    for L in layers:
        if not L["vis"]:
            continue
        w, h, x0, y0 = L["w"], L["h"], L["x"], L["y"]
        raw = read(L["src"], w * h * L["bpp"])
        if L["bpp"] == 4:
            px = np.frombuffer(raw, "<u4").reshape(h, w)
            rgb = np.stack([(px >> 16) & 255, (px >> 8) & 255, px & 255], -1).astype(np.float32)
            al = (px >> 24).astype(np.float32)[..., None] / 255 * (L["alpha"] / 255)
        else:
            px = np.frombuffer(raw, "<u2").reshape(h, w).astype(np.uint16)
            r, g, b = (px >> 11) & 31, (px >> 5) & 63, px & 31
            rgb = np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)
            al = None
        cw, ch = min(w, W - x0), min(h, H - y0)
        if cw <= 0 or ch <= 0:
            continue
        src = rgb[:ch, :cw]
        if al is None:
            canvas[y0:y0 + ch, x0:x0 + cw] = src
        else:
            a = al[:ch, :cw]
            canvas[y0:y0 + ch, x0:x0 + cw] = src * a + canvas[y0:y0 + ch, x0:x0 + cw] * (1 - a)
    return np.clip(canvas + 0.5, 0, 255).astype(np.uint8)


class _UsbTransport:
    def __init__(self, timeout):
        import usb.core
        import usb.util
        import usb.backend.libusb1
        import libusb_package
        self.usb = usb
        be = usb.backend.libusb1.get_backend(find_library=libusb_package.find_library)
        self.dev = usb.core.find(idVendor=VID, idProduct=PID, backend=be)
        if self.dev is None:
            raise AgentError(f"no {VID:04x}:{PID:04x} device (phone: run f286dbg, wait ~6 s for 'connected')")
        usb.util.claim_interface(self.dev, 0)
        for req, val, data in [(0x20, 0, struct.pack("<IBBB", 115200, 0, 0, 8)), (0x22, 3, None)]:
            try:
                self.dev.ctrl_transfer(0x21, req, val, 0, data, timeout=500)
            except usb.core.USBError:
                pass

    def write(self, data):
        self.dev.write(EP_OUT, data, timeout=2000)

    def read(self, timeout):
        try:
            return bytes(self.dev.read(EP_IN, 4096, timeout=max(1, int(timeout * 1000))))
        except self.usb.core.USBTimeoutError:
            return b""

    def close(self):
        self.usb.util.dispose_resources(self.dev)


class _TcpTransport:
    def __init__(self, hostport, timeout):
        import socket
        host, _, port = hostport.rpartition(":")
        self.sock = socket.create_connection((host or "127.0.0.1", int(port)), timeout=30)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def write(self, data):
        self.sock.sendall(data)

    def read(self, timeout):
        self.sock.settimeout(max(0.001, timeout))
        try:
            d = self.sock.recv(65536)
        except OSError:
            return b""
        if not d:
            raise AgentError("emulator closed the connection")
        return d

    def close(self):
        self.sock.close()


class Phone:
    def __init__(self, transport="usb", addr=None, timeout=None, on_log=None):
        self.timeout = timeout if timeout is not None else (2.0 if transport == "usb" else 90.0)
        self.t = _UsbTransport(self.timeout) if transport == "usb" else _TcpTransport(addr or "127.0.0.1:5555", self.timeout)
        self.seq = 0
        self.buf = bytearray()
        self.logs = []
        self.on_log = on_log or (lambda s: print("[phone]", s, file=sys.stderr))
        self.api = None
        self.panel = None
        if transport == "usb":
            self._drain(0.3)

    # ---- framing ----
    def _drain(self, secs):
        end = time.time() + secs
        while time.time() < end:
            self.buf += self.t.read(0.05)
        self._parse()
        self.buf.clear()

    def _parse(self):
        """-> list of (cmd, seq, status_or_None, payload) for complete frames; consumes them"""
        out = []
        b = self.buf
        while True:
            i = b.find(b"FD")
            if i < 0:
                del b[:max(0, len(b) - 1)]      # keep a possible lone 'F'
                break
            del b[:i]
            if len(b) < 6:
                break
            ln = b[4] | (b[5] << 8)
            if ln > MAX_PAY + 1:
                del b[:2]
                continue
            if len(b) < 6 + ln:
                break
            cmd, seq, body = b[2], b[3], bytes(b[6:6 + ln])
            del b[:6 + ln]
            if cmd == CMD["LOG"]:
                txt = body.decode("latin1")
                self.logs.append(txt)
                self.on_log(txt)
            elif cmd & 0x80 and body:
                out.append((cmd & 0x7F, seq, body[0], body[1:]))
        return out

    def request(self, cmd, payload=b"", timeout=None):
        """-> (status, payload); raises AgentError on timeout or nonzero status via the helpers"""
        self.seq = (self.seq + 1) & 0xFF
        seq = self.seq
        frame = b"FD" + bytes([cmd, seq]) + struct.pack("<H", len(payload)) + payload
        if len(frame) % 64 == 0:
            frame += b"\x00"                  # never end on a full USB packet (no ZLP)
        self.t.write(frame)
        end = time.time() + (timeout if timeout is not None else self.timeout)
        while True:
            for c, s, st, pl in self._parse():
                if c == cmd and s == seq:
                    return st, pl
            left = end - time.time()
            if left <= 0:
                raise AgentError(f"timeout waiting for reply to cmd {cmd:#x} seq {seq}")
            self.buf += self.t.read(min(left, 0.5))

    def _ok(self, cmd, payload=b"", timeout=None):
        st, pl = self.request(cmd, payload, timeout)
        if st:
            raise AgentError(f"cmd {cmd:#x} failed: status {st:#x}")
        return pl

    # ---- commands ----
    def ping(self):
        pl = self._ok(CMD["PING"])
        proto, api_ver, ms = struct.unpack("<HII", pl)
        return dict(proto=proto, api_version=api_ver, ms=ms)

    def read(self, addr, n):
        out = bytearray()
        while n:
            k = min(n, MAX_PAY)
            out += self._ok(CMD["READ"], struct.pack("<IH", addr, k))
            addr += k
            n -= k
        return bytes(out)

    def write(self, addr, data):
        for i in range(0, len(data), CHUNK):
            self._ok(CMD["WRITE"], struct.pack("<I", addr + i) + data[i:i + CHUNK])

    def call(self, fn, r0=0, r1=0, r2=0, r3=0, timeout=None):
        pl = self._ok(CMD["CALL"], struct.pack("<5I", fn, r0, r1, r2, r3), timeout)
        return struct.unpack("<II", pl)

    def call8(self, fn, *args, timeout=None):
        """CMD_CALL8 (v2): like call(), but up to 8 more arguments passed on the stack
        (12 total). Missing trailing args are 0. fn is called as a 12-arg AAPCS function
        regardless of how many the real callee reads, so this is safe for any function
        needing <= 12 words of arguments."""
        args = (list(args) + [0] * 12)[:12]
        pl = self._ok(CMD["CALL8"], struct.pack("<13I", fn, *args), timeout)
        return struct.unpack("<II", pl)

    def wake(self):
        """CMD_WAKE (v2): injects a real RIGHT key press+release through the KPD service
        path, meant to turn the backlight/screen on before a screenshot (not checked on the
        phone). RIGHT does nothing at idle (UP would open the main menu), so it has no side
        effect of its own there."""
        self._ok(CMD["WAKE"])

    def keypad(self, key, down):
        """CMD_KEYPAD (v2): inject one edge (down=True press, False release) of `key`
        (a name from VK, e.g. 'OK', '5', 'LSOFT') through the real KPD dispatch path
        (Q_KPDSVR -> Q_P_APP -> MMIDEFAULT_ConvertKeyCode), so it reaches the focused
        window exactly like a hardware key. Unlike key(), this does not need a window
        id. Call with down=True then down=False for a normal press; the agent does not
        hold between the two (no sleeping inside its tick), so space the two calls
        yourself for a held key."""
        vk = VK[key.upper()] if isinstance(key, str) else key
        self._ok(CMD["KEYPAD"], struct.pack("<BB", vk, 1 if down else 0))

    def press_key(self, key, hold_s=0.08):
        """keypad(key, True), sleep hold_s, keypad(key, False): a full press+release."""
        self.keypad(key, True)
        time.sleep(hold_s)
        self.keypad(key, False)

    def usbmode(self, mode):
        """CMD_USBMODE (v2): 'serial' (UCOM type 4, the agent's own mode - this call
        then reconnects ~5 s later like a fresh boot) or 'udisk' (USB_START(1); the type
        number for plain U Disk was never confirmed, and this is a one-way switch: the
        serial link does not come back without a reboot or hands on the phone. Never tried
        on the phone; the command line only offers 'serial'). The switch happens a tick
        after this call returns, once the ack has flushed."""
        m = {"serial": 0, "udisk": 1}[mode]
        self._ok(CMD["USBMODE"], bytes([m]))

    # ---- files on the phone (D:\ internal, E:\ SD) through the stock SFS calls; no U Disk needed ----
    # SFS_FIND_DATA_T (read live from the phone): dates 0x00-0x13, attr u16 @0x14 (0x10 = dir), size u32 @0x18,
    # UCS-2 name @0x1c.
    def _wstr(self, s):
        a = self.alloc(2 * len(s) + 2)
        self.write(a, s.encode("utf-16-le") + b"\0\0")
        return a

    def fs_ls(self, pattern="D:\\*"):
        """-> [(name, size, is_dir)]"""
        pat, fd = self._wstr(pattern), self.alloc(FIND_DATA_SZ)
        out = []
        try:
            self.write(fd, b"\0" * FIND_DATA_SZ)
            h = self.call(SFS_FIND_FIRST | 1, pat, fd)[0]
            if h in (0, 0xFFFFFFFF):
                return out
            while True:
                d = self.read(fd, 0x220)
                if len(out) >= 4096:
                    break
                attr, size = struct.unpack_from("<HxxI", d, 0x14)
                name = d[0x1c:].decode("utf-16-le").split("\0", 1)[0]
                out.append((name, size, bool(attr & 0x10)))
                if self.call(SFS_FIND_NEXT | 1, h, fd)[0] != 0:
                    break
            self.call(SFS_FIND_CLOSE | 1, h)
        finally:
            self.free(pat)
            self.free(fd)
        return out

    def fs_get(self, path, chunk=0x2000):
        p, buf, n = self._wstr(path), self.alloc(chunk), self.alloc(8)
        try:
            h = self.call(SFS_CREATE | 1, p, 0x31, 0, 0)[0]              # OPEN_EXISTING | READ
            if h == 0:
                raise AgentError(f"cannot open {path}")
            data = bytearray()
            try:
                while True:
                    self.write(n, b"\0" * 4)
                    self.call8(SFS_READ | 1, h, buf, chunk, n, 0)
                    got = struct.unpack("<I", self.read(n, 4))[0]
                    if got:
                        data += self.read(buf, got)
                    if got < chunk:
                        return bytes(data)
            finally:
                self.call(SFS_CLOSE | 1, h)
        finally:
            for a in (p, buf, n):
                self.free(a)

    def fs_put(self, path, data, chunk=0x2000):
        p, buf, n = self._wstr(path), self.alloc(chunk), self.alloc(8)
        try:
            h = self.call(SFS_CREATE | 1, p, 0x22, 0, 0)[0]              # CREATE_ALWAYS | WRITE
            if h == 0:
                raise AgentError(f"cannot create {path}")
            try:
                for off in range(0, len(data), chunk):
                    part = data[off:off + chunk]
                    self.write(buf, part)
                    self.write(n, b"\0" * 4)
                    self.call8(SFS_WRITE | 1, h, buf, len(part), n, 0)
                    if struct.unpack("<I", self.read(n, 4))[0] != len(part):
                        raise AgentError(f"short write at {off:#x} in {path}")
            finally:
                self.call(SFS_CLOSE | 1, h)
        finally:
            for a in (p, buf, n):
                self.free(a)

    def fs_rm(self, path):
        p = self._wstr(path)
        try:
            return self.call(SFS_DELETE | 1, p, 0)[0]                     # 0 = ok
        finally:
            self.free(p)

    def fs_mkdir(self, path):
        p = self._wstr(path)
        try:
            return self.call(SFS_MKDIR | 1, p, 0)[0]
        finally:
            self.free(p)

    # ---- dead-man switch (src/deadman.c). See the README incident before using it. ----
    def deadman(self, on=True):
        """on: upload+run deadman.bin (RAM only, gone after a reset): feeds the PMIC watchdog every 1 s
        with a 30 s timeout, so any hang of the MMI task resets the phone and the agent returns (~46 s).
        off: close its window, which disarms the watchdog. -> True if it is running afterwards."""
        if on and not self.call(MMK_IS_OPEN_WIN | 1, DEADMAN_WIN)[0]:
            self.upload_and_run(DEADMAN_BIN)
        elif not on and self.call(MMK_IS_OPEN_WIN | 1, DEADMAN_WIN)[0]:
            self.call(MMK_CLOSE_WIN | 1, DEADMAN_WIN)
        return bool(self.call(MMK_IS_OPEN_WIN | 1, DEADMAN_WIN)[0])

    def reboot(self):
        """Full reset through the stock PMIC (SC2720) watchdog driver, phone-verified:
        0x8136f3f0(&cfg) with cfg = {run 1, mode 0 = reset, load 0x666 = 50 ms at 32 kHz} unlocks the
        WDG (ADI 0x40608060 <- 0xE551), sets CTRL 0x48 bits 0|2|3, loads 0x40/0x44 and sets RUN. The
        reply comes back before the 50 ms expire; an autostarted agent was back after about 18 s
        in my tests. Never reset by faulting (README, "Safety")."""
        cfg = self.alloc(16)
        self.write(cfg, struct.pack("<III", 1, 0, 0x666))
        return self.call(PMIC_WDG_CONFIG | 1, cfg, timeout=3)

    def alloc(self, n):
        return struct.unpack("<I", self._ok(CMD["ALLOC"], struct.pack("<I", n)))[0]

    def free(self, addr):
        self._ok(CMD["FREE"], struct.pack("<I", addr))

    def cache(self, addr, n):
        self._ok(CMD["CACHE"], struct.pack("<II", addr, n))

    def key(self, msg, win_id=0):
        """post msg (e.g. 0xfa1b = OK pressed, see f286_api.h) to win_id (0 = the agent's own window)"""
        return struct.unpack("<I", self._ok(CMD["KEY"], struct.pack("<II", win_id, msg)))[0]

    def getapi(self):
        api, base, size = struct.unpack("<III", self._ok(CMD["GETAPI"]))
        self.api = api
        return api, base, size

    def exit(self):
        self._ok(CMD["EXIT"])

    def close(self):
        self.t.close()

    # ---- compound ----
    def upload_and_run(self, path, run=True, verify=True):
        """Load an app .bin exactly like the stub loader: heap copy, D-cache clean + I-cache
        invalidate, then entry(api, base, size) with entry = base + header[1] + 1 (Thumb)."""
        data = open(path, "rb").read()
        if len(data) < 8:
            raise AgentError("file too small")
        magic, off = struct.unpack_from("<II", data)
        if magic != 0x50414632 or off >= len(data):
            raise AgentError("not an F286 app (header magic/entry offset)")
        api = self.api or self.getapi()[0]
        addr = self.alloc(len(data))
        self.write(addr, data)
        if verify and self.read(addr, len(data)) != data:
            raise AgentError(f"readback mismatch at {addr:#x}")
        self.cache(addr, len(data))
        r = self.call(addr + off + 1, api, addr, len(data)) if run else None
        return addr, r

    def screenshot(self, path, layers=None):
        """True composed screen from RAM only (no LCDC registers): enumerate the MMI layer manager
        (ram_layers), read every visible layer's source buffer, compose them, write a PNG.
        Falls back to screenshot_buf() if no layer is visible. Returns (w, h, visible layers).
        ~3-8 s over USB (a full-screen RGB565 layer is 150 KB at ~50 KB/s)."""
        from PIL import Image
        ls = layers if layers is not None else ram_layers(self.read)
        if not any(L["vis"] for L in ls):
            return (240, 320, [self.screenshot_buf(path)])
        rgb = ram_compose(self.read, ls)
        Image.fromarray(rgb, "RGB").save(path)
        return 240, 320, [L for L in ls if L["vis"]]

    def screenshot_buf(self, path):
        """Old single-buffer fallback: GUI LCD layer table at 0x817ce2b8 (+0x10 = buffer ptr, 240x320
        RGB565). Right for single-layer screens only (misses ARGB overlay layers)."""
        from PIL import Image
        buf = struct.unpack("<I", self.read(MAIN_LCD_BUF_PTR, 4))[0]
        d = self.read(buf, 240 * 320 * 2)
        px = struct.unpack(f"<{240 * 320}H", d)
        rgb = b"".join(bytes(((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)) for v in px)
        Image.frombytes("RGB", (240, 320), rgb).save(path)
        return dict(src=buf, name="legacy-buffer")


def _hexdump(addr, data):
    for i in range(0, len(data), 16):
        c = data[i:i + 16]
        print(f"{addr + i:08x}  {c.hex(' '):<47}  {''.join(chr(b) if 32 <= b < 127 else '.' for b in c)}")


def _n(s):
    return int(s, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emu", metavar="HOST:PORT", help="speak the protocol over TCP instead of USB")
    ap.add_argument("--timeout", type=float)
    sp = ap.add_subparsers(dest="cmd", required=True)
    sp.add_parser("ping")
    p = sp.add_parser("read"); p.add_argument("addr", type=_n); p.add_argument("len", type=_n)
    p = sp.add_parser("write"); p.add_argument("addr", type=_n); p.add_argument("hex")
    p = sp.add_parser("call"); p.add_argument("fn", type=_n); p.add_argument("args", nargs="*", type=_n)
    p = sp.add_parser("alloc"); p.add_argument("len", type=_n)
    p = sp.add_parser("free"); p.add_argument("addr", type=_n)
    p = sp.add_parser("cache"); p.add_argument("addr", type=_n); p.add_argument("len", type=_n)
    p = sp.add_parser("key"); p.add_argument("win", type=_n); p.add_argument("msg", type=_n)
    sp.add_parser("getapi")
    sp.add_parser("exit")
    p = sp.add_parser("run"); p.add_argument("app")
    p = sp.add_parser("shot"); p.add_argument("out")
    sp.add_parser("layers")
    p = sp.add_parser("call8"); p.add_argument("fn", type=_n); p.add_argument("args", nargs="*", type=_n)
    sp.add_parser("wake")
    sp.add_parser("reboot")
    p = sp.add_parser("deadman"); p.add_argument("state", choices=["on", "off", "status"])
    p = sp.add_parser("ls"); p.add_argument("pattern", nargs="?", default="D:\\*")
    p = sp.add_parser("get"); p.add_argument("path"); p.add_argument("out")
    p = sp.add_parser("put"); p.add_argument("src"); p.add_argument("path")
    p = sp.add_parser("rm"); p.add_argument("path")
    p = sp.add_parser("mkdir"); p.add_argument("path")
    p = sp.add_parser("keypad"); p.add_argument("key"); p.add_argument("edge", choices=["down", "up"])
    p = sp.add_parser("press"); p.add_argument("key"); p.add_argument("hold_s", type=float, nargs="?", default=0.08)
    p = sp.add_parser("usbmode"); p.add_argument("mode", choices=["serial"])   # udisk: see Phone.usbmode
    a = ap.parse_args()
    ph = Phone("tcp" if a.emu else "usb", a.emu, a.timeout)
    try:
        if a.cmd == "ping":
            print(ph.ping())
        elif a.cmd == "read":
            _hexdump(a.addr, ph.read(a.addr, a.len))
        elif a.cmd == "write":
            ph.write(a.addr, bytes.fromhex(a.hex))
            print("ok")
        elif a.cmd == "call":
            r0, r1 = ph.call(a.fn, *(a.args + [0] * 4)[:4])
            print(f"r0={r0:#x} r1={r1:#x}")
        elif a.cmd == "alloc":
            print(hex(ph.alloc(a.len)))
        elif a.cmd == "free":
            ph.free(a.addr)
        elif a.cmd == "cache":
            ph.cache(a.addr, a.len)
        elif a.cmd == "key":
            print(ph.key(a.msg, a.win))
        elif a.cmd == "getapi":
            print("api=%#x base=%#x size=%#x" % ph.getapi())
        elif a.cmd == "exit":
            ph.exit()
        elif a.cmd == "run":
            addr, r = ph.upload_and_run(a.app)
            print(f"loaded at {addr:#x}, entry returned r0={r[0]:#x}")
        elif a.cmd == "shot":
            w, h, ls = ph.screenshot(a.out)
            print(f"{a.out}: {w}x{h}, layers " + ", ".join(
                f"id{L['id']}@{L['src']:#x} {L['w']}x{L['h']}+{L['x']},{L['y']} fmt{L['fmt']}" if "id" in L else L["name"] for L in ls))
        elif a.cmd == "layers":
            for L in ram_layers(ph.read):
                print(" ".join(f"{k}={v:#x}" if isinstance(v, int) and k in ("obj", "src", "hw") else f"{k}={v}" for k, v in L.items()))
        elif a.cmd == "call8":
            r0, r1 = ph.call8(a.fn, *a.args)
            print(f"r0={r0:#x} r1={r1:#x}")
        elif a.cmd == "wake":
            ph.wake()
            print("ok")
        elif a.cmd == "deadman":
            if a.state == "status":
                print("on" if ph.call(MMK_IS_OPEN_WIN | 1, DEADMAN_WIN)[0] else "off")
            else:
                print("on" if ph.deadman(a.state == "on") else "off")
        elif a.cmd == "ls":
            for name, size, is_dir in ph.fs_ls(a.pattern):
                print(f"{'<DIR>' if is_dir else size:>10}  {name}")
        elif a.cmd == "get":
            data = ph.fs_get(a.path)
            open(a.out, "wb").write(data)
            print(f"{a.path} -> {a.out} ({len(data)} bytes)")
        elif a.cmd == "put":
            data = open(a.src, "rb").read()
            ph.fs_put(a.path, data)
            print(f"{a.src} -> {a.path} ({len(data)} bytes)")
        elif a.cmd == "rm":
            print("ok" if ph.fs_rm(a.path) == 0 else "failed")
        elif a.cmd == "mkdir":
            print("ok" if ph.fs_mkdir(a.path) == 0 else "failed")
        elif a.cmd == "reboot":
            ph.reboot()
            print("reboot armed (PMIC watchdog, 50 ms)")
        elif a.cmd == "keypad":
            ph.keypad(a.key, a.edge == "down")
            print("ok")
        elif a.cmd == "press":
            ph.press_key(a.key, a.hold_s)
            print("ok")
        elif a.cmd == "usbmode":
            ph.usbmode(a.mode)
            print("ok (switching)")
    finally:
        ph.close()


if __name__ == "__main__":
    main()
