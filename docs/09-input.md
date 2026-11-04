# 09 — Input

## The controller is not a register

`$A10003` (player 1) and `$A10005` (player 2) do not return "the buttons". They
are a **multiplexer**: the TH line (bit 6) selects which half of the controller
appears in the low six bits, and the game toggles TH to read both.

```
TH = 1:  ?1CBRLDU
TH = 0:  ?0SA00DU
```

Every signal is **active low** — a released button reads 1. And pins configured
as outputs in `$A10009`/`$A1000B` read back *what was written*, not the
controller; that is how the game confirms its own TH write.

With TH low, left and right read a fixed zero. That is not a useless detail: it
is how the game tells "controller present" from "empty port".

## The header's `J4` is false — and three buttons is correct

The ROM header declares `J4`, which means Team Player (4-player adapter)
support. That led to listing the adapter as pending work. **The code says
otherwise**, and the code wins:

| finding | evidence |
|---|---|
| **No Team Player** | `IoInit` (`$00065E`) writes `$40` to `$A10009`/`$A1000B`/`$A1000D` — TH as output and nothing else. The Team Player protocol requires **TR as well** (`$60`/`$70`), because the handshake works by toggling TR and reading TL. |
| **Two ports only** | `PadReadAll` (`$000722`) calls the read with port index 0 and port index 1, and nothing else. `PadReadPort` computes `$A10003 + d0*2`, which is exactly `$A10003` and `$A10005`. |
| **Three buttons only** | `PadReadRaw` (`$00079E`) writes `$70` to the port's data register, reads, writes `$30`, reads. **Two TH cycles.** Six buttons would need six or more transitions. |

`J4` is metadata the developer fills in. It is not generated from the code, and
here it does not match it. It may have been an abandoned plan.

**Consequence:** input in this port is *complete*. Two players and three buttons
is not an implementation limit — it is what the game does. Six-button detection
would be code that never executes.

The names and the evidence live in `analysis/symbol-names.csv`, which is where
verified knowledge belongs.

## Mapping

| Mega Drive | Player 1 | Player 2 | Gamepad |
|---|---|---|---|
| D-pad | `W A S D` | arrow keys | D-pad or left stick |
| A | `J` | numpad `1` | west button |
| B | `K` | numpad `2` | south button |
| C | `L` | numpad `3` | east button |
| Start | `Enter` | numpad `0` | Start |

The split is physical, and that is what makes two players fit on one keyboard:
player 1 gets a continuous region on the left (`WASD` + `JKL` + Enter), player 2
a continuous region on the right (arrows + numpad). Neither reaches into the
other's.

Player 2's buttons also accept the **top number row** (`1` `2` `3` `0`), because
keyboards without a numeric keypad exist and that is the obvious fallback. The
alternates collide with nothing of player 1's, so accepting both costs nothing
and stops somebody being unable to play.

With two controllers connected, each takes a port and the keyboard stays live in
parallel.

A controller connected mid-game takes the **first free port** — it joins as
player 2 without displacing player 1. If the two land the wrong way round,
`F4` swaps them.

### Any controller, including the ones SDL does not know

`SDL_Gamepad` only sees devices in SDL's mapping database. For an Xbox or
DualShock that is ideal: SDL reports SOUTH/EAST/WEST directly. For a no-name
generic pad, or an original Mega Drive controller on a USB adapter, there is no
mapping — and SDL does not even emit `SDL_EVENT_GAMEPAD_ADDED`. The controller
simply does not exist as far as the game is concerned, with no error anywhere.

So every device SDL does *not* recognise is opened as a raw joystick and mapped
by heuristic:

- **directions** from the hat *and* the first two axes, combined. Nearly every
  device uses one or the other, and reading both avoids having to guess which.
- **A, B, C** are buttons 0, 1 and 2 — the order almost every device numbers its
  bottom row, and on a Mega Drive pad through an adapter literally A, B, C.
- **Start** is the one that genuinely varies: index 3 on a 3-button MD pad, 6 or
  7 on a 6-button one, 9 on a typical PC pad. Rather than pick one and be wrong,
  **any of the candidates is accepted**. Being stuck on the title screen unable
  to start is the worst place for a heuristic to fail.

`tinytoon.log` records which path each controller took. It is the first thing to
check when a button does not respond.

## Other keys

| | |
|---|---|
| `F1` | pause |
| `F2` | fast-forward |
| `F3` | image mode |
| `F4` | swap controller ports |
| `Esc` | quit |

Function keys, not letters, and deliberately. `P`, `Tab`, `F` and `T` sat in the
middle of the keyboard, where both players have their hands. Pausing or swapping
ports by accident mid-match is the kind of defect nobody reports as a bug — they
just find the program strange. The F row escapes that by construction: no player
map reaches it.

Pause freezes the entire machine, and that falls out of the architecture for
free: `host_frame()` is called from inside the V-blank handler, and the Z80 and
both sound chips advance from the 68000's memory accesses — which stop with it.
There is no state to save or restore.

Audio is paused **at the device**, not merely by ceasing to enqueue: the queue
already handed over would keep playing for about 100 ms after the key.

Keys and gamepad buttons can be remapped in `tinytoon.toml` (see the
release's README); the defaults above are what the game starts with.

### Pausing in BlastEm, for side-by-side comparison

BlastEm 0.6.2 **has no pause**. `ui.sms_pause` (key `z`) is the Master System's
pause *button*, a console control, not an emulator one. What works as a pause is
the debugger:

| | |
|---|---|
| `u` | enter the debugger and **freeze** emulation, sound included |
| `c` + Enter | at the debugger console, continue |

Confirmed in the `default.cfg` and `README.txt` shipped with the binary, not
from memory.

## The window does not resize when the game changes mode

The game alternates between H40 (320 px) and H32 (256 px) depending on the
screen: the Sega logo and the match are H40; the Konami logo, the menu and the
options screen are H32.

The first version resized the window to follow. It looked wrong, and it *was*
wrong: on a console the area scanned by the tube is the same in both modes. What
changes is pixel width — in H32 each pixel is 320/256 = 1.25× wider.

The logical area is now fixed at 320×224, set once, and the VDP texture is
stretched to fill it. Changing mode changes only the texture.
