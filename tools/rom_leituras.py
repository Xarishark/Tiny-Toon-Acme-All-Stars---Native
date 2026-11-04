"""Transforma o mapa de leitura do port rodando em regioes classificadas.

O jogo alcanca seus dados por tabelas de ponteiros indexadas em tempo de
execucao. Seguir isso estaticamente exigiria analise de fluxo de dados e ainda
assim daria palpite. O port resolve de graca: ele EXECUTA o jogo, e o runtime
anota, byte a byte da ROM:

    romlido.bin     como o byte foi lido   (bit 0 CPU, bit 1 DMA, bit 2 Z80)
    romleitor.bin   quem o leu             (endereco da funcao, ou PC do Z80)

Isto e' evidencia de execucao, nao inferencia: nao "acho que e' dado", e sim
"a funcao X leu estes bytes neste caminho". E como as duas gravacoes ACUMULAM
entre sessoes, jogar o jogo vira a ferramenta de extracao — cada tela nova
visitada classifica mais ROM.

Este script recorta o mapa em regioes: uma regiao e' um trecho contiguo lido
pelo MESMO leitor. Os limites nao sao chutados, sao onde o leitor muda.

Saida (regeneravel, nunca editar a mao): analysis/rom-leituras.csv

Uso:
    python tools/rom_leituras.py
    python tools/rom_leituras.py --csv          # regrava analysis/rom-leituras.csv
    python tools/rom_leituras.py --min 32
    python tools/rom_leituras.py --leitor 01AD02  # so' as regioes de uma funcao
"""

from __future__ import annotations

import argparse
import collections
import csv
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import Rom, hexa, load_symbol_names, project_root  # noqa: E402
from trace_code import Trace, load_dispatchers, load_jump_tables  # noqa: E402
from rom_coverage import montar_mapa  # noqa: E402

VIA_CPU, VIA_DMA, VIA_Z80 = 1, 2, 4
Z80_FLAG = 0x80000000

# Leitores cujo papel ja' esta' estabelecido pela analise estatica. Um leitor
# conhecido nomeia o dado: o que LzDecompressA le' e' fluxo comprimido, ponto.
#
# Os enderecos sao os CORPOS das rotinas, nao os pontos de entrada: LzDecompressB
# ($001DAC) tem uma instrucao so' e cai em $001DAE, e LzDecompressA ($001E3A) cai
# em $001E7C. Quem executa quando o dado e' lido e' o corpo.
PAPEL = {
    0x001DAE: ("lz_stream", "fluxo comprimido lido por LzDecompressB"),
    0x001E7C: ("lz_stream", "fluxo comprimido lido por LzDecompressA"),
}

# Quanto de bytes NAO lidos tolerar dentro de uma regiao antes de parti-la.
#
# Existe porque um leitor raramente toca todo byte de uma tabela: ele indexa a
# entrada que precisa. Buracos pequenos cercados pelo mesmo leitor sao a mesma
# estrutura, e parti-los produziria centenas de fragmentos de 4 bytes que nao
# ajudam ninguem. Buracos grandes, nao — ali pode comecar outra coisa.
FOLGA = 64


def nome_leitor(v: int, nomes: dict[int, str]) -> str:
    if v & Z80_FLAG:
        return f"z80:${v & 0xFFFF:04X}"
    return nomes.get(v) or f"sub_{v:06X}"


def segmentar(lido: bytes, leitor, folga: int = FOLGA):
    """Regioes contiguas com o mesmo leitor, tolerando buracos ate' `folga`."""
    regioes = []
    n = len(lido)
    i = 0
    while i < n:
        if not lido[i]:
            i += 1
            continue
        quem = leitor[i]
        vias = 0
        j = i
        fim = i           # ultimo byte efetivamente lido
        while j < n:
            if lido[j] and leitor[j] == quem:
                vias |= lido[j]
                fim = j
                j += 1
            elif not lido[j] and j - fim <= folga:
                j += 1    # buraco tolerado; so' conta se algo lido vier depois
            else:
                break
        regioes.append((i, fim, quem, vias))
        i = fim + 1
    return regioes


def rotular(quem: int, vias: int, nomes: dict[int, str]) -> tuple[str, str]:
    if quem in PAPEL:
        return PAPEL[quem]
    if vias & VIA_Z80:
        return ("audio", f"buscado pela janela de banco do Z80 em {nome_leitor(quem, nomes)}")
    if vias & VIA_DMA:
        return ("gfx_dma", f"origem de DMA para a VRAM montado por {nome_leitor(quem, nomes)}")
    return ("dado", f"lido pela CPU em {nome_leitor(quem, nomes)}")


def preenchimento(dados: bytes, lido: bytes, minimo: int = 8):
    """Corridas de um byte so' que ninguem le'. Isto e' fato, nao inferencia.

    Um trecho constante que o jogo nunca tocou em nenhuma sessao nao guarda
    informacao: e' o espaco entre dois arquivos, ou o resto do ultimo banco.
    Nao vale a mesma coisa que "lido pela funcao X", mas vale muito mais que
    "desconhecido" — e sao 29 KB desta ROM.
    """
    fora = []
    n = len(dados)
    i = 0
    while i < n:
        if lido[i]:
            i += 1
            continue
        j = i
        while j < n and not lido[j] and dados[j] == dados[i]:
            j += 1
        if j - i >= minimo:
            fora.append((i, j - 1, dados[i]))
        i = max(j, i + 1)
    return fora


def interiores(lido: bytes, leitor, minimo: int = 4):
    """Buracos cercados pelo MESMO leitor dos dois lados.

    Um arquivo comprimido guarda seus blocos encostados um no outro. Se o bloco
    antes e o bloco depois do buraco foram lidos pela mesma rotina, o buraco e'
    quase certamente mais um bloco daquele arquivo que ninguem selecionou nesta
    corrida.

    QUASE CERTAMENTE, e nao certamente — por isso sai marcado como inferido e
    entra numa conta separada. A regra de ouro do projeto e' nao adivinhar; o
    palpite util e' o que se declara palpite.
    """
    fora = []
    n = len(lido)
    i = 0
    while i < n:
        if lido[i]:
            i += 1
            continue
        j = i
        while j < n and not lido[j]:
            j += 1
        if i > 0 and j < n and leitor[i - 1] == leitor[j] and j - i >= minimo:
            fora.append((i, j - 1, leitor[j]))
        i = max(j, i + 1)
    return fora


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--lido", default="build/romlido.bin")
    p.add_argument("--leitor", dest="arq_leitor", default="build/romleitor.bin")
    p.add_argument("--min", type=int, default=8, help="tamanho minimo da regiao")
    p.add_argument("--folga", type=int, default=FOLGA)
    p.add_argument("--so", help="so' regioes deste leitor (hex, ex: 01AD02)")
    p.add_argument("--rom", help="caminho da ROM (padrao: autodetecta)")
    p.add_argument("--blocos", type=int, default=25)
    p.add_argument("--csv", action="store_true", help="regrava analysis/rom-leituras.csv")
    args = p.parse_args(argv)

    raiz = project_root()
    cam_lido = Path(args.lido)
    cam_leitor = Path(args.arq_leitor)
    if not cam_lido.is_absolute():
        cam_lido, cam_leitor = raiz / cam_lido, raiz / cam_leitor
    if not cam_lido.exists():
        print(f"sem {cam_lido}. Gere rodando o port com TINYTOON_ROMLIDO=romlido.bin")
        return 1

    lido = cam_lido.read_bytes()
    bruto = cam_leitor.read_bytes()
    leitor = struct.unpack(f">{len(bruto)//4}I", bruto)

    rom = Rom.load(args.rom)
    trace = Trace(rom, load_jump_tables(), load_dispatchers())
    trace.run()
    estatico = montar_mapa(rom, trace)
    nomes = load_symbol_names()

    regioes = [r for r in segmentar(lido, leitor, args.folga)
               if r[1] - r[0] + 1 >= args.min]
    if args.so:
        alvo = int(args.so, 16)
        regioes = [r for r in regioes if (r[2] & ~Z80_FLAG) == alvo or r[2] == alvo]

    # Quanto disto e' novidade: byte lido que a analise estatica nao classificava.
    novos = sum(1 for i, b in enumerate(lido) if b and estatico[i] == "desconhecido")
    total = len(estatico)
    antes = sum(1 for k in estatico if k != "desconhecido")

    print(f"ROM {total} bytes | lidos {sum(1 for b in lido if b)} "
          f"({100*sum(1 for b in lido if b)/total:.2f}%)")
    print(f"classificado antes: {100*antes/total:.2f}%  "
          f"depois: {100*(antes+novos)/total:.2f}%  (+{novos} bytes)")

    por_via = collections.Counter()
    for b in lido:
        if b:
            por_via[b] += 1
    rot = {VIA_CPU: "CPU", VIA_DMA: "DMA do VDP", VIA_Z80: "janela do Z80"}
    print("\nlido por via:")
    for v, k in sorted(por_via.items(), key=lambda t: -t[1]):
        print(f"  {rot.get(v, f'via {v}'):<16} {k:>8} bytes")

    print(f"\n{len(regioes)} regioes de {args.min}+ bytes. As {args.blocos} maiores:\n")
    for ini, fim, quem, vias in sorted(regioes, key=lambda r: -(r[1]-r[0]))[:args.blocos]:
        kind, ev = rotular(quem, vias, nomes)
        novo = sum(1 for i in range(ini, fim+1) if estatico[i] == "desconhecido")
        print(f"  {hexa(ini)}-{hexa(fim)} {fim-ini+1:>6}b  {kind:<10} "
              f"{nome_leitor(quem, nomes):<22} novo:{novo}")

    if args.csv:
        destino = raiz / "analysis" / "rom-leituras.csv"
        with destino.open("w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["start", "end", "kind", "confianca", "reader",
                        "vias", "evidence"])
            linhas = [(ini, fim, *rotular(quem, vias, nomes), "execucao",
                       nome_leitor(quem, nomes), vias)
                      for ini, fim, quem, vias in regioes]
            linhas += [(ini, fim, "preenchimento", f"corrida de ${b:02X} nunca lida",
                        "constante", "-", 0)
                       for ini, fim, b in preenchimento(rom.data, lido)]
            linhas += [(ini, fim, rotular(quem, 0, nomes)[0] + "?",
                        f"buraco cercado dos dois lados por {nome_leitor(quem, nomes)}",
                        "inferido", nome_leitor(quem, nomes), 0)
                       for ini, fim, quem in interiores(lido, leitor)]
            for ini, fim, kind, ev, conf, quem, vias in sorted(linhas):
                w.writerow([hexa(ini), hexa(fim), kind, conf, quem, vias, ev])
            regioes = linhas
        print(f"\ngravado {destino.relative_to(raiz)} ({len(regioes)} regioes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
