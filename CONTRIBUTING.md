# Contributing

Everything here is open. What follows is not bureaucracy — it is the one rule
that makes the rest of the project trustworthy, plus the practical steps.

## The rule: do not guess

**An address is code only when there is a proven execution path to it. A name
only enters with the evidence that supports it.**

This is not fussiness. A guess recorded as fact contaminates everything
downstream, silently, and you find out months later when a table that was
"obviously twelve entries" turns out to be seven and 5 KB of analysis was built
on sand.

Concretely:

- `analysis/symbol-names.csv` has an `evidence` column. **If you cannot fill it
  in, the name does not go in.** "It looks like a sound routine" is not
  evidence. "213 callers, writes `d0.b` into a ring at `$FFD410` and advances the
  index at `$FFD510`" is.
- `analysis/jump-tables.csv` needs the *count* justified, not just the base. The
  ROM stores table sizes nowhere. See below for what counts as proof.
- If you are not sure, say so in the `confidence` column (`alta` / `media`) or
  leave it out. An honest gap is worth more than a confident error.

The project has broken this rule and paid for it — twice, both recorded in the
docs. [19-native-runtime.md](docs/19-native-runtime.md) has an entry that begins
"And here I made a mistake", about a table entry added with the note "role not
yet investigated". It left interrupts masked forever.

## What counts as proof for a table's extent

The hard problem is not finding a jump table, it is knowing where it ends. What
has actually worked, in rough order of strength:

1. **An entry would fall inside the table itself.** At `$008C04`, 13 entries
   would make entry [0] land within its own bounds. So there are 12. Structural,
   not interpretive.
2. **Code begins immediately after.** At `$0088AE`, there are 12 words and then
   `$0088C6` decodes as a word load from RAM into `d0` — real code. Entry [12] would be
   reading that instruction's bytes.
3. **Unused slots point at a shared `rts`.** `$00DB8A` has 24 of its 30 slots
   pointing at the same `rts`.
4. **The table ends where another begins.**
5. **The code bounds the index.** A comparison of the index register against a
   constant before the dispatch says something about the range.

The tracer then validates all-or-nothing: every entry must be in the ROM, even,
decode as a valid instruction, and not fall inside the table. **One bad entry
rejects the whole table.** Accepting the good prefix was tried and it is worse —
the entries before a failure are not necessarily good.

## The gates

Run these before opening a pull request. They exist so that a change either
proves itself or announces its failure.

```bash
./verify.sh                 # ROM identity, static trace, coverage
```

If you changed `analysis/` or the emitter, the one that matters is the
**byte-identical reassembly**: the emitted assembly, assembled by vasm, must
reproduce the original ROM exactly. A name never changes a byte; a wrongly
declared table does.

If you changed the recompiler or `runtime/m68k.h`, run the differential test
against Musashi. If you changed `tools/codecs.py`, run the codec gate. Both need
reference implementations described in [CREDITS.md](CREDITS.md) — they are not
vendored here.

Two tracer metrics are the contamination alarm: **invalid decodes** and
**overlaps**. Both are currently zero. Any non-zero value means some declared or
recognised table is wrong. Do not merge past them.

## The workflow that grows coverage

There is exactly one:

```
python tools/trace_code.py         # run the tracer
open analysis/unresolved-flow.csv  # the work queue
                                   # investigate ONE indirect target
                                   # record what you CONFIRMED
python tools/trace_code.py         # run again, watch coverage rise
```

`analysis/` splits into two classes and confusing them breaks everything:

| | |
|---|---|
| **input**, hand-curated, versioned | `jump-tables.csv`, `dispatchers.csv`, `symbol-names.csv`, `ram-names.csv`, `data-regions.csv` |
| **output**, regenerable | `functions.csv`, `symbols.csv`, `rom-map.csv`, `ram-map.csv`, `unresolved-flow.csv`, `rom-leituras.csv` |

Never hand-edit an output file. The tracer overwrites it.

`tools/dump.py` is the inspection bench:

```bash
python tools/dump.py 0x9c8 -n 12        # disassembly
python tools/dump.py 0x9e4 --hex 64     # hex
python tools/dump.py 0x000b34 --table 9 # words as offsets relative to a base
python tools/dump.py 0x0004b6 --ptr 16  # longs as pointers
```

## Good first contributions

**Play the game with recording on.** Genuinely the highest-value thing, and it
needs no code:

```bash
TINYTOON_ROMLIDO=romlido.bin TINYTOON_GRAVAR=session.txt ./tinytoon rom.bin
```

The first makes your session raise ROM coverage — the game reaches its data
through pointer tables that static analysis cannot follow, so playing *is* the
extraction tool. The second makes it a byte-reproducible replay, which is a
permanent regression test. Neither costs measurable performance.

**Name something in `$FFA600`–`$FFA6FF`.** It is the hottest struct in the ROM
and has not a single named field. `$FFA622` alone has 200 references.

**Review `analysis/jump-tables-detected.csv`.** Those are tables the tracer
recognised automatically and nobody has checked one by one.

**Try the macOS build.** Nobody has. The result is worth reporting either way.

**Write a mod.** This is the one kind of contribution with no analysis rules
attached — a mod lives outside the fidelity discipline by construction, because
it cannot change what the game does. `mods/filtro-cor/` is a template; the ABI
is one header. HUD overlays, telemetry that helps name the unknown fields of
`$FFA6xx`, palette experiments, CRT filters. See [docs/27-mods.md](docs/27-mods.md).

## Reporting a bug

The single most useful thing you can attach is **`tinytoon.log`**, written beside
the executable in a `--release` build. It contains the build version, the ROM
verdict, what controllers were detected and by which path, cycle budget
overruns, unimplemented hardware accesses, sprite-per-line peaks and the audio
queue counters.

If the game aborted, the message names the exact cause. `funcao nao traduzida
alcancada: $XXXXXX` and `despacho fora de tabela` are not crashes — they are the
port refusing to execute something the analysis has not resolved, and they name
the address that needs work.

If it hung, a development build prints a stack trace in the game's own function
names.

Please include which ROM you used — the log records both SHA-1s when yours is not
the one the project validates against.

## What will be turned down

- A name or table entry without evidence.
- A change that makes the byte-identical gate fail, unless the change *is* about
  the emitter and the new bytes are explained.
- Modern improvements that change observable game behaviour, before fidelity
  work is done. Higher resolution, widescreen and a decoupled frame rate are all
  wanted — they belong to a later phase, on purpose, so that bugs introduced by
  the port are never mistaken for behaviour of the original.

  Mods are the exception, and by construction rather than by exemption: the mod
  ABI cannot write game state, so a mod cannot change what the game *does*. That
  is why mods could ship before the rest.
- Anything that puts the ROM, or data derived from it, into the repository.

## Language

The code, comments and documentation here are in English. The original research
notes were written in Portuguese and have been translated; if you find a
translation that has drifted from the code, fixing it is a welcome contribution.
