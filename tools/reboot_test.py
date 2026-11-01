"""reboot_test.py - reboot the phone N times over USB and time how long the autostarted agent takes to return.

Uses `f286.py reboot`: the stock PMIC (SC2720) watchdog driver 0x8136f3f0 with {run 1, reset mode, 50 ms}
(phone-verified). Never reset by faulting: an earlier version wrote to a read-only page,
which livelocks in the pager and needs a battery pull (README, "Safety").
Every step runs `f286.py` in a fresh process: once the phone re-enumerates, libusb inside a long-lived
process keeps missing the new device (the agent was up but in-process retries failed).

  python reboot_test.py [N] [--wait SECONDS]
"""
import argparse, subprocess, sys, time
from pathlib import Path

F286 = str(Path(__file__).with_name("f286.py"))


def f286(*args):
    r = subprocess.run([sys.executable, F286, *args], capture_output=True, text=True, timeout=60)
    return r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()[-200:]


def wait_agent(timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        out = f286("ping")
        if "proto" in out:
            return out
        time.sleep(2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("n", type=int, nargs="?", default=3)
    ap.add_argument("--wait", type=float, default=300)
    a = ap.parse_args()
    info = wait_agent(30)
    if info is None:
        raise SystemExit("agent not reachable before the test")
    print(f"start: {info}", flush=True)
    for i in range(1, a.n + 1):
        t0 = time.time()
        print(f"[{i}] {f286('reboot')}", flush=True)
        time.sleep(5)
        info = wait_agent(a.wait)
        if info is None:
            print(f"[{i}] agent did NOT come back within {a.wait:.0f} s - stopping", flush=True)
            return
        print(f"[{i}] agent back {time.time() - t0:.0f} s after reboot: {info}", flush=True)
    print("done", flush=True)


if __name__ == "__main__":
    main()
