"""Relatorio e verificacao de integridade da ROM.

Uso:
    python tools/rom_info.py                 # relatorio legivel
    python tools/rom_info.py --vectors       # inclui a tabela de vetores
    python tools/rom_info.py --json          # saida em JSON
    python tools/rom_info.py --check         # compara com analysis/rom-baseline.json
    python tools/rom_info.py --write-baseline

``--check`` e' o portao de regressao do projeto: qualquer ROM reconstruida
(reassembly) precisa passar nele com codigo de saida 0.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import HEADER_FIELDS, Rom, hexa, project_root, region_of  # noqa: E402

BASELINE_PATH = project_root() / "analysis" / "rom-baseline.json"


def build_facts(rom: Rom) -> dict:
    text = rom.header_text()
    num = rom.header_numeric()
    return {
        "file": rom.path.name,
        "size": rom.size,
        "md5": rom.md5,
        "sha1": rom.sha1,
        "header": text,
        "checksum_stored": num["checksum"],
        "checksum_computed": rom.checksum_computed(),
        "rom_start": num["rom_start"],
        "rom_end": num["rom_end"],
        "ram_start": num["ram_start"],
        "ram_end": num["ram_end"],
        "has_sram": rom.has_sram(),
        "initial_sp": rom.initial_sp,
        "entry_point": rom.entry_point,
    }


def print_report(rom: Rom, show_vectors: bool) -> None:
    facts = build_facts(rom)
    print(f"Arquivo            : {facts['file']}")
    print(f"Tamanho            : {facts['size']} bytes ({facts['size'] // 1024} KB)")
    print(f"MD5                : {facts['md5']}")
    print(f"SHA-1              : {facts['sha1']}")
    print()
    for key, _off, _len, label in HEADER_FIELDS:
        value = facts["header"][key]
        print(f"{label:19}: {value if value else '(em branco)'}")
    print()
    stored, computed = facts["checksum_stored"], facts["checksum_computed"]
    status = "CONFERE" if stored == computed else "DIVERGE"
    print(f"{'Checksum header':19}: {hexa(stored, 4)}  calculado {hexa(computed, 4)}  [{status}]")
    print(f"{'Faixa de ROM':19}: {hexa(facts['rom_start'])}-{hexa(facts['rom_end'])}")
    print(f"{'Faixa de RAM':19}: {hexa(facts['ram_start'])}-{hexa(facts['ram_end'])}")
    print(f"{'SRAM':19}: {'presente' if facts['has_sram'] else 'nenhuma'}")
    print(f"{'SP inicial':19}: {hexa(facts['initial_sp'], 8)}")
    print(f"{'Entry point':19}: {hexa(facts['entry_point'])}")

    if not show_vectors:
        return

    print("\nTabela de vetores:")
    vectors = rom.vectors()
    stub_counts: dict[int, list[int]] = {}
    for vec in vectors[2:]:
        stub_counts.setdefault(vec.value, []).append(vec.index)
    for vec in vectors:
        region = "" if vec.index == 0 else f"  [{region_of(vec.value)[0]}]"
        print(f"  {vec.index:2d} {hexa(vec.offset, 3)}  {hexa(vec.value, 8)}  {vec.name}{region}")
    shared = {v: idx for v, idx in stub_counts.items() if len(idx) > 1}
    if shared:
        print("\n  Destinos compartilhados por varios vetores (provaveis stubs):")
        for value, indices in sorted(shared.items()):
            names = ", ".join(str(i) for i in indices)
            print(f"    {hexa(value, 8)} <- vetores {names}")


def check(rom: Rom) -> int:
    if not BASELINE_PATH.exists():
        print(f"Baseline ausente: {BASELINE_PATH}", file=sys.stderr)
        print("Gere com: python tools/rom_info.py --write-baseline", file=sys.stderr)
        return 2

    baseline = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    facts = build_facts(rom)
    failures = []

    for key in ("size", "md5", "sha1", "checksum_stored", "checksum_computed",
                "initial_sp", "entry_point"):
        if facts[key] != baseline[key]:
            failures.append(f"{key}: esperado {baseline[key]!r}, obtido {facts[key]!r}")
    for key, expected in baseline["header"].items():
        if facts["header"].get(key) != expected:
            failures.append(f"header.{key}: esperado {expected!r}, obtido {facts['header'].get(key)!r}")
    if facts["checksum_stored"] != facts["checksum_computed"]:
        failures.append("checksum do header nao confere com a soma calculada")

    if failures:
        print("FALHOU - divergencias em relacao a baseline:")
        for line in failures:
            print(f"  - {line}")
        return 1

    # Sem caracteres fora de ASCII: o console do Windows usa cp1252 por padrao.
    print(f"OK - {rom.path.name} confere com {BASELINE_PATH.name} "
          f"({len(baseline['header'])} campos de header, hashes e checksum).")
    return 0


def write_baseline(rom: Rom) -> int:
    BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
    facts = build_facts(rom)
    facts.pop("file")  # o nome do arquivo nao faz parte da identidade da ROM
    BASELINE_PATH.write_text(json.dumps(facts, indent=2, ensure_ascii=False) + "\n",
                             encoding="utf-8")
    print(f"Baseline gravada em {BASELINE_PATH}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Relatorio e verificacao da ROM de Mega Drive")
    parser.add_argument("--rom", help="caminho da ROM (padrao: autodetecta em bin/)")
    parser.add_argument("--vectors", action="store_true", help="lista a tabela de vetores")
    parser.add_argument("--json", action="store_true", help="saida em JSON")
    parser.add_argument("--check", action="store_true", help="compara com a baseline")
    parser.add_argument("--write-baseline", action="store_true", help="grava a baseline")
    args = parser.parse_args(argv)

    rom = Rom.load(args.rom)

    if args.write_baseline:
        return write_baseline(rom)
    if args.check:
        return check(rom)
    if args.json:
        print(json.dumps(build_facts(rom), indent=2, ensure_ascii=False))
        return 0
    print_report(rom, args.vectors)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
