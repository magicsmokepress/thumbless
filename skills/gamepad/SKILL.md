---
name: gamepad
description: Drive the thumbless ESP32-C3 BLE HID gamepad from an AI agent to play games (e.g. Pong). Use when the agent needs to press buttons, move sticks/D-pad, or otherwise act as a game controller over the board's USB serial link. Triggers on "play the game", "press A", "move the paddle", "control the gamepad", "drive the controller".
---

# Playing games with the thumbless gamepad

You (the agent) control a real BLE game controller: an ESP32-C3 flashed with
this repo's firmware. It's paired to a computer/phone as **"Agent Pad"** and
shows up to games as a standard gamepad. You drive it over the board's USB
serial link with the `GamePad` Python class in `host/gamepad.py`.

The board does the Bluetooth; you just send high-level intents.

## Setup (once per session)

```python
from gamepad import GamePad     # run from the repo's host/ directory

pad = GamePad("/dev/ttyACM0")   # Linux; "COM5" on Windows, "/dev/cu.usbmodem…" on macOS
if not pad.wait_until_connected(timeout=30):
    raise SystemExit("No BLE host paired the controller yet — pair 'Agent Pad' first.")
```

- **Requires** `pyserial` (`pip install pyserial`) and the firmware flashed +
  paired (see the repo README / Releases).
- **Find the port:** Linux `ls /dev/ttyACM* /dev/ttyUSB*`; Windows Device
  Manager COM ports; macOS `ls /dev/cu.usbmodem*`. If `could not open port`,
  another program holds it (close serial monitors) or you lack permission
  (Linux: add your user to the `dialout` group).
- Always use it as a context manager or call `pad.reset()` when done, so no
  button is left stuck down:
  ```python
  with GamePad(port) as pad:
      pad.wait_until_connected()
      ...   # play
  # exit → everything released, axes centred
  ```

## The controls you have

| Intent | Call | Notes |
|--------|------|-------|
| Tap a button | `pad.tap("A")` | press ~60 ms then release; `ms=` to change |
| Hold / release | `pad.press("RB")` / `pad.release("RB")` | stays down until released |
| Hold for a block | `with pad.hold("X"): ...` | auto-releases at block end |
| Left stick | `pad.left_stick(x, y)` | floats −1..1. x: left..right, **y: down..up** |
| Right stick | `pad.right_stick(x, y)` | same convention |
| Triggers | `pad.trigger_left(v)` / `pad.trigger_right(v)` | floats 0..1 |
| D-pad | `pad.dpad("UP")` | `UP UR R DR DOWN DL L UL`, `None`=centre |
| Neutral axes | `pad.center()` | sticks/triggers rest, buttons kept |
| Full reset | `pad.reset()` | release everything, centre axes |
| Read state | `pad.status()` | `{connected, pressed_mask, lx, ly, …}` |

Buttons accept aliases `A B X Y LB RB LT RT BACK START L3 R3 GUIDE` or indices
`1..16`. **The alias is just a label — the game/OS decides what each index
does.** If a control does the wrong thing in-game, remap it in the game or
Steam, not here.

## How to actually play

1. **Confirm you're connected** before acting: `pad.wait_until_connected()`.
   Nothing you send reaches the game until a BLE host is paired.
2. **Perceive, then act.** You have no feedback through the pad about what's on
   screen — decide the move from whatever vision/state source you have, then
   issue the control. The pad is output-only.
3. **Hold vs tap.** For continuous motion (a Pong paddle, walking), `press`/hold
   a direction or set a stick and leave it; `center()` or release to stop. For
   discrete actions (jump, confirm), `tap`.
4. **One move at a time is fine.** Each call sends immediately. Sleep on the host
   between moves (`time.sleep`) to control timing.

### Example: play Pong

Paddle is usually the left stick's Y axis (or Up/Down). Move toward the ball,
stop when aligned.

```python
import time
from gamepad import GamePad

with GamePad("/dev/ttyACM0") as pad:
    pad.wait_until_connected()
    while game_is_running():                 # your loop condition
        ball_y  = read_ball_y()              # from your vision/state source
        pad_y   = read_paddle_y()
        error   = ball_y - pad_y
        if abs(error) < DEADZONE:
            pad.left_stick(y=0.0)            # aligned → stop
        else:
            pad.left_stick(y=1.0 if error > 0 else -1.0)  # up if ball above
        time.sleep(0.02)
    # context-manager exit centres the stick and releases everything
```

D-pad variant if the game uses digital up/down instead of a stick:

```python
pad.dpad("UP")    # move paddle up
pad.dpad(None)    # stop
pad.dpad("DOWN")
```

## Rules of thumb

- **Never leave a button or stick stuck.** End every session with the context
  manager or `pad.reset()`. A held direction runs the paddle into the wall
  forever.
- **Don't turn `ECHO` off.** The driver waits for the firmware's `OK`; disabling
  echo makes every call hang.
- **Check `pad.status()`** if behaviour is surprising — `connected=False` means
  the game isn't receiving anything.
- Wire-level details (raw command grammar, ranges, atomic `;`-chaining) are in
  `docs/PROTOCOL.md`; you rarely need them — the `GamePad` methods above cover
  normal play.
