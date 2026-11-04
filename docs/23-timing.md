# 23 — Timing

> The port's clock runs in **68000 cycles**. A PAL frame is
> 7,670,454 / 50 = **153,409 cycles** — that is not an estimate, it is the
> definition of the hardware.

## What was there before, and why it was the wrong quantity

Until now the clock advanced in **memory accesses**, 46,000 per frame. I
estimated that number:

```
7,670,000 cycles/s ÷ 50 = 153,400 cycles per frame
typical instruction: ~10 cycles, ~3 accesses
→ 153,400 / 10 × 3 ≈ 46,000 accesses
```

The problem was not the imprecision. It was that **memory accesses do not
measure time in this port**: recompiled code does not fetch instructions from
memory. On hardware every instruction costs at least one access — the opcode
fetch. Here, `moveq`, a register-to-register add, `dbra` and everything else that only touches
registers cost **zero accesses and real cycles**.

Measured with the game running, cycle counter on and the old clock still in
charge:

```
3,550,000 cycles per frame   (hardware: 153,409)
```

**23 times more CPU time per frame than on a Mega Drive.**

## Why this matters, if the game already worked

It does not matter while there is headroom — and there is, most of the time. It
matters when the original does **not** have headroom and stutters.

A port that never stutters where the original stutters is not reproducing the
original. It is the project's golden rule applied to time instead of pixels:
reproduce first, defects included.

Measured after the change, over 20,000 frames:

```
V-blank handler: 62,867 cycles per frame on average (40% of the budget)
frames that overrun the budget: 176 of 20,001  (0.9%)
worst case: 4,064,008 cycles = 26 frames
```

Those 0.9% are the moments a real Mega Drive freezes for an instant — a screen
transition with decompression and heavy DMA. With the old clock **no frame ever
overran**, because the budget was 23× larger than the real one.

The number is in the exit report, permanently. It is the metric that says
whether the port reproduces the *timing* of the original, not only the logic.

## Where the cycles come from

`tools/ciclos.py` computes the cost of each instruction, and the recompiler
emits `cpu->ciclos += N;` instruction by instruction. The tables are the ones
from the 68000 manual: **instruction base time + effective address calculation
time**.

```
mode            byte/word   long
Dn, An              0         0
(An), (An)+         4         8
-(An)               6        10
d(An)               8        12
d(An,Xn)           10        14
abs.w               8        12
abs.l              12        16
#immediate          4         8
```

`MOVE` costs `4 + EA(source) + EA(destination)`; arithmetic with a register
destination costs `4 + EA` (or `6`/`8` for long); a taken branch costs 10, `BSR`
18, `RTS` 16.

## What those tables do not model

Recorded here because the difference between "measured" and "measured with a
caveat" matters:

- **Bus contention and prefetch.** A real 68000 loses cycles competing with the
  VDP for the bus during active display, and DMA steals the bus entirely. None
  of that is modelled.
- **`MULU`/`DIVU`/`DIVS` use averages** (70/140/158). The real cost depends on
  the operands and varies by almost 2×.
- **`Bcc` always counts as taken** (10 cycles). Without knowing the outcome at
  translation time, the honest average is the taken one — a loop branch is taken
  nearly always.
- **Shifts by register** use 8 as an average count.

The remaining error is a few percent, against an entirely wrong quantity before.

## What moves with it

Everything else derives from the same cycles, proportional to **how much of the
frame has elapsed**, and closes exactly at the boundary:

| | |
|---|---|
| Z80 | 3,579,545 / 50 = 71,590 cycles per frame |
| YM2612 | receives the **same** clock as the 68000 — the target is the cycle count itself |
| audio | 1065 pairs per frame, exactly |

That gives both properties at once: **within** a frame the DAC is sampled at the
right instant and the Z80 executes interleaved with the 68000; **between**
frames the amount is always the same, so nothing accumulates and nothing is
missing.

## A side effect that confirmed the fix

The "interrupt masked" warning during boot started firing at 9.2 million
cycles — about 60 frames. That is the ROM checksum loop, which runs with
interrupts masked on purpose and takes ~1 second **on the console too**. With
the old clock the same loop appeared as 20 frames of fake time. The warning
threshold moved to 120 frames.
