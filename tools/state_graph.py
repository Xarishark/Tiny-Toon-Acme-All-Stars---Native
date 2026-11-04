"""Extrai o grafo de estados do jogo.

Todo estado e' trocado por ``SetGameState`` ($00053C), que grava ``d0`` em
``$FFFB04``. Como os 28 call sites carregam ``d0`` com um imediato pouco antes
do salto, da' para reconstruir "de qual rotina vai para qual estado" sem rodar
o jogo.

O que a analise estatica **nao** da' e' o significado de cada estado — qual e'
a tela de titulo, qual e' a partida. Isso vem de observacao no emulador e vive
em ``analysis/symbol-names.csv``.

Uso:
    python tools/state_graph.py            # grava analysis/state-graph.csv
    python tools/state_graph.py --dry-run
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import Rom, hexa, project_root  # noqa: E402
from trace_code import Trace, load_dispatchers, load_jump_tables  # noqa: E402

SET_GAME_STATE = 0x53C
OUTPUT = project_root() / "analysis" / "state-graph.csv"
LOOKBACK = 10

# Instrucoes que redefinem d0 de um jeito que nao e' um imediato: se aparecerem
# antes do imediato, o valor vem de outro lugar e nao da' para afirmar nada.
CLOBBERS_D0 = {"move", "moveq", "add", "sub", "lsr", "lsl", "and", "or", "eor"}


def immediate_into_d0(insn) -> int | None:
    """Valor imediato carregado em d0, ou None se a instrucao nao faz isso."""
    op = insn.op_str
    if insn.mnemonic == "clr" and op.strip() == "d0":
        return 0
    if insn.mnemonic in ("move", "moveq") and op.startswith("#") and op.endswith("d0"):
        return int(op.split(",")[0].lstrip("#$"), 16)
    return None


def find_new_state(trace: Trace, addrs: list[int], index: dict[int, int],
                   site: int) -> int | None:
    for step in range(1, LOOKBACK + 1):
        pos = index[site] - step
        if pos < 0:
            return None
        prev = trace.insns[addrs[pos]]
        value = immediate_into_d0(prev)
        if value is not None:
            return value
        if prev.op_str.endswith("d0") and prev.mnemonic in CLOBBERS_D0:
            return None      # d0 foi calculado, nao carregado de um imediato
    return None


def owner(functions: list[tuple[int, int, str]], addr: int) -> str:
    """Nome da funcao que contem o endereco (a mais interna que couber)."""
    name = "?"
    for start, end, fname in functions:
        if start <= addr < end:
            name = fname
    return name


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Grafo de estados do jogo")
    parser.add_argument("--rom")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    rom = Rom.load(args.rom)
    trace = Trace(rom, load_jump_tables(), load_dispatchers())
    trace.run()

    functions = sorted(
        (int(f["address"].lstrip("$"), 16), int(f["end"].lstrip("$"), 16), f["name"])
        for f in csv.DictReader(
            (project_root() / "analysis" / "functions.csv").open(encoding="utf-8")))

    addrs = sorted(trace.insns)
    index = {a: i for i, a in enumerate(addrs)}

    rows = []
    for addr in addrs:
        if trace.insns[addr].target != SET_GAME_STATE:
            continue
        new_state = find_new_state(trace, addrs, index, addr)
        rows.append([hexa(addr), owner(functions, addr),
                     "" if new_state is None else new_state,
                     trace.insns[addr].text])

    resolved = sum(1 for r in rows if r[2] != "")
    print(f"{len(rows)} sites saltam para SetGameState ({hexa(SET_GAME_STATE)}); "
          f"{resolved} com o estado destino resolvido")

    targets: dict[object, int] = {}
    for row in rows:
        targets[row[2]] = targets.get(row[2], 0) + 1
    for state in sorted(targets, key=lambda s: (s == "", s if s != "" else 0)):
        label = state if state != "" else "nao resolvido"
        print(f"  -> estado {label}: {targets[state]} site(s)")

    if not args.dry_run:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        with OUTPUT.open("w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(["site", "in_function", "new_state", "instruction"])
            writer.writerows(rows)
        print(f"\nGravado em {OUTPUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
