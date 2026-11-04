# 19 — The native runtime

The runtime is the layer between recompiled logic and hardware. The logic
**never** touches a peripheral directly: everything goes through
`runtime/memory.c`. That boundary is what will later allow replacing the VDP
implementation without editing game code.

## Running

```bash
./build.sh                       # generates the C and compiles
./build/tinytoon rom.bin [access_budget] [frame_limit]
```

The executable carries 1,948 C functions, one per routine of the game.

## How the ROM's idle loop became the frame loop

The game's boot ends at `$003F38` with a short branch to its own address — a
loop that spins forever. It
is not a bug: on the console all the work is done by the V-blank interrupt, and
the main code just waits.

The recompiler recognises a **branch to its own address** and inserts a call to
`md_idle()` before the `goto`. `md_idle` delivers a frame: it calls the
registered V-blank handler and returns. The ROM's idle loop becomes the port's
main loop, with no hack.

## Three instruments that made debugging possible

Recompiled code has no instruction counter, so an infinite loop has no way to
announce itself. Three mechanisms solve that, and each caught a different
problem:

**Access budget.** `md_set_budget(n)` aborts after `n` memory accesses and prints
which hardware addresses were reached and how often. That is how it became clear
that boot loads the sound driver into Z80 RAM byte by byte.

**Backtrace on alarm.** A `SIGALRM` prints the stack. Because each of the game's
routines is a real C function, the stack comes out readable:

```
./tinytoon(sub_003EC2+0x2f5)
./tinytoon(sub_000300+0x799)
./tinytoon(Entry+0xe66)
./tinytoon(main+0x131)
```

Combined with `addr2line`, that points at the **exact 68000 instruction** — the
generated C carries the address in a comment on every line.

**A `default` that aborts.** Every indirect dispatch has a `default` that aborts
naming the site and the target. Not gratuitous strictness: a dispatch outside the
table means the static analysis is incomplete, and continuing would execute the
wrong target.

## Five translation bugs found by running

None of them showed up at compile time or in the differential tests. Only by
executing.

### 1. An `rts` that returns two levels

The VDP command-list interpreter stops at opcode 0, whose handler, at `$001572`,
pops a long off the stack into `d0` — its own return address — and then, at
`$001574`, executes an `rts`, which therefore returns **two** levels up.

It is the same idiom as `SubStateDispatch`. Translated as a C call, the loop
never ended and the pointer walked through RAM until it read garbage — which was
the original symptom (`site $001546 -> $A6384278`).

The fix is an `unwind` field in the CPU state: the routine marks it, and each
caller checks after calling. The sites live in `analysis/stack-returns.csv`.

**And here I made a mistake.** I registered a second site (`$001AFA`) with the
note "role not yet investigated" — violating the project's own rule that nothing
enters without evidence. It pops no return address at all: four instructions
earlier there is a long push of `d0` onto the stack, an ordinary save. The
effect was to make `VdpUpload` return early, skipping the instruction that pops
`sr` back off the stack, leaving interrupts masked
forever.

### 2. V-blank modelled as a marker, not a timer

The first version recognised `bra *` (a branch to itself) as the idle loop. But
the game's main loop is three instructions: at `$0003DC` it clears the SR
interrupt mask (enabling interrupts), at `$0003E0` it calls `Random` with a
`bsr`, and at `$0003E4` it branches back to `$0003E0` — back to the `bsr`, not to
itself.

That is not a branch to itself, and it escaped. The real error was conceptual:
**V-blank is a timer, not a marker in the code**. The runtime now delivers the
interrupt periodically, respecting the SR mask — which also makes the
decompressor's critical sections work.

### 3. Falling through into the next function

**120 functions** end without `rts`/`bra`/`jmp`: in the original, execution
continues at the next address, which happens to be the start of another
function. The generated C returned there instead.

The worst case: `LzDecompressB` became **a single instruction**. The entire
decompressor vanished. Fall-through is now a tail call.

### 4. The console version register

This one was not a translation bug — it was missing hardware, and it cost me a
wrong hypothesis. I had concluded the boot checksum loop was slow because of an
error in `sub.l`/`asr.l`/`swap`. Those were all correct; the problem was three
instructions before an entire code path. At `$003EC2` the routine tests bit 6 of
the version register at `$A10001`; at `$003ECA`, if the bit is 0 (domestic), it
branches to `$003ED0` and continues; otherwise, at `$003ECE`, an `rts` **returns
immediately** (overseas).

The stub returned zero, so the game believed it was on a Japanese console and
took a path ending in `bra *` — waiting for an interrupt still masked by boot.
On a European Mega Drive, which is what this ROM's header declares (region `E`),
the routine returns immediately.

`$A10001` now returns `$E0`: no Mega CD, overseas, PAL.

**Effect: from 1 frame to 568.**

The lesson is about diagnosis. The symptom was "slow loop"; the cause was a
region `btst`. Had I gone off to write differential tests for `asr.l` — my first
guess — I would have confirmed they were correct and lost the whole time. What
solved it was looking at the stack at the moment it stopped, not theorising about
the instruction.

### 5. The pop that did not exist (4 bytes of drift per frame)

The symptom was the game hanging at frame 568 inside `LzDecompressB`. The cause
was five levels away, and the path to it is the method:

```
[lz]  a5=0003D168 a1=0003D5F7 d0=0000048F   <- fine
[lz]  a5=22246713 a1=22256712 d0=0000FFFF   <- garbage, and odd
[idx] d0=008C6001                            <- should be $00000001
[idx] a7=00000798                            <- the STACK is inside the ROM
```

`a7` pointing into ROM explains everything at once: writes to ROM are ignored, so
the MOVEM that saves `d0`–`d2` on the stack pushed into the void, and the MOVEM
that restores them read ROM bytes as if they were the saved registers. One measurement per frame gave the
exact rate:

```
frame   0  a7=FFFFFF00
frame  64  a7=00000000     <- +4 per frame, exactly one per V-blank
```

And the source is the "returns two levels" idiom — the same as bug 1, with the
half I had not seen: the handler at `$001572` that pops its own return address
into `d0` and then, at `$001574`, returns **two** levels up with an `rts`.

`unwind` solved the **control flow** and I left the **pop translated normally** —
it is written in the comment I put there myself. But on the 68000 that pop
removes a return address the `bsr` pushed, and here `bsr` became a C function
call: that address **never existed on the emulated stack**. Each pass added 4
bytes to `a7`, and after ~570 frames the stack had left RAM.

The lesson is about the model, not the instruction: when you replace the
hardware's return stack with C's, any code that *manipulates return addresses as
data* stops having meaning and must be handled, not translated. Only the
observable effect survives.

The runtime now **checks** this: if a V-blank handler returns `a7` different from
what it received, it stops immediately and reports the imbalance. An error that
took 568 frames to appear, far from its cause, now appears on the first frame, on
top of it.

## Where the hardware stands

**Real:** ROM, 64 KB work RAM, Z80 RAM, the VDP (VRAM/CRAM/VSRAM, DMA, planes,
sprites, window, per-line scroll, per-line sprite limit), the Z80 executing the
game's real sound driver, the YM2612 with verified FM synthesis, the PSG, the
joypad ports, and a clock in 68000 cycles.

**Measured as unreachable, therefore not implemented:** sprite masking, the VDP
FIFO stall, SSG-EG on the YM2612. Each of those was closed by measuring whether
the game reaches it — see [05-vdp.md](05-vdp.md) and
[14-audio.md](14-audio.md).

**Still unimplemented hardware accesses: 7 in 20,000 frames**, all during boot.
The number lives in the exit report, so it cannot quietly grow.
