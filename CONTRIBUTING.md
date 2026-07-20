# Contributing

Thanks for your interest in improving the Agent BLE Gamepad Simulator!

## Project layout

```
firmware/gamepad/   ESP32-C3 Arduino sketch
host/                       Python driver + demo
docs/                       Protocol specification
.github/workflows/          CI (compile check)
```

## Building the firmware locally

Using [`arduino-cli`](https://arduino.github.io/arduino-cli/):

```bash
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "ESP32-BLE-Gamepad"   # pulls NimBLE-Arduino too
arduino-cli compile --fqbn esp32:esp32:esp32c3 firmware/gamepad
```

Version note: Arduino-ESP32 core **3.x** pairs with NimBLE-Arduino **2.x**;
core **2.0.x** pairs with NimBLE-Arduino **1.4.x**. Mismatches cause build
errors.

## Working on the host driver

```bash
pip install -r host/requirements.txt
python -m py_compile host/gamepad.py host/example_demo.py
python host/gamepad.py --port /dev/ttyACM0   # interactive REPL
```

## Protocol changes

`docs/PROTOCOL.md` is the source of truth. If you change the wire protocol,
update the firmware parser, the Python driver, **and** `docs/PROTOCOL.md`
together, and note it in `CHANGELOG.md`.

## Style

- Firmware: keep it C++11-clean and warning-free; match the existing 2-space
  indentation (see `.editorconfig`).
- Python: PEP 8, 4-space indentation, standard library + `pyserial` only.

## Pull requests

Keep PRs focused, describe the behavior change, and make sure CI is green
(firmware compiles for `esp32c3` and the host code byte-compiles).
