# 21 — Build system

## Two builds, two purposes

| | |
|---|---|
| `./build.sh` | native, for the machine you are on. Development: console output, PNG dumps on exit. |
| `./build-docker.sh [linux\|windows]` | in a container, nothing installed but Docker. Also the only cross-compile path. |

Both do the same five steps: check the toolchain, verify the ROM, translate the
68000 code to C, generate headers, compile in parallel. Details and
troubleshooting in [BUILDING.md](../BUILDING.md).

The step that cannot ship pre-built is the third: the generated C is derived from
the ROM, so it is produced on your machine from your dump. That is not a
workaround — it is the honest structure of the project.

## The reassembly gate

This is the gate that makes the disassembly trustworthy rather than merely
plausible.

```
tools/emit_asm.py          →  build/tinytoon.asm     (Python)
vasm                       →  build/tinytoon.bin     (in a container)
cmp                        →  byte-identical?
tools/rom_info.py --check  →  header, checksum and hashes match?
```

It exits 0 **only** if the rebuilt ROM is byte-identical to the original. Run it
after any change to `analysis/` or to the emitter. It is what stops a badly
declared jump table or a mis-decoded instruction from entering the analysis
unnoticed.

Current state: **passes with all 27,690 instructions emitted as real 68000
code**, without a single exception. The whole path capstone → text → vasm →
bytes is reversible for 100% of the identified code. Regions not yet classified
come out as `dc.b`.

The source has thousands of labels — one at every function entry and every
branch target — and every `bra`/`bcc`/`bsr`/`jmp`/`jsr` destination is written
by name, not by address. Labels are only generated at addresses the tracer
confirmed to be instruction starts.

The gate was verified by a **negative test**: changing the immediate of one
quick-load instruction in the emitted source from 0 to 1 makes `cmp` report a
divergence at byte 568. It is not vacuous.

## The vasm container

`docker/vasm/Dockerfile` builds [vasm](http://sun.hasenbraten.de/vasm/) with the
M68k backend and Motorola syntax, as a two-stage build so the final image
carries only the binary.

Upstream always publishes at `.../release/vasm.tar.gz`, with no versioned URL, so
the only anchor for reproducibility is the **pinned sha256** in the Dockerfile.
If upstream publishes a new version, the build fails at verification instead of
silently swapping the assembler underneath a project that depends on byte-exact
output.

Versions in use: vasm 2.0f, M68k/CPU32/ColdFire backend 2.8c, Motorola syntax
3.19f.

## Emission rules that preserve byte identity

Assemblers optimise by default. Any encoding choice different from the original
breaks equality. What has been verified against vasm 2.0f:

- **Explicit size suffixes are respected.** `bra.w` to a target that would fit in
  `.b` is still assembled as `bra.w`, even without `-no-opt`. The emitter must
  write the size of every instruction; `-no-opt` stays as a seatbelt.
- **Short absolute addresses must be sign-extended.** The 68000 sign-extends
  `.w`, so an absolute short operand written as `$F700.w` is rejected (*absolute
  short address out of range*) and the correct form is `$FFFFF700.w`. Capstone prints the
  short form without extending — the emitter has to convert.
  `romlib._sign_extend_short` holds that logic.
- **Bit instructions take no suffix.** `btst`, `bset`, `bclr` and `bchg` have no
  size field in the opcode: the size is implied by the destination (data
  register = long, memory = byte), so there is exactly one encoding per operand
  form. Capstone prints a bit test against memory with a `.l` suffix, and vasm
  rejects it with *illegal opcode extension*. The emitter omits the suffix and lets the assembler pick the only
  possible encoding.
- **Source in pure ASCII**, no accents or em dashes.

These are exactly the kind of detail that only appears when you try to assemble,
which is why the reassembly came before growing coverage further. The first
three were the **only** corrections needed for 100% of the code to round-trip:
`analysis/emit-fallbacks.csv` — the list of instructions that would have to be
emitted as `dc.w` with the original bytes — is empty.

Contrary to expectation, branches and PC-relative addressing caused no trouble
at all: vasm recomputes the displacements and arrives at exactly the same bytes,
because the emitter writes each instruction's size explicitly.

## Building ~1,950 translation units

The first version handed every file to a single `gcc`, which compiled them in
series and used one core out of sixteen. The Makefile now builds objects in
parallel, and incrementally.

Incremental depends on two things that are easy to break without noticing:

**Timestamps must be preserved** when sources move between machines. A file that
did not change must arrive with its old date, or `make` rebuilds it. Anything
that stamps the current date turns every build into a full build, silently.

**`-MMD -MP` generates header dependencies.** Without it, editing `m68k.h` would
rebuild nothing and the binary would come out inconsistent — the worst kind of
error, because it never appears at compile time.

Each target gets its own object directory: the same `.c` compiled for Linux and
for Windows does not produce the same object.

The source list excludes `runtime/testes/`, which contains standalone
verification programs with their own `main()`. A wildcard once dragged them into
the link and broke it with `multiple definition of main`, reported at the linker,
far from the cause.

## `build/` layout

Not versioned. Everything in it is regenerable from the ROM and `analysis/`.

| File | What it is |
|---|---|
| `recomp/` | the generated C, one file per routine. **Derived from the ROM** — never committed. |
| `obj/` | object files |
| `tinytoon.asm` | emitted source: 27,690 instructions plus `dc.b` regions |
| `tinytoon.map` | line → address map, to find the instruction behind a vasm error or a `cmp` divergence |
| `tinytoon.bin` | reconstructed ROM, 1,048,576 bytes |
