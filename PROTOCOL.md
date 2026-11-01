# f286dbg wire protocol

The agent opens USB service type 4 on the phone, which enumerates as composite device 1782:000B
with four vendor bulk interfaces. Interface 0 (EP 0x02 OUT, EP 0x81 IN) is UCOM channel 0 and
carries this protocol. Interfaces 1-3 (EP 04/83, 06/85, 08/87) are UCOM channels 1-3; the agent
opens them too (it has to, see below) and discards whatever arrives on them.

Before the first frame the host sends the CDC requests SET_LINE_CODING (115200 8N1) and
SET_CONTROL_LINE_STATE(3) (DTR and RTS) on interface 0. I have always sent both and never tried
without them.

## Frames

All integers are little endian.

```
request  'F' 'D'  cmd u8   seq u8  len u16  payload[len]           len <= 0x7f0
reply    'F' 'D'  cmd|0x80 seq u8  len u16  status u8  payload      len counts status + payload
log      'F' 'D'  0x7f     0       len u16  text[len]               unsolicited, no status byte
```

- The reply echoes `seq`. The PC client increments it per request (mod 256) and drops replies
  that do not match.
- Frames may be split across USB packets or several may arrive in one. The agent reassembles in a
  6 KB buffer, resyncs on `FD` after garbage, and drops a header whose len is over 0x7f0.
- There are no zero-length packets on this link, so a transfer that ends exactly on a 64-byte
  boundary would leave the receiver waiting. Both sides therefore append one 0x00 byte to any
  frame whose total length is a multiple of 64. The resync skips it.
- Status: 0 ok, 1 failed (e.g. alloc returned NULL, key not in the keymap), 2 bad arguments,
  0xff unknown command.

## Commands

| cmd | name | request payload | reply payload |
|---|---|---|---|
| 0x01 | PING | - | proto u16 (2), API table version u32, timer ms u32 (counter at 0x4023000c) |
| 0x02 | READ | addr u32, len u16 (<= 0x7f0) | bytes |
| 0x03 | WRITE | addr u32, data | bytes written u16 |
| 0x04 | CALL | fn u32, r0..r3 u32 (missing = 0) | r0 u32, r1 u32 |
| 0x05 | ALLOC | len u32 | addr u32 (status 1 on NULL) |
| 0x06 | FREE | addr u32 | - |
| 0x07 | CACHE | addr u32, len u32 | - (D-cache clean then I-cache invalidate over the range) |
| 0x08 | KEY | win_id u32 (0 = the agent's window 0x00f28610), msg u32 | MMK_PostMsg result u32 |
| 0x09 | EXIT | - | - (reply first, then the channels close and the window closes) |
| 0x0a | GETAPI | - | API table pointer u32, agent image base u32, image size u32 |
| 0x0b | CALL8 | fn u32, r0..r11 u32 (missing = 0) | r0 u32, r1 u32 |
| 0x0c | WAKE | - | - (a RIGHT press and release through the keypad path) |
| 0x0d | KEYPAD | SCI_VK u8, edge u8 (1 down, 0 up) | - (status 1 if the key is not in the keymap) |
| 0x0e | USBMODE | mode u8 (0 serial, 1 U Disk) | - (the switch happens a tick later, after the reply is out) |
| 0x7f | log | agent to PC only | text: on connect, on clearing the boot counter |

Commands 0x01-0x0a are protocol version 1 and did not change in version 2, so a version 1 client
still works.

### Notes per command

**READ / WRITE** use the widest aligned access that fits: 32-bit where `addr & 3 == 0` and at least
4 bytes remain, then 16-bit, then bytes. MMIO therefore gets 32-bit accesses when aligned.
There is no address check. An unmapped address, or a register block whose clock is off, is a data
abort, and the kernel's abort handler only handles pager faults: anything else resets the phone.

**CALL** runs `fn` in the MMI task from the agent's timer tick. Bit 0 of `fn` selects the mode as
given: set for Thumb (`mmi_app` functions), clear for ARM (kernel veneers and kernel ARM code).
The 64-bit return is r0:r1; r1 only means something for 64-bit returns.

**CALL8** calls `fn` as a 12-argument AAPCS function: r0-r3 in registers and eight words on the
stack. A callee that takes fewer arguments ignores the rest, so this is safe for anything that
needs up to 12 words, including small structs passed by value (e.g. a `GUI_RECT_T` takes two words).

**KEY** posts a message to a window by id. There is no focused-window lookup, so you name the
window. Key messages are `0xfa00 | KEY` for a press (e.g. 0xfa02 = DOWN), see mocor-sdk
`include/f286_api.h`.

**KEYPAD** injects one edge of a real key through the kernel keypad service (0x81179d7e with a
`{u16 keymap index, u16 0x81 down / 0x80 up}` record), which posts it to the MMI task the same way
the keypad interrupt does. The focused window gets it like a hardware key, so this drives the
stock UI. The agent looks the SCI_VK code up in the 64-entry keymap the kernel uses (pointer at
0x812189dc). Red/power is not in the keypad matrix and cannot be sent this way. The agent never
sleeps inside a tick, so a press is two requests, down then up, with the delay on the PC side.

SCI_VK codes: UP 4, DOWN 5, LEFT 6, RIGHT 7, CALL 1, LSOFT 8, RSOFT 9, OK 0x0d, `*` 0x2a,
`#` 0x23, digits `'0'..'9'` (0x30..0x39).

**WAKE** presses RIGHT, which does nothing at idle (UP, DOWN and OK would open the main menu). It
is meant to turn the screen on before a screenshot. Whether it reliably lights the backlight I
have not checked.

**USBMODE** 0 restarts USB service 4 the same way the agent starts, and it reconnects about 5 s
later. 1 calls `USB_START(1)` for U Disk mode. The service number for plain U Disk was never
confirmed, and once the serial channels are closed nothing brings them back without a reboot or
hands on the phone. I have never run mode 1 on the phone; `f286.py` does not offer it.

## Agent side details

Bring-up order: `USB_STOP(current)`, `USB_START(4)`, then open UCOM channels 0-3 at once
(`UCOM_OPS` table at 0x814c8154: open, close, pump, read, write). The kernel starts a 5 s detect
timer that calls the callbacks of all four channels; if one is not open by then, it calls a
missing callback and the phone resets. Likewise all four are closed before the window goes away,
because the kernel keeps the callbacks.

TX is asynchronous. `UCOM_OPS[4]` (write) only stores the caller's buffer pointer and length in the
pipe's transfer descriptor and submits it, so the controller reads the buffer after `write()` has
returned. If the descriptor is still busy (`desc+0x20 != 0`) the write does nothing at all and still
returns `len`. So the agent keeps replies in a ring of 4 heap buffers of 0x800 bytes, has at most
one transfer in flight, starts the next only when the busy flag clears, never queues more than 3
(the buffer in flight is never refilled), and cleans the D-cache over each buffer after filling it.
The busy flag is found through `[0x817040d0 + 4*ch]` (pipe index) -> `[0x818fc800 + pipe*0x20]`
(descriptor) -> `+0x20`. If that pointer fails its range checks the agent falls back to a 3 ms gap
between writes. A transfer still busy after 1 s (host not reading) counts as done.

Timing: a one-shot 20 ms window timer while there is traffic (and for 3 s after the last byte),
1 s when the link is quiet. Each tick pumps RX, runs complete frames and sends replies, repeating
while data keeps arriving (up to 16 rounds, waiting up to 8 ms for the rest of a split frame).
Latency is about one tick plus transfer time; throughput was about 51 KB/s for READ on my phone.

## Screenshots (PC side, `Phone.screenshot`)

The LCD controller's layer registers read 0 between frames on the phone, and the kernel's per-flush
layer copies are reset right after each flush, so the client reads the MMI layer manager instead,
which persists:

| address | content |
|---|---|
| 0x818ac6b4 | 16 z-order slots of 16 bytes, bottom to top: `u32 inuse, u32 ?, u32 layer id (0xffff = empty), u32 ?` |
| 0x818ac874 + 0x60 * (id - 5) | layer object for id 6..0x68; id 0 (main layer) is at 0x818ac874 |

Layer object (0x60 bytes): +0x18 source buffer, +0x1c buffer bytes, +0x20/+0x22 x/y, +0x24/+0x26
w/h (u16), +0x28 format (0 or 9 = ARGB8888, 2..8 = RGB565; the UI uses 3), +0x2c flags (bit 1 =
shown), +0x40 layer alpha. Pitch = width. The client reads every shown layer and composes them
bottom to top: RGB565 opaque, ARGB8888 blended with pixel alpha times layer alpha. A full-screen
RGB565 layer is 150 KB, so a shot takes 3-8 s.

When the backlight goes off the MMI releases the overlay layers, and a shot then shows only the
base layer (status bar, title, softkeys). Wake the screen first and wait 1.5-3 s after a key before
taking a shot.

## Files (PC side, `fs_ls` / `fs_get` / `fs_put` / `fs_rm` / `fs_mkdir`)

These are built from ALLOC, WRITE, CALL/CALL8 and READ on the stock SFS functions: CreateFile
0x88085b16 (mode 0x31 read, 0x22 create/overwrite), ReadFile 0x88086562 and WriteFile 0x880865ba
(through CALL8 for the fifth argument), CloseFile 0x88086530, FindFirst/Next/Close 0x88086754 /
0x88086a54 / 0x88086a92, DeleteFile 0x88085daa(path, 0), CreateDirectory 0x88085cc8.
SFS_FIND_DATA_T: attributes u16 at +0x14 (0x10 = directory), size u32 at +0x18, UCS-2 name at
+0x1c. Allocate at least 0x400 bytes for it: 0x220 overflowed and reset the phone.
