# 18 — Recompilation

> Status: **100% of instructions translate** and every function compiles without
> a single error. The last unresolved indirect site, `$020C08`, was closed —
> see the end of this document.

Static translation of 68000 to C. Each instruction becomes C over a CPU struct;
memory access goes through a bus. There is no interpreter at run time.

## What exists

```
tools/recompile.py      translates one traced function to C
runtime/m68k.h          CPU state and flag helpers
runtime/memory.[ch]     the bus: ROM, RAM, hardware
tools/difftest.py       generates, compiles, runs and compares against Musashi
```

## Design decisions

**Flags computed eagerly.** Every instruction that affects flags computes N, Z,
V, C and X immediately. It is slower than computing on demand, and that is
deliberate: correctness is worth more than speed here. Optimising later is easy;
discovering a flag has been wrong for a thousand functions is not.

**Registers as `uint32_t` with mandatory sized access.** On the 68000, writing
`.b` or `.w` to a data register **preserves** the upper bits. `set_d16` exists to
make that impossible to get wrong — writing `cpu->d[0] = v` would be the easiest
bug to commit here.

Address registers are the opposite: a `.w` write is sign-extended to 32 bits.
That is why `set_a16` exists separately.

**An unsupported instruction is an error, not a warning.** The translator raises
`Unsupported`. No silently wrong C for something not yet understood.

## Coverage

`python tools/recompile.py --coverage` measures what fraction of the 27,690
traced instructions translate:

```
translatable: 27,690  (100.0%)
```

`python tools/recompile.py --all DIR` generates one `.c` per function: 1,948
files, all compiling with `-Wall -Wextra` without a single error.

The translator's first version stopped at **26.4%**, and the diagnosis was
right: the bottleneck was not the mnemonics, it was the **addressing modes**.
`move` showed 3,944 missing not because `move` did not exist, but because it only
accepted immediate, register and absolute.

Implementing the modes (`(aN)`, `(aN)+`, `-(aN)`, `d(aN)`, `d(aN,Xn)`,
PC-relative) plus control flow took it from 26.4% to 96.9% in one step.

### MOVEM cannot reuse the operand machinery

`movem` transfers several registers at once, and the `-(aN)` mode behaves
**differently** from normal: in a `move.l dN,-(aN)` the pointer moves once; in
a `movem.l <register list>,-(aN)` it moves once **per register**, and in reverse
order — `a7` first, `d0` last.

Reusing the `Operand` class, which decrements once, would generate silently wrong
code. So `movem` has its own translator.

Another quirk: in `movem.w`, the loaded value is sign-extended to 32 bits **even
in a data register** — the exception to the rule that a `.w` write preserves the
upper half.

## Control flow

A branch inside the same function becomes `goto`; outside, a call followed by
`return` — a tail call. `bsr`/`jsr` become function calls. Labels are emitted
**only** where something branches, otherwise the C becomes unreadable for no
gain.

The tail of `SinMul` (a PC-relative indexed table read into a data register, a
conditional branch on N, a long negate, an `rts`) comes out with this shape:

```c
    /* $ADDR1  <table read, PC-relative indexed> */
    { uint16_t v = md_read16((TABLE + (uint32_t)(int32_t)(int16_t)cpu->d[I]));
      set_d16(cpu, R, v);
      flags_logic16(cpu, v); }
    /* $ADDR2  <branch if plus> */
    if (!cpu->n) goto L_TARGET;
    /* $ADDR3  <long negate of dR> */
    set_d32(cpu, R, op_neg32(cpu, d32(cpu, R)));
L_TARGET:
    /* $TARGET  <rts> */
    return;
```

Each instruction keeps its ROM address and text as a comment, a branch target
becomes a label, and the flags are computed by the runtime helpers.

## Indirect dispatch

Table jumps become a `switch`. The translator does **not** try to recognise the
idiom: whether it is `jmp (aN)`, `jsr (aN,dN.w)` or `jmp BASE(pc,dN.w)`, the
operand already gives the destination address expression, and the cases are the
targets the analysis catalogued in `analysis/jump-tables.csv`.

The main state dispatcher (an indirect jump through an address register, at
`$0004B4`) comes out with this shape:

```c
    /* $ADDR  <indirect jump through aN> */
    { uint32_t target = cpu->a[N];
      switch (target) {
        case 0xTARGET0u: State_A(cpu); return;
        case 0xTARGET1u: State_B(cpu); return;
        ...
        default: md_unresolved_dispatch(0xADDRu, (uint32_t)(target)); return;
      }
    }
```

There is one `case` per target catalogued for that site, each calling the
function by its name from `symbol-names.csv` (here, `State_08_Boot`,
`State_02_SegaLogo` and the other game states).

The `default` **aborts**. That is not gratuitous strictness: a dispatch outside
the table means the static analysis is incomplete, and continuing would execute
the wrong target. The error points at the file that needs the entry.

## The `jsr` that does not return

`SubStateDispatch` (`$0005CE`) is the case that breaks the naive translation. It
pops the long at the top of the stack into `a0` — it **pops the return
address** to use as the base of
the inline table that follows the `jsr`. Translated as a C call, control would
return to the next instruction, which is actually the table's bytes.

The correct translation is a `switch` on the index, ending in `return`:

```c
    /* $ADDR  <jsr to SubStateDispatch> */
    switch (d16(cpu, 0) & 0x7F) {
      case 0: sub_ENTRY0(cpu); return;
      case 1: sub_ENTRY1(cpu); return;
      case 2: sub_ENTRY2(cpu); return;
      default: md_unresolved_dispatch(0xADDRu, ...); return;
    }
```

(The first site of this kind is at `$00131A`.) The index is `d0 & $7F` because
the dispatcher shifts `d0` left by one bit and then masks it with `$FF`: the word
offset is even, so the entry index is half of it.

Worth noting that this case was only tractable because the disassembly phase came
first. The inline tables were already catalogued; going straight to the
recompiler would have made this appear as a mysterious runtime bug.

## Two 68000 quirks the code had to respect

**The Z flag of ABCD, SBCD and SUBX is cumulative.** These instructions never
*set* Z — they only clear it when the result is non-zero. That is how a BCD
addition chained across several bytes can say "the total was zero" at the end.
Treating Z as in other instructions would break multi-digit score comparison,
which is exactly where the game uses BCD.

**MOVEM.W sign-extends on load**, even into a data register — the exception to
the rule that a `.w` write preserves the upper half.

Neither appears if you translate instruction by instruction without consulting
the manual.

## A trap that showed up twice

The translator's first version generated `md_read16(0xFB02u)` for a word read
of the short absolute address `$FB02`. The correct form is `0xFFFFFB02` — the 68000 sign-extends
short absolute addresses.

It is **exactly** the same mistake vasm refused to assemble during the reassembly
phase (*absolute short address out of range*, see
[21-build-system.md](21-build-system.md)). There the assembler caught it; here it
would have passed silently, reading ROM at `$00FB02` instead of RAM at
`$FFFFFB02`.

The lesson: the assembler is a checker the C compiler does not have. Everything
the reassembly taught about addressing semantics has to be deliberately
reapplied here.

## CMPA: the bug that required going from a pixel to an operand

Reported symptom: on the character select screen, both selection wheels sat in
the same corner. The path to the cause is worth more than the fix, because each
step eliminated an entire layer.

**1. Is the renderer right?** The sprite table in VRAM already had every piece of
the wheel at X 159-275, Y 30-105. If the data arrives wrong, the renderer is
faithful and the problem is logic. One question, half a layer gone.

**2. Which object?** `md_dump_ram` dumps all 64 KB, and the object array
(`$FFA000`, 55 entries of `$80`) showed the full picture: objects 9-19 and 20-31
formed two wheels with the **same centre (232,81)**.

**3. Who writes it?** `md_watch` on the object position returned the stack in the
game's own function names, up to `sub_01CBA4`. Further in, the centre came from
`$42(a0)`/`$44(a0)`, written by `sub_01CB10` — **twice in the same frame**, first
(88,172) and then (232,84).

**4. The assembly.** The choice between the two centres is one line. At
`$01CB12` the routine writes the centre (88,172), bottom left, into `$42(a0)`.
At `$01CB1E` a word-sized CMPA checks whether `a0` is below `$FFAA00`; at
`$01CB22`, if it is, a branch to `$01CB32` keeps that centre. Otherwise, at
`$01CB26`, it overwrites it with (232,84), top right.

`cmpa` compares in 32 bits, but reads its source at the **instruction's size**
and **sign-extends** it. My translator was reading the source as a long:

```c
flags_cmp32(cpu, cpu->a[N], (uint32_t)imm16);                     /* wrong */
flags_cmp32(cpu, cpu->a[N], (uint32_t)(int32_t)(int16_t)imm16);   /* right */
```

For the comparison against `$FFAA00`, the 16-bit immediate is `$AA00`: the
wrong form compared against `0xAA00`, the right one against `0xFFFFAA00`.

And since **every Mega Drive RAM address has `$FFFF` on top**, the comparison
always produced the opposite result. It was not a rare case: it was every
`cmpa.w` against a RAM pointer, and there are **196 in the ROM**.

The lesson is about where the error was and where it appeared. The bug is a
missing sign extension; the symptom was two character wheels in the wrong corner,
four levels of abstraction above. `ADDA`/`SUBA`/`MOVEA` were already correct —
the same rule, written three times and forgotten on the fourth. When a family of
instructions shares a rule, it is worth checking the whole family at once.

## `$020C08`: the last unresolved dispatch, and how the game found it

For a long time two functions could not be translated, both containing the
indexed indirect JSR through `a1` (with `d0` as the index) at `$020C08` — an
indirect call through an offset table relative
to a register whose base comes from the caller. The recompiler emitted a stub
that deliberately aborts rather than execute garbage.

It stayed harmless until somebody finished the racing stage, where the game
reaches it for the first time. The log said exactly which function, and the
translation was one table registration away.

Two different callers load two different bases: `$0088AE` (one) and `$008C04`
(three). Each table's extent was proven a different way, and neither was "it
looks like twelve":

- **`$008C04`, structurally.** With 13 entries, entry [0] would fall *inside the
  table itself*. With 12 it points at `$008C1C`, exactly the first byte past the
  end.
- **`$0088AE`, from the bytes.** There are 12 words from `$0088AE` to `$0088C5`,
  and at `$0088C6` real code begins (a word read of `$FFE02A` into `d0`). Entry
[12] would be
  reading that instruction's bytes as a table entry.

Registered in `jump-tables.csv` with the evidence, the tracer's all-or-nothing
validator accepted both, and **1,076 previously unreachable instructions** joined
the analysis. Unresolved indirect flow went to zero.

That the byte-identical gate still passed afterwards is the point: a thousand new
instructions were emitted as assembly and reassembled to the same bytes.
