# Agent Gamepad — Serial Command Protocol

This is the authoritative specification of the USB-serial protocol between the
host ("Agent") and the ESP32-C3 firmware. The firmware parser
(`firmware/gamepad/gamepad.ino`) and the Python driver
(`host/gamepad.py`) both implement this document.

## Transport

- **Link:** the ESP32-C3's USB serial (native USB-Serial/JTAG, or a UART bridge
  — see the README for board wiring).
- **Baud:** `115200` (ignored on native USB-CDC, but set it anyway).
- **Framing:** one command per line, terminated by `\n` (line feed). A carriage
  return (`\r`) is accepted and ignored, so `\r\n` is fine too.
- **Encoding:** 7-bit ASCII.
- **Max line length:** 159 characters (longer lines yield `ERR line-too-long`).

## Syntax

```
<line>      ::= <comment> | <command> ( ';' <command> )*
<comment>   ::= '#' <anything>
<command>   ::= <keyword> ( <ws> <arg> )*
```

- Keywords are **case-insensitive** (`press`, `PRESS`, `Press` are equal).
- Arguments are separated by spaces/tabs.
- A line beginning with `#` is a comment and is acknowledged with `OK`.
- Multiple commands separated by `;` on one line are applied **atomically**:
  the firmware mutates its internal state for each, then emits **one** HID
  report at the end of the line.

  ```
  PRESS A ; LX -32000 ; DPAD UP
  ```

## Responses

Every command line produces exactly **one terminal reply**:

| Reply | Meaning |
|-------|---------|
| `OK` | Line accepted and applied. |
| `ERR <reason>` | Parse/validation error; nothing after the failing sub-command is applied. |

Some commands print **informational lines first**, each prefixed with `# `,
followed by the terminal `OK`. Asynchronous events (see below) are also emitted
as `# `-prefixed lines and may appear between commands.

> A host should read lines until it sees a terminal `OK` or `ERR`, treating any
> `# `-prefixed line as info/event. Keep `ECHO` **ON** (the default) when doing
> this, or there will be no terminal reply to wait for.

### Asynchronous events

| Event | Meaning |
|-------|---------|
| `# CONNECTED` | A BLE host has connected to the gamepad. |
| `# DISCONNECTED` | The BLE host disconnected. |
| `# Agent BLE Gamepad Simulator vX.Y.Z ready. ...` | Printed once at boot. |

## Value ranges

- **Axes (sticks):** signed integers `-32767 .. 32767`, `0` = centre.
  - Left stick → HID **X / Y**; right stick → HID **Z / RZ**.
  - `Y` follows HID convention: **negative = up**, positive = down.
- **Triggers:** integers `0 .. 32767`, `0` = released. Left → HID **RX**,
  right → HID **RY**.
- **Buttons:** index `1 .. 16`, or an alias (see below).
- **Hat/D-pad:** `0 .. 8` (see mapping below).

Out-of-range numeric values are **clamped**, not rejected.

## Buttons

Buttons are indices `1..16`. The following aliases are accepted anywhere a
button is expected — they are just human labels for indices; the **host** (game
/ OS / Steam) decides what each index actually does, so remap there as needed.

| Alias(es) | Index | Alias(es) | Index |
|-----------|-------|-----------|-------|
| `A` | 1 | `RT` | 8 |
| `B` | 2 | `BACK`, `SELECT` | 9 |
| `X` | 3 | `START`, `MENU` | 10 |
| `Y` | 4 | `L3`, `LS` | 11 |
| `LB` | 5 | `R3`, `RS` | 12 |
| `RB` | 6 | `GUIDE`, `HOME` | 13 |
| `LT` | 7 | *(14–16)* | spare |

A **button list** is comma-separated with no spaces: `A,B,3` or `1,2,7`.

## Hat / D-pad

`DPAD <dir>` (word) or `HAT <n>` (raw 0..8):

| Value | Word aliases |
|-------|--------------|
| 0 | `C` `CENTER` `CENTRE` `NONE` |
| 1 | `U` `UP` `N` |
| 2 | `UR` `UPRIGHT` `NE` |
| 3 | `R` `RIGHT` `E` |
| 4 | `DR` `DOWNRIGHT` `SE` |
| 5 | `D` `DOWN` `S` |
| 6 | `DL` `DOWNLEFT` `SW` |
| 7 | `L` `LEFT` `W` |
| 8 | `UL` `UPLEFT` `NW` |

## Command reference

| Command | Args | Effect |
|---------|------|--------|
| `PRESS` | `<btns>` | Hold button(s) down. |
| `RELEASE` | `<btns>` \| `ALL` | Release specific button(s), or all. |
| `TAP` | `<btns> [ms]` | Press, wait `ms` (default 60, max 5000), release. Emits its own reports. |
| `LX` / `LY` | `<v>` | Left stick X / Y. |
| `RX` / `RY` | `<v>` | Right stick X / Y. |
| `LSTICK` | `<x> <y>` | Left stick X and Y at once. |
| `RSTICK` | `<x> <y>` | Right stick X and Y at once. |
| `LT` / `RT` | `<v>` | Left / right analog trigger (0..32767). |
| `DPAD` | `<dir>` | Set D-pad by direction word. |
| `HAT` | `<0..8>` | Set hat by raw value. |
| `CENTER` | — | Sticks + triggers to rest, hat centred. Buttons unchanged. |
| `RESET` | — | Release all buttons and centre all axes. |
| `WAIT` | `<ms>` | Blocking delay (max 10000). Useful inside `;`-chained lines. |
| `PING` | — | Liveness check → `OK`. |
| `STATUS` | — | Print state (`# ...` lines) then `OK`. |
| `HELP` | — | Print command list then `OK`. |
| `ECHO` | `ON` \| `OFF` | Enable/disable the `OK`/`ERR` replies. |

### `STATUS` output format

```
# name=Agent Pad fw=1.0.0 connected=1 buttons=16 axis=[-32767,32767]
# pressed=0x0005 LX=0 LY=0 RX=0 RY=0 LT=0 RT=0 HAT=0
OK
```

`pressed` is a hex bitmask (bit 0 = button 1).

## Error reasons

`unknown-command`, `need-button`, `bad-button`, `need-value`, `need-x-y`,
`need-dir`, `bad-dir`, `need-ms`, `echo-needs-arg`, `echo-on-or-off`,
`line-too-long`.

## Examples

```
PING                         -> OK
TAP A                        -> OK            (A pressed ~60 ms then released)
TAP A,B 120                  -> OK            (A+B held 120 ms)
PRESS RB                     -> OK
TAP X ; RELEASE RB           -> OK            (atomic: X tap emits its own reports)
LSTICK 32767 0               -> OK            (left stick full right)
LX -32767 ; LY -32767        -> OK            (left stick up-left, one report)
LT 16384                     -> OK            (left trigger half)
DPAD UP                      -> OK
CENTER                       -> OK
RESET                        -> OK
STATUS                       -> # ... / # ... / OK
# this is a comment          -> OK
```
