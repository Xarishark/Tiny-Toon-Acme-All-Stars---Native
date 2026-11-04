# 16 — Functions

`analysis/functions.csv` lists the functions reached by the scan. The vast
majority are still called `sub_XXXXXX` — an address wearing a name's clothes.
This document covers the ones already identified, and above all **why**.

## How a name gets in here

`analysis/symbol-names.csv` is an **input** file, versioned, in the same spirit as
`jump-tables.csv`. Each row carries the name and the **evidence** behind it:

```csv
address,name,confidence,evidence
$0003E6,Random,alta,"LCG: seed at $FFFB02 times $23 plus $929, divided by $11D7, remainder kept; ..."
```

`confidence` is `alta` (high) when the code proves the name on its own (an LCG is
an LCG), and `media` (medium) when the name is a reasonable but unclosed
inference — for instance knowing a routine copies position between two objects
without knowing in what situation it is called.

Names apply simultaneously to the emitted disassembly, to `functions.csv` and to
`symbols.csv`. Renaming is one CSV line, and the byte-identical gate stays green
afterwards — a name never changes a byte.

**A name without evidence is a guess wearing a fact's clothes.** If the
`evidence` column cannot be filled in, the name does not go in.

## The object struct

Deduced from `ObjApplyVelocity` and `ObjSetVelocityFromAngle`, which together fix
the layout:

| Offset | Size | Field |
|---|---|---|
| `$00` | word | type/id (written by `ObjSetTypeAndCopyPos`) |
| `$04` | long | X position, 16.16 fixed point |
| `$08` | long | Y position |
| `$0C` | long | Z position |
| `$10` | long | X velocity |
| `$14` | long | Y velocity |
| `$18` | long | Z velocity |
| `$1E` | word | animation frame index |
| `$20` | word | frame counter |

`ObjApplyVelocity` literally does `$4 += $10`, `$8 += $14`, `$C += $18`, in long.
Three axes with Z separate is coherent with a perspective sports game, where the
ball's height is independent of its position on the field.

### The array

Objects live in an array at `$FFA000`, walked by the loop at `$0073E4`. At
`$0073C6` the code loads the address of `ObjectArray` into `a0`. Each iteration
(`$0073E4`–`$007400`) reads the word at offset `$0` of the object into `d0` (the
object type); if it is zero the slot is skipped. Otherwise it subtracts one,
multiplies by four to form a long index, loads a pointer from the table at
`$007410` (PC-relative, indexed by `d0`) into `a1` and calls it — the type's
behaviour. Then it advances `a0` by `$80` and loops back while `a0` is below
`$FFCA00`.

Four facts come out of that:

- **stride `$80`**, from `$FFA000` to `$FFC9FF` — **84 slots**;
- field `$0` is the **type**, and zero means an inactive slot;
- the type indexes the **53-behaviour** table at `$007410` (`type − 1`), the same
  one already documented as a pointer table;
- the loop runs every frame over all 84 slots, calling each active object's
  behaviour.

It is the game's object loop.

### Slot 12

`$FFA600` is `$FFA000 + 12·$80`, i.e. **slot 12** — and it is different from the
others. It receives 30 absolute `lea`s, and its fields are accessed **by absolute
address**, not by displacement from a pointer: the code reads `Obj12_PosX`
directly as an absolute short address, instead of offset `$4` from an object
pointer in `a0`.

The other slots only appear via a pointer. An object the entire codebase knows by
a fixed address is almost certainly something central and unique to the match —
the ball is the obvious candidate in a sports game, but that has not been
confirmed.

Slot 12's position and velocity fields are among the most-referenced addresses in
the ROM: `Obj12_PosX` with 133 references, `Obj12_PosY` with 126, `Obj12_PosZ`
with 101.

## Mathematics

**`SinMul` (`$0045A2`)** — sine with the magnitude built in.

The angle arrives in `d0` and the magnitude in `d1`. The routine keeps a copy of
the original angle in `d3`, doubles the angle and masks it to a word index over
half a period, reads the sine word from the PC-relative table into `d2` and does
an unsigned multiply by `d1`, so `d2 = sin(angle) * d1`. Finally it tests the low
byte of the saved angle: if its sign bit is set (the negative half of the circle)
it negates `d2` as a long.

Angle 0–255 for a full turn. The table at `$0045BA` has 128 words, and its values
are `sin(k · 360/256) · 256`.

**`CosMul` (`$00459E`)** — adds `$40` to the angle in `d0` and falls into
`SinMul`. A quarter turn on a 0–255 scale is `$40`; cosine by definition.

**`Random` (`$0003E6`)** — a linear congruential generator, 62 callers:

```
seed = ($FFFB02 · $23 + $929) mod $11D7
```

The remainder stays in `$FFFB02` and is returned in `d0.w`. So `$FFFB02` is
`RandomSeed`.

## Video

**`VdpUpload` (`$001CDE`)**, 53 callers — disables interrupts (raises the SR interrupt mask to 7),
saves `a4`/`a5`, points `a6` at `$FFFF0000`, calls `VdpUploadEntry` and restores
`sr`.

**`VdpUploadEntry` (`$001CFE`)** — reads a word from `(a5)+`, calls
`LzDecompressA` and assembles the VDP's VRAM write command in two words: in
`d1`, the low 14 bits of the address combined with the write code `$4000`; in
`d2`, the top two address bits rotated down to bits 0–1 and combined with `$80`.

## Compression

**`LzDecompressA` (`$001E3A`)** and **`LzDecompressB` (`$001DAC`)** — the two LZSS
variants. Full formats in [25-codecs.md](25-codecs.md).

`$001DAC` had been named `ClearTextBuffer` and `$FF8800` `TextBuffer`, from
reading only the loop that fills the area with `$20202020`. That was wrong: it is
the decompressor's ring, and `$20` is the classic LZSS initialisation. The lesson
is recorded in [10-graphics.md](10-graphics.md).

**`RleUnpack` (`$01AD02`)** — the third codec, and the one static analysis never
found. It was discovered from the read map, by the reverse of the usual path: not
"what does this routine do?" but "147 KB of the ROM ends up here, who is this?".
See [24-read-map.md](24-read-map.md).

## Game states

The 16 pointers at `$0004B6` are indexed by `$FFFB04`. Thirteen of them begin
by reading the word at `$FFFB06` into `d0` — they read the sub-state and
dispatch.

Eleven were identified by observing the game in BlastEm, with a breakpoint at
`$000540` (just after `SetGameState` writes the variable):

| State | Screen |
|---|---|
| 0 | Konami logo |
| 1 | demo (enters on its own if nothing is pressed at the menu) |
| 2 | Sega logo |
| 3 | *unidentified* — only written directly by boot, at `$0003AA` |
| 4 | character/stage select **and the match itself** |
| 5 | the screen preceding selection (main menu on display, "stage 1" in story mode) |
| 6 | password |
| 7 | post-match ("thank you") |
| 8 | first screen, right after boot |
| 9 | options menu |
| 10 | *unidentified* |
| 11 | story mode start |
| 12 | story mode, after the intro cutscene |
| 13, 14, 15 | *unidentified* — 14 and 15 come from `State_07_PostMatch` |

Two corrections observation forced, both against what I had inferred statically:

**The match has no state of its own.** It runs inside state 4, together with
character selection, distinguished only by the sub-state `$FFFB06`. None of the
five write breakpoints on `$FFFB04` fires when the match begins.

**State 10 is not the match.** I had inferred that from the 11
word comparisons of `$FFFB04` against 10 scattered through the game code. Re-read, they
are branches treating state 10 as a *special case* inside shared code — at
`$00DA72`, if the state is 10 a check on `$FFFB1E` is skipped. What state 10 *is*
remains open; the demo, which was the next guess, turned out to be state 1.

State 5 deserves a note: the handler is generic — it runs `$01C4B2` until `$FB08`
is set and then loads 4 into `d0`. The screen's contents come from the
sub-state, not from the state. That is why it appears as the main menu on one
path and as a stage opening on another.

## The trampoline table at `$0E5000` — the sound API

`$0E5000` has 213 callers and a single instruction: an absolute long JMP to
`$0E5042`. It is not a function — it is entry [0] of a **trampoline table**,
6-byte entries (each an absolute long JMP) at regular spacing:

| Entry | Jumps to |
|---|---|
| `$0E5000` | `$0E5042` |
| `$0E5006` | `$0E5040` |
| `$0E500C` | `$0E5098` |
| `$0E5012` | `$0E5142` |
| `$0E5018` | `$0E5100` |
| `$0E501E` | `$0E5040` — unused slots point at the same place |

The targets all live in `$0E5xxx`, far from the calling code, which suggested a
module with a stable interface — a natural candidate for a clean boundary in the
native port. It turned out to be exactly that: **the sound API.**

| Address | Name | What it does |
|---|---|---|
| `$0E5000` | `SoundRequest` | the public veneer, 213 callers — the most in the ROM |
| `$0E5064` | `PlaySound` | writes `d0.b` into the `SoundQueue` ring at `$FFD410` and advances the write index. It does not talk to the Z80; it only deposits. |
| `$0E5098` | `SoundQueueDrain` | drains one item per call: masks interrupts, takes the Z80 bus via `$A11100`, checks the `$A01E00` mailbox is free, copies one byte, releases the bus. If the mailbox is busy it gives up without consuming — the item waits for the next frame. |

Finding this had a practical payoff beyond naming: writing into that ring is
exactly what the menu's sound test does, so the runtime can drive every sound ID
by the front door in order to raise ROM coverage of the audio bank. See
[24-read-map.md](24-read-map.md).
