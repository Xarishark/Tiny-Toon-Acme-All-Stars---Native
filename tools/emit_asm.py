"""Gera o fonte assembly da ROM a partir do mapa de analise.

O objetivo e' o checkpoint central do projeto: reassemblar e obter uma ROM
**byte-identical** a original.

A emissao e' incremental por regiao:

- regioes ``code`` saem como instrucoes 68000, com label nomeado em cada
  ponto de entrada e alvo de desvio, e os destinos escritos por nome;
- todo o resto sai como ``dc.b``.

A identidade byte a byte e' a trava de regressao de toda a analise: qualquer
mudanca em ``analysis/`` ou aqui so' pode ser aceita se ``tools/rebuild.sh``
continuar verde. Um label em endereco errado, por exemplo, muda o
deslocamento calculado pelo assembler e o ``cmp`` acusa na hora.

Uso:
    python tools/emit_asm.py                  # grava build/tinytoon.asm
    python tools/emit_asm.py -o outro.asm
    python tools/emit_asm.py --data-only      # sem instrucoes, so' dc.b
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import (Rom, hexa, load_data_regions, load_ram_names,  # noqa: E402
                    project_root)
from trace_code import Trace, load_dispatchers, load_jump_tables  # noqa: E402

ROM_MAP_PATH = project_root() / "analysis" / "rom-map.csv"
FALLBACKS_PATH = project_root() / "analysis" / "emit-fallbacks.csv"
DEFAULT_OUTPUT = project_root() / "build" / "tinytoon.asm"

BYTES_PER_LINE = 16

# Endereco absoluto curto impresso pelo capstone, ex.: "$f700.w".
SHORT_ABSOLUTE = re.compile(r"\$([0-9a-fA-F]{1,4})\.w\b")

# Instrucoes de bit: o opcode nao tem campo de tamanho. O tamanho e' implicado
# pelo destino (registrador de dados = long, memoria = byte), entao existe uma
# unica codificacao possivel para cada forma de operando e o sufixo e' apenas
# ruido. O capstone imprime sufixos que o vasm rejeita (um btst com sufixo .l e destino em memoria).
BIT_INSTRUCTIONS = {"btst", "bset", "bclr", "bchg"}

# Endereco de RAM ja' estendido com sinal pelo emissor, ex.: "$fffffb04".
# A forma longa (".l") fica de fora: ela precisaria de um EQU com outro valor,
# e no fonte inteiro so' existem 9 refs assim, nenhuma em endereco nomeado.
RAM_SHORT = re.compile(r"(?<!#)\$(ffff[0-9a-f]{4})")


def build_labels(trace: Trace) -> tuple[dict[int, str], int]:
    """Monta o dicionario endereco -> label a partir do trace.

    Um label so' e' emitido em endereco que o tracer confirmou ser **inicio de
    instrucao**. Label no meio de uma instrucao mudaria o deslocamento
    calculado pelo assembler e quebraria a identidade byte a byte — que e'
    exatamente o que o portao pegaria, mas e' melhor nao gerar.
    """
    labels: dict[int, str] = {}
    skipped = 0

    for addr in sorted(trace.entries):
        if addr not in trace.insns:
            skipped += 1
            continue
        labels[addr] = trace.label_for(addr)
    for addr in sorted(trace.branch_targets):
        if addr in labels:
            continue
        if addr not in trace.insns:
            skipped += 1
            continue
        labels[addr] = f"Loc_{addr:06X}"

    # Nomes verificados a mao valem tambem para alvos de desvio, que por
    # padrao sairiam como Loc_XXXXXX.
    for addr, name in trace.names.items():
        if addr not in trace.insns:
            raise SystemExit(
                f"symbol-names.csv: {name} em {hexa(addr)} nao e' inicio de instrucao")
        labels[addr] = name

    if len(set(labels.values())) != len(labels):
        raise SystemExit("labels duplicados: nomes precisam ser unicos no fonte")
    return labels, skipped


def to_vasm(insn, labels: dict[int, str] | None = None,
            ram_names: dict[int, str] | None = None):
    """Traduz o texto do capstone para sintaxe aceita pelo vasm.

    O capstone imprime em sintaxe Motorola, mas com duas diferencas que
    importam (ver docs/21-build-system.md):

    - endereco absoluto curto sai sem extensao de sinal (``$f700.w``), e o
      vasm rejeita porque o 68000 estende ``.w`` com sinal — o correto e'
      ``$fffff700.w``;
    - o sufixo de tamanho e' sempre escrito, porque sem ele o assembler
      escolhe a codificacao e a identidade byte a byte se perde.
    """
    def sign_extend(match: re.Match) -> str:
        value = int(match.group(1), 16)
        return f"${value | 0xFFFF0000:08x}.w" if value & 0x8000 else match.group(0)

    operands = insn.op_str

    # Troca o destino numerico pelo label, preservando o sufixo de tamanho:
    # um jsr absoluto para $0013D0 vira um jsr para Sub_0013D0, com o .l. Sem o ".l" o vasm escolheria
    # absoluto curto para enderecos abaixo de $8000 e os bytes mudariam.
    substituted = None
    if labels and insn.target is not None and insn.target in labels:
        substituted = False
        name = labels[insn.target]
        for form, replacement in ((f"${insn.target:x}.l", f"{name}.l"),
                                  (f"${insn.target:x}.w", f"{name}.w"),
                                  (f"${insn.target:x}", name)):
            # Ancorado no fim: o destino e' sempre o ultimo operando (em dbcc
            # vem depois do registrador de contagem).
            if operands.endswith(form):
                operands = operands[:-len(form)] + replacement
                substituted = True
                break

    operands = SHORT_ABSOLUTE.sub(sign_extend, operands)

    # Troca o endereco de RAM pelo nome; o EQU no topo do fonte da' o valor.
    ram_hits = 0
    if ram_names:
        def name_ram(match: re.Match) -> str:
            nonlocal ram_hits
            name = ram_names.get(int(match.group(1), 16) & 0xFFFFFF)
            if not name:
                return match.group(0)
            ram_hits += 1
            return name
        operands = RAM_SHORT.sub(name_ram, operands)

    operands = operands.replace(", ", ",")
    suffix = "" if insn.mnemonic in BIT_INSTRUCTIONS else insn.suffix
    mnemonic = insn.mnemonic + (f".{suffix}" if suffix else "")
    return f"{mnemonic}\t{operands}".rstrip(), substituted, ram_hits


def load_fallbacks(path: Path = FALLBACKS_PATH) -> dict[int, str]:
    """Instrucoes que nao sobrevivem ao round-trip e saem como bytes crus."""
    if not path.exists():
        return {}
    out = {}
    with path.open(encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            if row.get("address", "").strip():
                out[int(row["address"].lstrip("$"), 16)] = row.get("reason", "")
    return out


def load_regions(path: Path, rom: Rom) -> list[dict]:
    """Le analysis/rom-map.csv; cai para uma unica regiao se ele nao existir."""
    if not path.exists():
        return [{"start": 0, "end": rom.size - 1, "kind": "unknown"}]
    regions = []
    with path.open(encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            regions.append({
                "start": int(row["start"].lstrip("$"), 16),
                "end": int(row["end"].lstrip("$"), 16),
                "kind": row["kind"].strip(),
            })
    regions.sort(key=lambda r: r["start"])

    # O mapa tem de cobrir a ROM inteira e sem buracos: qualquer byte que
    # escape aqui vira uma ROM reconstruida de tamanho errado.
    expected = 0
    for region in regions:
        if region["start"] != expected:
            raise SystemExit(
                f"rom-map.csv nao e' continuo: esperado {hexa(expected)}, "
                f"encontrado {hexa(region['start'])}")
        expected = region["end"] + 1
    if expected != rom.size:
        raise SystemExit(
            f"rom-map.csv cobre ate' {hexa(expected - 1)}, mas a ROM tem "
            f"{rom.size} bytes")
    return regions


class Emitter:
    """Escreve o fonte e mantem o mapa linha -> endereco.

    O mapa e' o que torna o ciclo de correcao viavel: quando o vasm reclama da
    linha N, ou quando o ``cmp`` acusa divergencia no offset X, da' para voltar
    a instrucao exata sem adivinhar.
    """

    def __init__(self, out):
        self.out = out
        self.line = 0
        self.line_to_addr: dict[int, int] = {}

    def write(self, text: str, addr: int | None = None) -> None:
        self.out.write(text)
        self.line += text.count("\n")
        if addr is not None:
            self.line_to_addr[self.line] = addr

    def data(self, data: bytes, start: int, end: int) -> None:
        for off in range(start, end + 1, BYTES_PER_LINE):
            chunk = data[off:min(off + BYTES_PER_LINE, end + 1)]
            self.write("\tdc.b\t" + ",".join(f"${b:02X}" for b in chunk) + "\n", off)

    def words(self, data: bytes, start: int, end: int, per_line: int,
              note: str = "") -> None:
        """Emite uma faixa como dc.w, com um comentario de indice por linha.

        Mesmos bytes que dc.b — o que muda e' a legibilidade: uma paleta de 16
        cores numa linha so' se le' como paleta, nao como 32 numeros soltos.
        """
        index = 0
        offset = start
        while offset + 2 * per_line <= end + 1:
            values = ",".join(f"${data[offset + 2 * k]:02X}{data[offset + 2 * k + 1]:02X}"
                              for k in range(per_line))
            comment = f"\t; {note} {index:02d}" if note else ""
            self.write(f"\tdc.w\t{values}{comment}\n", offset)
            offset += 2 * per_line
            index += 1
        if offset <= end:
            self.data(data, offset, end)

    def raw_words(self, data: bytes, start: int, end: int, note: str) -> None:
        """Bytes crus preservando o encoding original, para o que nao round-trips."""
        words = ",".join(f"${data[o]:02X}{data[o + 1]:02X}"
                         for o in range(start, end, 2))
        self.write(f"\tdc.w\t{words}\t\t; {note}\n", start)
        if (end - start) % 2:
            self.write(f"\tdc.b\t${data[end - 1]:02X}\n", end - 1)


def emit_code(em: Emitter, rom: Rom, trace: Trace, start: int, end: int,
              fallbacks: dict[int, str], stats: dict[str, int],
              labels: dict[int, str], ram_names: dict[int, str]) -> None:
    """Emite uma faixa de codigo como instrucoes, com recuo para bytes crus."""
    addr = start
    while addr <= end:
        if addr in labels:
            em.write(f"{labels[addr]}:\n", addr)
            stats["labels"] = stats.get("labels", 0) + 1
        insn = trace.insns.get(addr)
        if insn is None:
            # A faixa veio do bitmap de codigo do tracer, entao isto nao deveria
            # acontecer; se acontecer, preservar os bytes e' o unico caminho seguro.
            nxt = addr
            while nxt <= end and nxt not in trace.insns:
                nxt += 1
            em.raw_words(rom.data, addr, nxt, "sem instrucao no trace")
            stats["raw"] = stats.get("raw", 0) + (nxt - addr)
            addr = nxt
            continue
        if addr in fallbacks:
            em.raw_words(rom.data, addr, insn.end, fallbacks[addr] or insn.text)
            stats["fallback"] = stats.get("fallback", 0) + insn.size
        else:
            text, substituted, ram_hits = to_vasm(insn, labels, ram_names)
            em.write("\t" + text + "\n", addr)
            stats["ram_refs"] = stats.get("ram_refs", 0) + ram_hits
            if substituted is not None:
                stats["targets"] = stats.get("targets", 0) + 1
                stats["resolved"] = stats.get("resolved", 0) + int(substituted)
            stats["insn"] = stats.get("insn", 0) + insn.size
        addr = insn.end


# Quantas words por linha ao emitir cada tipo de faixa de dado.
WORDS_PER_LINE = {"palette": 16}


def emit(rom: Rom, regions: list[dict], out, trace: Trace | None,
         fallbacks: dict[int, str], labels: dict[int, str],
         ram_names: dict[int, str]) -> tuple[dict[str, int], Emitter]:
    em = Emitter(out)
    data_regions = {r["start"]: r for r in load_data_regions()}
    # Fonte em ASCII puro: e' o que todo assembler 68000 aceita sem surpresa.
    em.write("; Tiny Toon Adventures: ACME All-Stars - fonte reconstruido\n")
    em.write("; Gerado por tools/emit_asm.py. Nao editar a mao.\n")
    em.write(f"; ROM de origem: {rom.size} bytes, sha1 {rom.sha1}\n")
    em.write(";\n")
    em.write("; Montar e verificar:  tools/rebuild.sh\n\n")

    if ram_names:
        em.write("; Simbolos de RAM (analysis/ram-names.csv)\n")
        for addr, name in sorted(ram_names.items(), key=lambda kv: kv[1]):
            # Valor estendido com sinal: e' assim que o 68000 enxerga um
            # endereco absoluto curto, e e' o que o vasm exige.
            em.write(f"{name}\tequ\t${addr | 0xFFFF0000:08X}\n")
        em.write("\n")

    em.write("\torg\t$0\n\n")

    stats: dict[str, int] = {}
    for region in regions:
        size = region["end"] - region["start"] + 1
        stats[region["kind"]] = stats.get(region["kind"], 0) + size
        em.write(f"; {hexa(region['start'])}-{hexa(region['end'])}  "
                 f"{size} bytes  [{region['kind']}]\n")
        if region["kind"] == "code" and trace is not None:
            emit_code(em, rom, trace, region["start"], region["end"],
                      fallbacks, stats, labels, ram_names)
        elif region["start"] in data_regions:
            declared = data_regions[region["start"]]
            em.write(f"{declared['name']}:\n", region["start"])
            em.words(rom.data, region["start"], region["end"],
                     WORDS_PER_LINE.get(declared["kind"], 8), declared["kind"])
            stats["named_data"] = stats.get("named_data", 0) + size
        else:
            em.data(rom.data, region["start"], region["end"])
        em.write("\n")
    return stats, em


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Gera o assembly da ROM")
    parser.add_argument("--rom", help="caminho da ROM")
    parser.add_argument("-o", "--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument("--data-only", action="store_true",
                        help="emite tudo como dc.b, sem instrucoes")
    args = parser.parse_args(argv)

    rom = Rom.load(args.rom)
    regions = load_regions(ROM_MAP_PATH, rom)

    trace = None
    if not args.data_only:
        trace = Trace(rom, load_jump_tables(), load_dispatchers())
        trace.run()
    fallbacks = load_fallbacks()
    labels, skipped_labels = build_labels(trace) if trace else ({}, 0)
    ram_names = load_ram_names() if trace else {}

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="ascii", newline="\n") as fh:
        stats, em = emit(rom, regions, fh, trace, fallbacks, labels, ram_names)

    # Mapa linha -> endereco, consumido por tools/fix_emit.py para transformar
    # erro do vasm ou divergencia do cmp em endereco de instrucao.
    map_path = output.with_suffix(".map")
    with map_path.open("w", encoding="ascii", newline="\n") as fh:
        fh.write("line,address\n")
        for line, addr in sorted(em.line_to_addr.items()):
            fh.write(f"{line},{addr}\n")

    by_region = {k: v for k, v in stats.items() if k not in ("insn", "fallback", "raw", "labels", "targets", "resolved",
                              "ram_refs", "named_data")}
    total = sum(by_region.values())
    print(f"{output}  ({output.stat().st_size} bytes de fonte, {em.line} linhas)")
    print(f"{len(regions)} regioes, {total} bytes de ROM:")
    for kind, size in sorted(by_region.items(), key=lambda kv: -kv[1]):
        print(f"  {kind:8} {size:>8} bytes  ({size / total:.1%})")
    if stats.get("insn"):
        print(f"\nDentro das regioes 'code': {stats['insn']} bytes como instrucoes, "
              f"{stats.get('fallback', 0)} como dc.w de recuo, "
              f"{stats.get('raw', 0)} sem instrucao no trace")
        emitted, defined = stats.get("labels", 0), len(labels)
        print(f"Labels: {defined} definidos, {emitted} emitidos, "
              f"{stats.get('resolved', 0)} de {stats.get('targets', 0)} "
              f"destinos trocados por nome")
        if emitted != defined:
            print(f"  AVISO: {defined - emitted} labels nao foram emitidos — "
                  f"caem fora de uma regiao 'code'")
        print(f"Simbolos de RAM: {len(ram_names)} definidos, "
              f"{stats.get('ram_refs', 0)} referencias trocadas por nome")
        if skipped_labels:
            print(f"  {skipped_labels} descartados por nao cairem em inicio de instrucao")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
