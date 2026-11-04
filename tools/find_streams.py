"""Cataloga os fluxos comprimidos da ROM.

Os dois descompressores recebem o endereco do fluxo num registrador que o
chamador carrega com um lea absoluto poucas instrucoes antes: o endereco
do fluxo em a5 e o do destino em a6 (por exemplo, fluxo em $047C42 e
destino em $FFFF4000), e entao chama LzDecompressB.

Entao da' para achar cada fluxo seguindo os call sites e olhando para tras
ate' o ``lea`` que carrega o registrador certo — mesma tecnica usada em
trace_code.py para reconhecer tabelas de salto.

Cada fluxo encontrado e' descomprimido de verdade. Se a descompressao falhar
ou a saida nao for multipla de 32 bytes (tamanho do tile 4bpp), a linha sai
marcada: e' sinal de que o ponteiro nao foi o certo, nao de que o formato
esta' errado.

Uso:
    python tools/find_streams.py             # grava analysis/compressed-streams.csv
    python tools/find_streams.py --dry-run
    python tools/find_streams.py --dump build/streams
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lzss import LzssError, decompress, decompress_b  # noqa: E402
from romlib import Rom, hexa, load_data_regions, project_root  # noqa: E402
from trace_code import Trace, load_dispatchers, load_jump_tables  # noqa: E402

OUTPUT = project_root() / "analysis" / "compressed-streams.csv"
TILE_BYTES = 32
LOOKBACK = 10

# Filtros do modo --scan. O criterio "saida multipla de 32" sozinho aprova 1 em
# 32 fluxos por puro acaso, entao nao serve para varredura cega. Estes tres
# juntos derrubaram 549 falsos positivos de 603 candidatos.
MIN_RUN = 4                # ponteiros consecutivos para considerar tabela
RATIO_RANGE = (1.3, 6.0)   # razao de compressao plausivel
MAX_COLORS_PER_TILE = 9.0  # grafico 4bpp real fica entre 2,5 e 8; ruido, perto de 14
MIN_OUTPUT = 64

# Registradores do capstone: d0-d7 = 1-8, a0-a7 = 9-16.
REG_A0, REG_A5 = 9, 14

# (endereco da rotina, registrador com o ponteiro, variante, nome)
ENTRIES = [
    (0x001CDE, REG_A0, "a", "VdpUpload"),
    (0x001DAC, REG_A5, "b", "LzDecompressB"),
]


def source_of(trace: Trace, addrs: list[int], index: dict[int, int],
              site: int, reg: int) -> int | None:
    """Endereco absoluto carregado no registrador ``reg`` antes de ``site``."""
    for step in range(1, LOOKBACK + 1):
        pos = index[site] - step
        if pos < 0:
            return None
        insn = trace.insns[addrs[pos]]
        if insn.mnemonic == "lea" and insn.dest_reg == reg:
            for ref in insn.refs:
                if ref.kind == "abs":
                    return ref.target
            return None
        if insn.dest_reg == reg:
            return None      # registrador foi carregado de outro jeito
    return None


def colors_per_tile(data: bytes) -> float:
    """Media de cores distintas por tile de 32 bytes.

    E' o discriminador que separa grafico de lixo: um tile 4bpp de verdade usa
    poucas das 16 cores, enquanto dado aleatorio decodificado usa quase todas.
    """
    tiles = len(data) // TILE_BYTES
    total = 0
    for t in range(tiles):
        seen = set()
        for byte in data[t * TILE_BYTES:(t + 1) * TILE_BYTES]:
            seen.add(byte >> 4)
            seen.add(byte & 0xF)
        total += len(seen)
    return total / tiles if tiles else 99.0


def pointer_targets(rom: Rom) -> set[int]:
    """Alvos de runs de longs consecutivos que parecem endereco de ROM."""
    data, size = rom.data, rom.size
    targets: set[int] = set()
    offset = 0x200
    while offset + 4 <= size:
        value = int.from_bytes(data[offset:offset + 4], "big")
        if 0x200 <= value < size and value % 2 == 0:
            begin = offset
            while offset + 4 <= size:
                value = int.from_bytes(data[offset:offset + 4], "big")
                if not (0x200 <= value < size and value % 2 == 0):
                    break
                offset += 4
            if (offset - begin) // 4 >= MIN_RUN:
                for pos in range(begin, offset, 4):
                    targets.add(int.from_bytes(data[pos:pos + 4], "big"))
        else:
            offset += 4
    return targets


def scan(rom: Rom, taken: set[int]) -> list[tuple[int, str, int, int, float]]:
    """Fluxos alcancados por tabela de ponteiros, validados descomprimindo."""
    found = []
    for target in sorted(pointer_targets(rom)):
        if target in taken:
            continue
        for variant, routine in (("a", decompress), ("b", decompress_b)):
            try:
                out, consumed = routine(rom.data, target)
            except (LzssError, IndexError):
                continue
            if len(out) < MIN_OUTPUT or len(out) % TILE_BYTES or consumed <= 16:
                continue
            if any(a in taken for a in range(target, target + consumed)):
                continue
            ratio = len(out) / consumed
            if not RATIO_RANGE[0] <= ratio <= RATIO_RANGE[1]:
                continue
            colors = colors_per_tile(out)
            if colors > MAX_COLORS_PER_TILE:
                continue
            found.append((target, variant, consumed, len(out), colors))
            break
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Cataloga fluxos comprimidos")
    parser.add_argument("--rom")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--dump", help="grava cada fluxo descomprimido neste diretorio")
    parser.add_argument("--scan", action="store_true",
                        help="tambem varre alvos de tabelas de ponteiros")
    args = parser.parse_args(argv)

    rom = Rom.load(args.rom)
    trace = Trace(rom, load_jump_tables(), load_dispatchers())
    trace.run()
    addrs = sorted(trace.insns)
    index = {a: i for i, a in enumerate(addrs)}

    rows = []
    seen: dict[int, str] = {}
    for routine, reg, variant, name in ENTRIES:
        sites = sorted(trace.callers.get(routine, ()))
        for site in sites:
            if site not in index:
                continue
            source = source_of(trace, addrs, index, site, reg)
            if source is None or not rom.contains(source):
                rows.append([hexa(site), name, "", variant, "", "", "",
                             "ponteiro nao resolvido"])
                continue
            if source in seen:
                rows.append([hexa(site), name, hexa(source), variant, "", "", "",
                             f"mesmo fluxo de {seen[source]}"])
                continue
            try:
                data, consumed = (decompress if variant == "a" else decompress_b)(
                    rom.data, source)
            except (LzssError, IndexError) as exc:
                rows.append([hexa(site), name, hexa(source), variant, "", "", "",
                             f"falhou: {exc}"])
                continue
            seen[source] = hexa(site)
            tiles = len(data) / TILE_BYTES
            note = "" if tiles == int(tiles) else "saida nao e' multipla de 32 bytes"
            rows.append([hexa(site), name, hexa(source), variant, consumed,
                         len(data), int(tiles) if not note else "", note])
            if args.dump:
                out_dir = Path(args.dump)
                out_dir.mkdir(parents=True, exist_ok=True)
                (out_dir / f"{source:06X}.bin").write_bytes(data)

    if args.scan:
        taken: set[int] = set()
        for region in load_data_regions():
            taken.update(range(region["start"], region["end"] + 1))
        for addr, insn in trace.insns.items():
            taken.update(range(addr, insn.end))
        for row in rows:
            if row[2] and row[4] != "":
                begin = int(row[2].lstrip("$"), 16)
                taken.update(range(begin, begin + int(row[4])))
        for target, variant, consumed, size, colors in scan(rom, taken):
            rows.append(["(tabela)", "scan", hexa(target), variant, consumed, size,
                         size // TILE_BYTES, f"{colors:.1f} cores/tile"])
            taken.update(range(target, target + consumed))
            if args.dump:
                out_dir = Path(args.dump)
                out_dir.mkdir(parents=True, exist_ok=True)
                (out_dir / f"{target:06X}.bin").write_bytes(
                    (decompress if variant == "a" else decompress_b)(rom.data, target)[0])

    ok = [r for r in rows if r[5] != "" and (not r[7] or "cores/tile" in r[7])]
    total_in = sum(int(r[4]) for r in ok)
    total_out = sum(int(r[5]) for r in ok)
    print(f"{len(rows)} call sites, {len(ok)} fluxos descomprimidos com sucesso")
    print(f"comprimido: {total_in} bytes  ->  descomprimido: {total_out} bytes"
          + (f"  (razao {total_out / total_in:.2f}x)" if total_in else ""))
    print(f"tiles: {total_out // TILE_BYTES}")
    problemas = [r for r in rows if r[7]]
    if problemas:
        print(f"\n{len(problemas)} linha(s) com pendencia:")
        for row in problemas[:8]:
            print(f"  {row[0]}  {row[1]:14} {row[7]}")

    if not args.dry_run:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        with OUTPUT.open("w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(["site", "routine", "source", "variant",
                             "compressed", "decompressed", "tiles", "note"])
            writer.writerows(rows)
        print(f"\nGravado em {OUTPUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
