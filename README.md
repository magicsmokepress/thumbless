# thumbless — a BLE gamepad for AIs that don't have thumbs (ESP32‑C3)

[![build](https://github.com/magicsmokepress/thumbless/actions/workflows/build.yml/badge.svg)](https://github.com/magicsmokepress/thumbless/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![platform](https://img.shields.io/badge/platform-ESP32--C3-blue)
![framework](https://img.shields.io/badge/framework-Arduino-teal)

Turn an **ESP32‑C3** into a *simulated* Bluetooth gamepad. It advertises as a
standard **BLE HID gamepad**, so any BLE‑gamepad‑capable host — Windows, Linux,
macOS, Android, Raspberry Pi, Steam, RetroArch/emulators, browser Gamepad‑API
games — pairs with it and uses it like a real controller.

Instead of physical sticks and buttons, the inputs are driven over the C3's
built‑in **USB serial** link by **Agent** (your program/agent), using a small
line‑based text protocol.

```
   [ Agent ]  ── USB serial ──▶  [ ESP32‑C3 ]  ── Bluetooth LE HID ──▶  [ PC / phone / … ]
   (your code)   text commands      (this firmware)   standard gamepad      (any BLE gamepad host)
```

> **Heads‑up: BLE‑only.** The ESP32‑C3 has Bluetooth **LE** only (no Classic
> BT). It works great as a generic BLE HID gamepad for PCs and phones, but it
> **cannot** impersonate a real Xbox/PlayStation/Switch controller (those use
> Bluetooth Classic). For a game *console* you'd need a Classic‑BT‑capable chip.

---

## Repository structure

```
gamepad/
├── firmware/
│   └── gamepad/
│       └── gamepad.ino     ESP32‑C3 Arduino sketch (BLE HID + serial parser)
├── host/
│   ├── gamepad.py              Python driver Agent imports (GamePad) + REPL
│   ├── example_demo.py             Scripted demo
│   └── requirements.txt            pyserial
├── docs/
│   └── PROTOCOL.md                 Full serial command protocol spec
├── .github/workflows/build.yml     CI: compile firmware for esp32c3 + byte‑compile host
├── CHANGELOG.md
├── CONTRIBUTING.md
├── LICENSE                         MIT
└── README.md
```

---

## 1. Hardware

* An ESP32‑**C3** board (e.g. ESP32‑C3‑DevKitM‑1, Seeed XIAO ESP32C3,
  Lolin C3 Mini, or any C3 module).
* A USB cable from the host running Agent to the board.

**Which USB port?** Pick the one matching your board and set the Arduino option
below accordingly:

* **Native USB** (the C3's built‑in USB‑Serial/JTAG, GPIO18/19). Set
  *USB CDC On Boot → Enabled*. `Serial` in the firmware becomes this USB link.
* **USB‑UART bridge** (a CH340/CP210x chip on the board, on UART0). `Serial`
  maps to UART0; the *USB CDC On Boot* setting doesn't matter for the data link.

Either way the firmware just uses `Serial`, so only the board setting needs to
match your cabling.

## 2. Flash the firmware

### Arduino IDE

1. Install **ESP32 board support** (*Boards Manager → “esp32” by Espressif*;
   core **3.0.x** recommended, 2.0.14+ also works).
2. Install libraries (*Library Manager*): **ESP32‑BLE‑Gamepad** by lemmingDev,
   and its **NimBLE‑Arduino** dependency.
3. Open `firmware/gamepad/gamepad.ino`.
4. *Tools*: **Board** = `ESP32C3 Dev Module` (or your board), **USB CDC On
   Boot** = `Enabled` (if using native USB), default partition scheme, select
   the **Port**, then **Upload**.

### arduino-cli

```bash
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "ESP32-BLE-Gamepad"          # pulls NimBLE-Arduino too
arduino-cli compile --fqbn esp32:esp32:esp32c3 firmware/gamepad
arduino-cli upload  --fqbn esp32:esp32:esp32c3 -p /dev/ttyACM0 firmware/gamepad
```

> **NimBLE version note:** Arduino‑ESP32 core **3.x** ↔ NimBLE‑Arduino **2.x**;
> core **2.0.x** ↔ NimBLE‑Arduino **1.4.x**. A mismatched pair causes build
> errors — align them (update the core, or pin NimBLE to 1.4.3).

On boot the serial monitor (115200) prints:

```
# Agent BLE Gamepad Simulator v1.0.0 ready. Type HELP.
# Advertising as BLE HID gamepad; pair from your host now.
```

## 3. Pair the gamepad

Pair **“Agent Pad”** like any Bluetooth device:

* **Windows:** *Settings → Bluetooth → Add device*; verify in `joy.cpl`.
* **Android / macOS:** pair from Bluetooth settings.
* **Linux:** `bluetoothctl` → `scan on` / `pair` / `connect`; test with
  `jstest /dev/input/js0`.
* **Web (any OS):** <https://hardwaretester.com/gamepad> — press a button to
  register the pad and watch inputs live.

The firmware prints `# CONNECTED` / `# DISCONNECTED` on the serial link so
Agent always knows when a host is attached.

## 4. Drive it from Agent (Python)

```bash
pip install -r host/requirements.txt
```

```python
from gamepad import GamePad          # from the host/ directory

# Port: /dev/ttyACM0 (Linux), COM5 (Windows), /dev/cu.usbmodemXXXX (macOS)
with GamePad("/dev/ttyACM0") as pad:
    pad.wait_until_connected()               # block until the PC has paired

    pad.tap("A")                             # tap A (~60 ms)
    with pad.hold("RB"):                     # hold RB for the block…
        pad.tap("X")                         # …tap X while it's held

    pad.left_stick(x=1.0, y=0.0)             # left stick fully right
    pad.right_stick(x=0.0, y=1.0)            # right stick fully up (up = +1)
    pad.trigger_right(0.5)                   # right trigger half
    pad.dpad("UP")
    pad.reset()                              # release everything, centre axes
```

Interactive tester (type protocol lines directly):

```bash
python host/gamepad.py --port /dev/ttyACM0
pad> PRESS A ; LX 32000 ; DPAD UP     # chain with ';' → one atomic report
pad> RESET
```

Full demo: `python host/example_demo.py --port /dev/ttyACM0`.

### `GamePad` API

| Method | Effect |
|--------|--------|
| `wait_until_connected(timeout=30)` | Block until a BLE host connects → `bool`. |
| `press(*btns)` / `release(*btns)` / `release_all()` | Hold / release button(s). |
| `tap(*btns, ms=None)` | Press then release (default 60 ms). |
| `hold(*btns)` | Context manager — hold for the `with` block. |
| `left_stick(x, y)` / `right_stick(x, y)` | Floats −1..1. `x`: left..right, `y`: down..up. |
| `trigger_left(v)` / `trigger_right(v)` | Floats 0..1. |
| `dpad(direction)` | `"UP" "UR" "R" "DR" "DOWN" "DL" "L" "UL"` / `None` / `0..8`. |
| `center()` / `reset()` | Neutralise axes (keep buttons) / everything. |
| `run_macro(steps)` | Host‑timed `"CMD"` or `("CMD", delay_s)` sequence. |
| `status()` | Dict: `connected`, `pressed_mask`, axis values. |
| `raw(line)` | Send a raw protocol line. |

## 5. Protocol

The host↔firmware wire protocol (line‑based ASCII over USB serial) is specified
in **[`docs/PROTOCOL.md`](docs/PROTOCOL.md)**. In short: one command per line,
`OK` / `ERR <reason>` replies, `;` to chain sub‑commands into one atomic HID
report. Buttons are `1..16` (with `A B X Y LB RB …` aliases), axes are signed
`−32767..32767`, triggers `0..32767`.

## 6. Troubleshooting

| Symptom | Fix |
|---------|-----|
| Won't compile, NimBLE errors | Match versions: core 3.x ↔ NimBLE 2.x, or core 2.0.x ↔ NimBLE 1.4.x. |
| “Agent Pad” never pairs | Confirm upload + serial greeting; remove any stale pairing and re‑add. |
| Paired but no movement in a tester | Check `# CONNECTED`; some testers need a button press first. Try <https://hardwaretester.com/gamepad>. |
| Python `could not open port` | Wrong port, or the Serial Monitor/another app holds it. Linux: join the `dialout` group. |
| Commands hang in Python | Don't run with `ECHO OFF`; the driver waits for `OK`/`ERR`. |
| Sticks/triggers mapped oddly in a game | Host‑side HID mapping — remap in the game/Steam, or edit `setWhichAxes`/axis routing in the sketch. |

## 7. Customising

Edit the config block at the top of `firmware/gamepad/gamepad.ino`:
`DEVICE_NAME`, `BUTTON_COUNT` (up to 128), `AXIS_MIN`/`AXIS_MAX`,
`STATUS_LED_PIN`, `FW_VERSION`.

---

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgements

Built on the [ESP32‑BLE‑Gamepad](https://github.com/lemmingDev/ESP32-BLE-Gamepad)
library by lemmingDev (NimBLE‑based BLE HID gamepad for the ESP32 family).
