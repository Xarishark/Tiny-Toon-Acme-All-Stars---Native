# 22 — Differential testing

> `python tools/difftest.py` — 2,889 distinct instructions from the ROM,
> 739,451 comparisons, all matching.

## The question

**Does the C I generate do the same thing as a real 68000?**

Until this existed, the answer came from playing: the `cmpa.w` missing its sign
extension — 196 wrong instructions — only surfaced when the user saw two
character-select wheels in the same corner of the screen. That is expensive, and
it is luck.

## Why Musashi and not a model of my own

Comparing my recompiler against a model I also wrote would test my reading of
the manual against itself. The `cmpa` bug would survive on both sides, because
its origin is having read the same rule the same way twice.

[Musashi](https://github.com/kstenerud/Musashi) was written by somebody else,
from the same manual, and has run inside several emulators for years. The commit
is pinned in `docker/musashi/Dockerfile` for the same reason vasm is: if the
test result changes, it has to be because my code changed.

It is not part of the runtime — it is a test reference, like BlastEm.

## How it works

**Real instructions from the ROM**, not random opcodes: the test covers exactly
what the game uses. One sample of each *form* (mnemonic + suffix + addressing
modes), translated through the same path that generates the game.

Each case runs on both sides from the same CPU state and the same 16 MB memory
image. Everything that remains is compared: 8 data registers, 8 address
registers, the inactive stack pointer, the CCR, a hash of RAM, and the address
of the first write.

The comparison is a **text diff** between two outputs of identical format. There
is no comparison logic inside the harnesses, and therefore nowhere to hide a
mistake that makes the test pass.

Initial values are biased toward the **edges** — `$7FFFFFFF`, `$80000000`,
`$FFFF`, `$80`. A uniform 32-bit draw almost never produces those, and that is
exactly where sign and overflow bugs live.

## What it does NOT cover, deliberately

**Control flow** (`bra`/`bcc`/`dbra`/`jmp`/`jsr`/`rts`). The recompiler does not
translate them as instructions but as C structure — `goto`, a call, `return`.
Testing one in isolation would be meaningless; what validates them is the game
running.

**Exceptions.** Division by zero and address errors change PC and stack in
Musashi; my side does not model that. Those cases are detected by the PC and
counted separately — 133 out of 739,451 — rather than swept under the rug.

## The four bugs it found on the first run

### 1. `ADDQ`/`SUBQ` with an address register destination

An `An` destination changes the whole instruction: the operation is always
**32-bit** and **affects no flags**, even when the instruction carries a word
size suffix. This is not
a special case of `ADDA`/`SUBA` — it is the 68000's rule for any `An`
destination.

I was adding in 16 bits and sign-extending on write: a word-sized quick add of 1
to `a1` produced `$FFFF8973` instead of `$00FF8973`. And a long-sized quick add
of 4 to `a1` cleared a CCR that should have stayed intact.

### 2. `ASL` is not `LSL`

The bit pattern is the same, which is why the mapping went unnoticed. The
difference is **overflow**: `LSL` always clears V; `ASL` sets V when the sign bit
changes at any point during the shift — the only way a program can notice the
value no longer fit.

### 3. BCD: adjust by 6, do not subtract 10

Both forms give the same result for valid digits (0-9). They diverge when a
digit is A-F — a situation the manual calls undefined, but which the silicon
resolves in a specific way. The carry comes from the comparison against `$99`,
not from each digit separately, and that was what made `sbcd` return `$FF` where
the hardware returns `$9F`.

### 4. Stack swap when the supervisor bit changes

The 68000 has two stack pointers and only one is visible at a time. Writing an
SR with a different S bit **swaps A7**. The game always runs in supervisor mode,
so in practice this never fires — but "never fires" is a claim about the game,
not about the CPU, and the test compares against the CPU.

## Three traps in the test itself

They cost more time than the bugs, and all of them produced **false
positives** — the worst kind of defect in a test, because it sends you hunting
for a bug that is not there.

**`m68k_execute(1)` is not "execute one instruction"**, it is "spend up to 1
cycle". A 16-cycle instruction leaves the counter at -15, and the next call
starts in the red and executes nothing. The reference was simply skipping
instructions. Looping until the PC changes defines "one step" without depending
on the cycle model.

**Restoring only RAM was not enough.** My side does not fetch instructions from
memory — the translation is compiled. Musashi does fetch. A case that wrote over
another case's bytes made the reference execute different code from mine, and
the divergence showed up looking like instruction semantics. The fix is a
**write journal**: record the previous value of every byte written and undo in
reverse order, exactly, anywhere in the 16 MB.

**`docker run ... | head -20` masked compile failures.** A pipeline's status is
that of its last command, and `head` always returns 0 — so the previous run's
binary stayed in place and I was comparing a new side against an old one. Same
mistake `remote_vasm.sh` had already learned, repeated in a new file.

What resolved all three was the same thing: when a result does not close,
compute the expected answer by a **third independent path**. Reproducing the
instruction in isolation showed Musashi was right; recomputing the RAM hash in
Python showed I was right. Two sources disagreeing do not say who is wrong.
Three do.

## Usage

```bash
python tools/difftest.py                                 # default: fast
python tools/difftest.py --sementes 256 --por-forma 12   # wide sweep
python tools/difftest.py --apenas cmpa                   # one mnemonic only
python tools/difftest.py --so-gerar                      # generate without running
```
