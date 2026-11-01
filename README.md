# f286dbg

A debug agent that runs as an app on the stock MocorOS firmware of the Ruio F286 (Unisoc UMS9117 /
T107), and a PC client for it. The agent turns the phone's hidden USB serial channel into a
request/reply link, so from the PC you can read and write memory, call any firmware function,
allocate heap, upload and run apps, press keys, take screenshots, copy files to and from the phone,
and reboot it. No U Disk mode and no hands on the phone needed once it is set up.

It needs the app loader from [mocor-sdk](https://github.com/openfeaturephone/mocor-sdk) patched
into the phone's `mmi_app`. Wire protocol: [PROTOCOL.md](PROTOCOL.md).

## Building

```
pip install ziglang pyelftools
python build.py
```
This writes `out/f286dbg.bin` (manual start), `out/f286dbg_embed.bin` (the same agent at window
priority 0, for autostart) and `out/deadman.bin`. The headers in `sdk/` are copies from mocor-sdk;
`python build.py --sdk ../mocor-sdk` uses a checkout instead.

## Getting it onto the phone

Either start it by hand: copy `out/f286dbg.bin` to the SD card as `E:\apps\launcher.bin` and open
Apps -> Facebook. The screen shows "F286 debug agent" and "USB ch0 waiting", which turns green about
5 s after the agent started USB. Right soft key or red exits.

Or have it start at every boot, which is how I use it:
```
cd ../mocor-sdk
python build.py --stock dumps\mmi_app.bin --autostart ..\f286dbg\out\f286dbg_embed.bin
```
then flash that image (see the mocor-sdk README). With the cable in, the agent answers about
18-25 s after power-on. The stock "select USB application" popup still appears at boot; leave it
alone or pick Charging (an injected LSOFT does it: `f286.py press LSOFT`). Never pick U Disk: it
takes USB away from the serial link and nothing brings it back without a reboot.

The autostart can be stopped by holding any key through boot or by creating `D:\F286_NOAGENT`,
and it stops by itself after three boots in a row where the agent did not survive 60 s. Details in
mocor-sdk `docs/loader.md`.

## PC setup

```
pip install pyusb libusb-package          # numpy and pillow too, for screenshots
```
The phone in serial mode is 1782:000B, a composite device with four vendor interfaces. On Windows
it needs WinUSB on those interfaces: in Zadig, Options -> List All Devices, pick the 1782:000B
interface 0 entry (I did all four) and install WinUSB. That is a one-time step per USB id. I have
only used the client on Windows; on Linux pyusb should work as is, given permission on the device.

## Running it

From `tools/`:
```
python f286.py ping                          # {'proto': 2, 'api_version': 3, 'ms': ...}
python f286.py read 0x80a05e00 64            # hexdump
python f286.py write 0x82957000 deadbeef
python f286.py call8 0x887eab17 OUT A0 A1 B0 B1   # bit 0 set = Thumb
python f286.py run ../../mocor-sdk/out/uidemo.bin # alloc, write, read back, flush caches, call entry
python f286.py press DOWN                    # a real key press through the keypad driver
python f286.py wake
python f286.py shot screen.png               # composed from the MMI layers in RAM
python f286.py ls "D:\*"
python f286.py get "D:\some.file" some.file
python f286.py put local.bin "E:\apps\3.bin"
python f286.py rm "D:\F286_01_00000000.TXT"
python f286.py reboot                        # PMIC watchdog, back in about 18 s
```
As a library: `from f286 import Phone; p = Phone("usb"); p.read(addr, n); p.call8(fn, *args)`.
`phone_smoke.py` runs a set of non-destructive checks (pass `--kernel`/`--mmi-app` with your own
dumps to compare reads). `reboot_test.py N` reboots N times and times how long the agent takes to
come back.

Everything above has run on my phone except `mkdir`. Not tried there: EXIT, USBMODE, and whether
WAKE actually lights the backlight. One F286, one firmware.

## Safety

These each cost me a battery pull or a reset. Read them before pointing the tools at anything new.

- **Never reset the phone by faulting it.** I once wrote to a read-only `mmi_app` page to force a
  reset. A permission fault passes the kernel's pager test, the pager finds the page present and
  returns, and the store retries forever: the phone froze until the battery came out. Only other
  fault types (alignment, unmapped) reset it. Use `f286.py reboot`.
- **Reads and writes are not checked.** An unmapped address or an unclocked register block is a
  data abort, which resets the phone. When you read app state, take symbol offsets from the ELF of
  the build that is actually running, and range-check every pointer you follow (app heap is
  0x82000000..0x83000000). A stale offset from an earlier build once had me follow a garbage
  pointer straight into a reset.
- **Use a fresh process after a reboot.** When the phone re-enumerates, libusb in a long-running
  Python process keeps missing the new device, so it looks as if the agent never came back.
  `reboot_test.py` runs every step as a new `f286.py` process for this reason.
- **Do not use U Disk mode**, neither from the boot popup nor through USBMODE. The serial link does
  not come back from it without a reboot or hands on the phone.
- **Only USB service type 4.** Type 2 (1782:000C, RNDIS plus two vendor interfaces) never connected,
  and on my PC it crashed Windows in mslldp.sys until I disabled LLDP on that adapter.
- **Unplugging while a manually started agent runs is not handled.** Exit and start it again.

### Dead-man incident

`f286.py deadman on` uploads `deadman.bin`, which re-arms the PMIC watchdog every second with a
30 s timeout, so a hung MMI task resets the phone by itself. That worked once: with it on, the
read-only page write above reset the phone and the agent was back 46 s later.

Then I left it armed and unattended. After about 10 minutes the phone dropped off USB, and from then
on every boot ran the agent but serial never connected, and the boot popup offered only Charging and
U Disk. A battery pull fixed it. I don't know why. A watchdog reset does clear the watchdog (its
control register read 0 after the reboot tests), so it is not a watchdog left running. My suspicion
is that the stock configuration call also enables the watchdog interrupt, which fires 4 s before
the reset and may make the firmware record an abnormal restart; the 50 ms reboot never reaches it.
Until that is understood, **do not use the dead-man**. `reboot` (50 ms) is fine.

## Layout

```
src/f286dbg.c       the agent
src/deadman.c       PMIC watchdog dead-man switch (see above)
sdk/                headers, runtime and linker script copied from mocor-sdk
tools/f286.py       PC client and library
tools/phone_smoke.py, tools/reboot_test.py
build.py
```

## Licence

MIT, see [LICENSE](LICENSE).
