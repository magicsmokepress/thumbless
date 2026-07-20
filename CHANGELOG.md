# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-07-20

### Added
- ESP32-C3 firmware (`firmware/gamepad/`) presenting a standard **BLE
  HID gamepad**: 16 buttons, two analog sticks, D-pad/hat, and two analog
  triggers.
- USB-serial command protocol driven by a host ("Agent"): buttons
  (`PRESS`/`RELEASE`/`TAP`), sticks (`LX/LY/RX/RY`, `LSTICK`/`RSTICK`),
  triggers (`LT`/`RT`), D-pad (`DPAD`/`HAT`), `CENTER`/`RESET`, `WAIT`, and
  meta commands (`PING`/`STATUS`/`HELP`/`ECHO`).
- Atomic multi-input updates via `;`-chained sub-commands (one HID report per
  line).
- Async `# CONNECTED` / `# DISCONNECTED` link-state events on the serial port.
- Host-side Python driver `host/gamepad.py` (`GamePad`) with a clean API
  and an interactive REPL, plus `host/example_demo.py`.
- Documentation: `README.md`, `docs/PROTOCOL.md`.
- CI: firmware compile check for `esp32:esp32:esp32c3` and host `py_compile`.

[1.0.0]: https://github.com/OWNER/gamepad/releases/tag/v1.0.0
