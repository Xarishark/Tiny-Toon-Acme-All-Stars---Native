"""Localiza e extrai paletas de cores do Mega Drive.

Formato de cor do VDP, uma word por cor::

    bit  15 14 13 12 | 11 10  9  8 | 7  6  5  4 | 3  2  1  0
          0  0  0  0 |  B  B  B  0 | G  G  G  0 | R  R  R  0

Ou seja, os bits da mascara ``$F111`` sao sempre zero. Isso da' um detector
exato, nao heuristico: uma sequencia de 16 words que passa na mascara **pode**
ser paleta; uma que nao passa **nao pode**.

Como zero passa trivialmente na mascara, o detector exige tambem um minimo de
cores distintas e nao-zero, senao qualquer bloco de zeros viraria paleta.

Uso:
    python tools/extract_palettes.py             # relatorio + analysis/palettes.csv
    python tools/extract_palettes.py --png       # + previews em build/palettes/
    python tools/extract_palettes.py --min-distinct 4
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import Rom, hexa, project_root  # noqa: E402

COLORS_PER_PALETTE = 16
PALETTE_BYTES = COLORS_PER_PALETTE * 2
COLOR_MASK = 0xF111          # bits que o VDP ignora e que o jogo mantem zerados

OUTPUT = project_root() / "analysis" / "palettes.csv"
PNG_DIR = project_root() / "build" / "palettes"


def is_color(word: int) -> bool:
    return (word & COLOR_MASK) == 0


def to_rgb(word: int) -> tuple[int, int, int]:
    """Converte a word do VDP para RGB de 8 bits.

    Cada componente tem 3 bits (0-7); multiplicar por 36 espalha 0-7 em 0-252,
    que e' a aproximacao usual para visualizar paleta de Mega Drive.
    """
    red = (word >> 1) & 0x7
    green = (word >> 5) & 0x7
    blue = (word >> 9) & 0x7
    return red * 36, green * 36, blue * 36


def read_palette(rom: Rom, offset: int) -> list[int] | None:
    if offset + PALETTE_BYTES > rom.size:
        return None
    words = [rom.word(offset + 2 * i) for i in range(COLORS_PER_PALETTE)]
    return words if all(is_color(w) for w in words) else None


def find_color_runs(rom: Rom, start: int = 0x200) -> list[tuple[int, int]]:
    """Sequencias maximais de words que passam na mascara de cor.

    Achar os runs primeiro, e so' depois fatiar em paletas, evita o erro de
    varrer de 2 em 2 bytes: dentro de um banco de paletas quase toda janela
    deslocada tambem passa na mascara, e o scanner trava numa grade errada.
    """
    runs: list[tuple[int, int]] = []
    offset, size = start, rom.size
    while offset + 2 <= size:
        if not is_color(rom.word(offset)):
            offset += 2
            continue
        begin = offset
        while offset + 2 <= size and is_color(rom.word(offset)):
            offset += 2
        runs.append((begin, offset))
    return runs


def find_palettes(rom: Rom, min_distinct: int, min_nonzero: int,
                  start: int = 0x200) -> list[dict]:
    """Fatia cada run de cores em paletas de 16, a partir do inicio do run."""
    found: list[dict] = []
    for begin, end in find_color_runs(rom, start):
        if end - begin < PALETTE_BYTES:
            continue
        # Riqueza avaliada no run inteiro: um banco pode conter uma paleta
        # quase toda escura, que sozinha nao passaria no filtro.
        run_words = [rom.word(o) for o in range(begin, end, 2)]
        if len(set(run_words)) < min_distinct or sum(1 for w in run_words if w) < min_nonzero:
            continue
        for offset in range(begin, end - PALETTE_BYTES + 1, PALETTE_BYTES):
            words = [rom.word(offset + 2 * i) for i in range(COLORS_PER_PALETTE)]
            found.append({"offset": offset, "words": words,
                          "distinct": len(set(words)),
                          "nonzero": sum(1 for w in words if w)})
    return found


def group_runs(palettes: list[dict]) -> list[list[dict]]:
    """Agrupa paletas consecutivas (uma logo apos a outra) num mesmo banco."""
    runs: list[list[dict]] = []
    for pal in palettes:
        if runs and pal["offset"] == runs[-1][-1]["offset"] + PALETTE_BYTES:
            runs[-1].append(pal)
        else:
            runs.append([pal])
    return runs


def write_csv(palettes: list[dict]) -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(["address", "distinct", "nonzero", "colors_vdp", "colors_rgb"])
        for pal in palettes:
            writer.writerow([
                hexa(pal["offset"]), pal["distinct"], pal["nonzero"],
                " ".join(f"{w:04X}" for w in pal["words"]),
                " ".join("%02X%02X%02X" % to_rgb(w) for w in pal["words"]),
            ])


def write_pngs(palettes: list[dict], cell: int = 16) -> int:
    """Uma folha de contato com todas as paletas, uma por linha.

    Uma imagem so' e' mais util que 49 arquivos separados: da' para comparar os
    bancos de olho e perceber rampas e variacoes de um mesmo tema.
    """
    from PIL import Image

    PNG_DIR.mkdir(parents=True, exist_ok=True)
    sheet = Image.new("RGB", (COLORS_PER_PALETTE, len(palettes)))
    sheet.putdata([to_rgb(w) for pal in palettes for w in pal["words"]])
    sheet = sheet.resize((COLORS_PER_PALETTE * cell, len(palettes) * cell),
                         Image.NEAREST)
    path = PNG_DIR / "all.png"
    sheet.save(path)
    print(f"folha de contato: {path} ({len(palettes)} linhas de 16 cores)")
    return len(palettes)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Extrai paletas do Mega Drive")
    parser.add_argument("--rom")
    parser.add_argument("--min-distinct", type=int, default=8,
                        help="minimo de cores distintas (padrao 8)")
    parser.add_argument("--min-nonzero", type=int, default=12,
                        help="minimo de cores nao-zero (padrao 12)")
    parser.add_argument("--png", action="store_true", help="grava previews em build/palettes/")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    rom = Rom.load(args.rom)
    palettes = find_palettes(rom, args.min_distinct, args.min_nonzero)
    runs = group_runs(palettes)

    print(f"{len(palettes)} paletas em {len(runs)} bloco(s)")
    print(f"{len(palettes) * PALETTE_BYTES} bytes ("
          f"{100 * len(palettes) * PALETTE_BYTES / rom.size:.2f}% da ROM)\n")
    for run in sorted(runs, key=len, reverse=True):
        start = run[0]["offset"]
        end = run[-1]["offset"] + PALETTE_BYTES - 1
        print(f"  {hexa(start)}-{hexa(end)}  {len(run):3d} paleta(s), "
              f"{end - start + 1} bytes")

    if not args.dry_run:
        write_csv(palettes)
        print(f"\nGravado em {OUTPUT}")
        if args.png:
            print(f"{write_pngs(palettes)} previews em {PNG_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
