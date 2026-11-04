"""Quanto da ROM esta' classificado, e o que sobra.

Este e' o medidor de progresso rumo aos 100%. Percorre o megabyte inteiro e
atribui cada byte a uma categoria, com a evidencia que a sustenta:

    vetores/header   estrutura fixa do cartucho
    codigo           tracado por caminho de execucao ate' ele
    dado declarado   registrado em analysis/data-regions.csv, com evidencia
    desconhecido     ninguem sabe o que e'

O numero que importa e' o ultimo. Ele so' cai quando alguem investiga uma regiao
e registra o que CONFIRMOU — nao quando alguem chuta.

O relatorio lista os maiores blocos desconhecidos em ordem de tamanho, porque
essa e' a fila de trabalho: atacar o maior primeiro rende mais por hora.

Uso:
    python tools/rom_coverage.py
    python tools/rom_coverage.py --blocos 40      # mais candidatos
    python tools/rom_coverage.py --min 256        # so' blocos grandes
"""

from __future__ import annotations

import argparse
import collections
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import Rom, hexa, load_data_regions, project_root  # noqa: E402
from trace_code import Trace, load_dispatchers, load_jump_tables  # noqa: E402


def montar_mapa(rom: Rom, trace: Trace) -> list[str]:
    """Um rotulo por byte da ROM."""
    mapa = ["desconhecido"] * len(rom.data)

    for i in range(0x100):
        mapa[i] = "vetores"
    for i in range(0x100, 0x200):
        mapa[i] = "header"

    for addr, insn in trace.insns.items():
        for i in range(addr, min(addr + insn.size, len(mapa))):
            mapa[i] = "codigo"

    for r in load_data_regions():
        ini, fim = r["start"], r["end"]
        for i in range(ini, min(fim + 1, len(mapa))):
            if mapa[i] == "desconhecido":
                mapa[i] = r["kind"]

    # Por ultimo, o que o jogo RODANDO leu.
    #
    # Vem depois das outras fontes de proposito: onde as duas concordam, a
    # analise estatica ja' tinha nome melhor, e onde discordam quem manda e' o
    # rotulo curado a mao. O mapa de leitura preenche o que sobrou — que e'
    # justamente onde a analise estatica nao chega, porque o jogo alcanca esses
    # dados por tabela de ponteiro indexada em tempo de execucao.
    for r in load_read_regions():
        for i in range(r["start"], min(r["end"] + 1, len(mapa))):
            if mapa[i] == "desconhecido":
                mapa[i] = r["kind"]

    return mapa


def load_read_regions() -> list[dict]:
    """analysis/rom-leituras.csv, gerado por tools/rom_leituras.py."""
    caminho = project_root() / "analysis" / "rom-leituras.csv"
    if not caminho.exists():
        return []
    with caminho.open(encoding="utf-8", newline="") as fh:
        return [{"start": int(l["start"].lstrip("$"), 16),
                 "end": int(l["end"].lstrip("$"), 16),
                 "kind": l["kind"],
                 "confianca": l.get("confianca", "execucao")}
                for l in csv.DictReader(fh)]


def blocos_desconhecidos(mapa: list[str]) -> list[tuple[int, int]]:
    blocos = []
    i = 0
    n = len(mapa)
    while i < n:
        if mapa[i] != "desconhecido":
            i += 1
            continue
        j = i
        while j < n and mapa[j] == "desconhecido":
            j += 1
        blocos.append((i, j - i))
        i = j
    return blocos


def pista(rom: Rom, ini: int, tam: int) -> str:
    """Um palpite de PARTIDA sobre o bloco — nunca um fato.

    Serve para ordenar o trabalho, nao para classificar. Quem classifica e' quem
    investigar e registrar em data-regions.csv com evidencia.
    """
    d = rom.data[ini:ini + min(tam, 4096)]
    if not d:
        return ""
    zeros = d.count(0) / len(d)
    if zeros > 0.9:
        return "quase tudo zero (preenchimento?)"

    # Densidade de rts/nop/link/unlk alinhados: assinatura de codigo.
    marcas = 0
    for k in range(0, len(d) - 1, 2):
        w = (d[k] << 8) | d[k + 1]
        if w in (0x4E75, 0x4E71, 0x4E73, 0x4E56, 0x4E5E):
            marcas += 1
    if marcas and len(d) // marcas < 400:
        return f"denso em rts/nop (1 a cada {len(d)//marcas} bytes) — pode ser codigo"

    # Nibbles: arte 4bpp usa os 16 valores de forma parecida.
    nib = collections.Counter()
    for b in d:
        nib[b >> 4] += 1
        nib[b & 15] += 1
    usados = sum(1 for v in nib.values() if v > len(d) // 64)
    if usados >= 14:
        return "nibbles bem distribuidos — arte 4bpp descomprimida?"

    # Palavras que parecem cor do VDP: 0000 BBB0 GGG0 RRR0.
    cores = 0
    for k in range(0, min(len(d), 512) - 1, 2):
        w = (d[k] << 8) | d[k + 1]
        if not (w & 0xF111):
            cores += 1
    if cores > min(len(d), 512) // 2 // 2:
        return "muitas palavras no formato de cor — paleta?"

    return f"{usados} de 16 nibbles em uso, {zeros*100:.0f}% zeros"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--rom", help="caminho da ROM (padrao: autodetecta)")
    p.add_argument("--blocos", type=int, default=20)
    p.add_argument("--min", type=int, default=64)
    args = p.parse_args(argv)

    rom = Rom.load(args.rom)
    trace = Trace(rom, load_jump_tables(), load_dispatchers())
    trace.run()
    mapa = montar_mapa(rom, trace)

    contagem = collections.Counter(mapa)
    total = len(mapa)
    print(f"ROM: {total} bytes\n")
    print(f"{'categoria':<22} {'bytes':>10} {'%':>7}")
    for kind, n in contagem.most_common():
        print(f"{kind:<22} {n:>10} {100*n/total:>6.2f}%")

    desconhecido = contagem["desconhecido"]
    # Inferido nao entra na mesma conta que evidencia. A regra de ouro do
    # projeto vale tambem para a metrica: um numero que soma o que se sabe com o
    # que se supoe para de medir progresso e passa a medir otimismo. Os rotulos
    # terminados em "?" sao os inferidos, e eles saem sempre em linha separada.
    inferido = sum(n for k, n in contagem.items() if k.endswith("?"))
    firme = total - desconhecido - inferido
    print(f"\nCLASSIFICADO (evidencia): {100*firme/total:>6.2f}%  {firme} bytes")
    print(f"  + inferido:             {100*inferido/total:>6.2f}%  {inferido} bytes")
    print(f"  FALTA:                  {100*desconhecido/total:>6.2f}%  "
          f"{desconhecido} bytes")

    blocos = [b for b in blocos_desconhecidos(mapa) if b[1] >= args.min]
    blocos.sort(key=lambda b: -b[1])
    print(f"\n{len(blocos)} blocos desconhecidos de {args.min}+ bytes. "
          f"Os {min(args.blocos, len(blocos))} maiores:\n")
    for ini, tam in blocos[:args.blocos]:
        print(f"  {hexa(ini)}-{hexa(ini+tam-1)}  {tam:>7} bytes  {pista(rom, ini, tam)}")

    grandes = sum(t for _, t in blocos)
    print(f"\nEsses {len(blocos)} blocos somam {grandes} bytes "
          f"({100*grandes/desconhecido:.1f}% do que falta).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
