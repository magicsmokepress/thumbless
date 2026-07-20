#!/usr/bin/env python3
"""
example_demo.py — a small scripted demo showing how Agent drives the pad.

Usage:
    pip install pyserial
    python example_demo.py --port /dev/ttyACM0     # Linux
    python example_demo.py --port COM5             # Windows
    python example_demo.py --port /dev/cu.usbmodem1101   # macOS

Before running: flash gamepad.ino to the ESP32-C3 and pair the
"Agent Pad" from your PC's Bluetooth settings. To watch it work, open a
gamepad tester (Windows: joy.cpl; web: https://hardwaretester.com/gamepad;
Linux: `jstest /dev/input/js0`).
"""

import argparse
import math
import time

from gamepad import GamePad


def main(port: str) -> None:
    with GamePad(port) as pad:
        print("Waiting for a BLE host to connect to 'Agent Pad'...")
        if not pad.wait_until_connected(timeout=60):
            print("No host connected. Pair the pad from your PC and re-run.")
            return
        print("Connected! Running demo. Watch your gamepad tester.\n")

        # 1) Tap the face buttons in sequence.
        for btn in ("A", "B", "X", "Y"):
            print(f"  tap {btn}")
            pad.tap(btn)
            time.sleep(0.4)

        # 2) Roll the D-pad around the compass.
        for d in ("UP", "UR", "R", "DR", "DOWN", "DL", "L", "UL", "C"):
            print(f"  dpad {d}")
            pad.dpad(d)
            time.sleep(0.15)

        # 3) Sweep the left stick in a full circle.
        print("  left-stick circle")
        for deg in range(0, 361, 15):
            r = math.radians(deg)
            pad.left_stick(x=math.cos(r), y=math.sin(r))
            time.sleep(0.03)
        pad.center()

        # 4) Squeeze both triggers in and out.
        print("  triggers")
        for v in [i / 10 for i in range(0, 11)] + [i / 10 for i in range(10, -1, -1)]:
            pad.trigger_left(v)
            pad.trigger_right(1.0 - v)
            time.sleep(0.05)
        pad.center()

        # 5) A tiny "combo": hold RB while tapping X twice, then a macro.
        print("  combo + macro")
        with pad.hold("RB"):
            pad.tap("X")
            time.sleep(0.1)
            pad.tap("X")

        pad.run_macro([
            ("PRESS A", 0.05),
            ("RELEASE A", 0.15),
            ("LSTICK 32767 0", 0.30),   # full right
            ("LSTICK -32767 0", 0.30),  # full left
            "CENTER",
        ])

        pad.reset()
        print("\nDemo complete. Pad reset to neutral.")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Agent gamepad demo")
    ap.add_argument("--port", "-p", required=True,
                    help="serial port, e.g. /dev/ttyACM0 or COM5")
    main(ap.parse_args().port)
