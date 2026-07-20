#!/usr/bin/env python3
"""
gamepad.py — host-side driver for the Agent BLE Gamepad Simulator.

This is the piece that "Agent" (your program / agent) imports to drive the
ESP32-C3 over its USB serial link. The C3 does the BLE HID part; this class
just speaks the simple line protocol the firmware understands.

    from gamepad import GamePad

    with GamePad("/dev/ttyACM0") as pad:      # or "COM5", "/dev/cu.usbmodem..."
        pad.wait_until_connected()               # wait for the PC to pair
        pad.tap("A")                             # tap the A button
        pad.left_stick(x=1.0, y=0.0)             # push left stick fully right
        pad.dpad("UP")
        with pad.hold("RB"):                     # hold RB for the block
            pad.tap("X")

Requires pyserial:  pip install pyserial

Conventions
-----------
* Sticks take floats in [-1.0, 1.0].  x: -1 left / +1 right.  y: -1 down / +1 up
  (intuitive "up is positive"; the driver flips it to the HID sign for you).
* Triggers take floats in [0.0, 1.0].
* Buttons are 1..16, or the aliases the firmware knows:
  A B X Y LB RB LT RT BACK START L3 R3 GUIDE
  (aliases are just labels for button indices — the *host* decides what each
  index means; remap on the host/game side as you like.)
"""

from __future__ import annotations

import re
import sys
import time
from contextlib import contextmanager

try:
    import serial  # pyserial
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "pyserial is required:  pip install pyserial"
    ) from exc


AXIS_MAX = 32767  # must match the firmware's AXIS_MAX


class GamePadError(RuntimeError):
    """The firmware replied with 'ERR <reason>' to a command."""

    def __init__(self, reason: str, command: str):
        super().__init__(f"device rejected {command!r}: {reason}")
        self.reason = reason
        self.command = command


class GamePad:
    """Talks to the ESP32-C3 Agent gamepad simulator over USB serial."""

    def __init__(
        self,
        port: str,
        baud: int = 115200,
        cmd_timeout: float = 2.0,
        read_timeout: float = 0.2,
    ):
        # baud is ignored by the C3 native USB-CDC but harmless to set.
        self.ser = serial.Serial(port, baud, timeout=read_timeout)
        self.cmd_timeout = cmd_timeout
        # Give the port a moment and drop any boot/greeting chatter.
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    # ----------------------------------------------------------------- I/O ---
    def _readline(self) -> str:
        return self.ser.readline().decode(errors="replace").rstrip("\r\n")

    def send(self, line: str, timeout: float | None = None) -> list[str]:
        """Send one command line; return any '#' info lines. Raise on ERR.

        Blocks until the firmware's terminal 'OK'/'ERR' reply arrives (echo
        must be ON, which is the firmware default — don't turn it off if you
        use this method).
        """
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        info: list[str] = []
        deadline = time.time() + (self.cmd_timeout if timeout is None else timeout)
        while time.time() < deadline:
            ln = self._readline()
            if ln == "":
                continue  # read timeout tick; keep waiting until deadline
            if ln.startswith("#"):
                info.append(ln[1:].strip())
                continue
            if ln == "OK":
                return info
            if ln.startswith("ERR"):
                raise GamePadError(ln[3:].strip() or "error", line)
            info.append(ln)  # anything else: treat as info, keep waiting for OK
        raise TimeoutError(f"no reply from device for command: {line!r}")

    # ----------------------------------------------------------- meta/query ---
    def ping(self) -> None:
        self.send("PING")

    def status(self) -> dict:
        """Return a dict of the device state (connected, pressed mask, axes)."""
        info = self.send("STATUS")
        text = " ".join(info)
        out: dict = {}
        m = re.search(r"connected=(\d+)", text)
        if m:
            out["connected"] = bool(int(m.group(1)))
        m = re.search(r"pressed=0x([0-9A-Fa-f]+)", text)
        if m:
            out["pressed_mask"] = int(m.group(1), 16)
        for key in ("LX", "LY", "RX", "RY", "LT", "RT", "HAT"):
            m = re.search(rf"\b{key}=(-?\d+)", text)
            if m:
                out[key.lower()] = int(m.group(1))
        return out

    def is_connected(self) -> bool:
        return bool(self.status().get("connected"))

    def wait_until_connected(self, timeout: float = 30.0, poll: float = 0.4) -> bool:
        """Block until a BLE host has paired/connected, or timeout. """
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.is_connected():
                return True
            time.sleep(poll)
        return False

    # ------------------------------------------------------------- buttons ---
    @staticmethod
    def _fmt_buttons(buttons) -> str:
        parts: list[str] = []
        for b in buttons:
            if isinstance(b, (list, tuple)):
                parts.append(GamePad._fmt_buttons(b))
            else:
                parts.append(str(b).upper())
        if not parts:
            raise ValueError("no buttons given")
        return ",".join(parts)

    def press(self, *buttons) -> None:
        """Hold one or more buttons down (until release)."""
        self.send("PRESS " + self._fmt_buttons(buttons))

    def release(self, *buttons) -> None:
        """Release one or more buttons."""
        self.send("RELEASE " + self._fmt_buttons(buttons))

    def release_all(self) -> None:
        self.send("RELEASE ALL")

    def tap(self, *buttons, ms: int | None = None) -> None:
        """Press then release button(s); the press lasts `ms` (default 60)."""
        cmd = "TAP " + self._fmt_buttons(buttons)
        if ms is not None:
            cmd += f" {int(ms)}"
        self.send(cmd)

    @contextmanager
    def hold(self, *buttons):
        """Context manager: hold button(s) for the duration of the block."""
        self.press(*buttons)
        try:
            yield self
        finally:
            self.release(*buttons)

    # -------------------------------------------------------------- axes -----
    @staticmethod
    def _axis(v: float) -> int:
        v = max(-1.0, min(1.0, float(v)))
        return int(round(v * AXIS_MAX))

    @staticmethod
    def _trig(v: float) -> int:
        v = max(0.0, min(1.0, float(v)))
        return int(round(v * AXIS_MAX))

    def left_stick(self, x: float = 0.0, y: float = 0.0) -> None:
        """Left stick. x:-1..1 (left..right), y:-1..1 (down..up)."""
        self.send(f"LSTICK {self._axis(x)} {self._axis(-y)}")  # up = +1 -> HID negative

    def right_stick(self, x: float = 0.0, y: float = 0.0) -> None:
        """Right stick. x:-1..1 (left..right), y:-1..1 (down..up)."""
        self.send(f"RSTICK {self._axis(x)} {self._axis(-y)}")

    def trigger_left(self, v: float) -> None:
        """Left analog trigger, 0.0 (released) .. 1.0 (full)."""
        self.send(f"LT {self._trig(v)}")

    def trigger_right(self, v: float) -> None:
        """Right analog trigger, 0.0 (released) .. 1.0 (full)."""
        self.send(f"RT {self._trig(v)}")

    def dpad(self, direction) -> None:
        """Set the D-pad. direction: one of
        C U UR R DR D DL L UL  (case-insensitive), or None/'' for centre,
        or an int 0..8."""
        if direction is None or direction == "":
            direction = "C"
        self.send(f"DPAD {str(direction).upper()}")

    # ------------------------------------------------------------- bulk ------
    def center(self) -> None:
        """Return sticks + triggers to rest and centre the D-pad (buttons kept)."""
        self.send("CENTER")

    def reset(self) -> None:
        """Release everything and centre all axes — the safe neutral state."""
        self.send("RESET")

    def wait(self, ms: int) -> None:
        """Ask the *device* to block for `ms`. Usually prefer time.sleep() on
        the host instead; this exists mainly for atomic ';'-chained lines."""
        self.send(f"WAIT {int(ms)}")

    def raw(self, line: str) -> list[str]:
        """Send a raw protocol line (advanced / debugging)."""
        return self.send(line)

    def run_macro(self, steps) -> None:
        """Run a host-timed sequence.

        `steps` is an iterable of either:
          - a str  -> a raw command line, sent immediately, or
          - (str, delay_seconds) -> send the line, then sleep delay_seconds.
        Example:
            pad.run_macro([
                ("PRESS A", 0.05),
                ("RELEASE A", 0.20),
                ("LSTICK 32767 0", 0.30),
                "CENTER",
            ])
        """
        for step in steps:
            if isinstance(step, (list, tuple)):
                line, delay = step[0], (step[1] if len(step) > 1 else 0)
            else:
                line, delay = step, 0
            self.send(line)
            if delay:
                time.sleep(float(delay))

    # ------------------------------------------------------------ lifecycle --
    def close(self) -> None:
        try:
            self.reset()
        except Exception:
            pass
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# --------------------------------------------------------------------------- #
#  Tiny interactive REPL:  python gamepad.py --port /dev/ttyACM0
#  Type protocol lines directly (HELP, PRESS A, LX 32000, ...). Ctrl-D to quit.
# --------------------------------------------------------------------------- #
def _repl(port: str) -> None:
    with GamePad(port) as pad:
        print(f"[gamepad] connected to {port}. Type HELP, or Ctrl-D to quit.")
        st = pad.status()
        print(f"[gamepad] device status: {st}")
        try:
            while True:
                try:
                    line = input("pad> ").strip()
                except EOFError:
                    print()
                    break
                if not line:
                    continue
                try:
                    info = pad.send(line)
                    for i in info:
                        print(f"  # {i}")
                    print("  OK")
                except GamePadError as e:
                    print(f"  {e}")
                except TimeoutError as e:
                    print(f"  (timeout) {e}")
        except KeyboardInterrupt:
            print()


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="Agent BLE gamepad host driver / REPL")
    ap.add_argument("--port", "-p", required=True,
                    help="serial port (e.g. /dev/ttyACM0, COM5, /dev/cu.usbmodem1101)")
    args = ap.parse_args()
    _repl(args.port)
