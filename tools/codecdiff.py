"""Portao dos descompressores: tools/codecs.py contra o jogo rodando.

O quinto portao de referencia independente do projeto, na mesma forma dos
outros quatro:

    rebuild.sh    o assembly emitido, montado pelo vasm, contra a ROM original
    difftest.py   o C recompilado contra o Musashi, instrucao a instrucao
    ymdiff.sh     a sintese FM contra o Nuked-OPN2
    zexdoc        o Z80 contra CRCs de hardware
    codecdiff.py  os codecs em Python contra os codecs do jogo    <-- este

Por que precisa existir: **consumir os mesmos bytes nao prova produzir os mesmos
bytes**. E para o LzA nem consumir prova nada — medido, 99,5% de offsets
aleatorios da ROM "decodificam" sem erro, porque o codec nao tem checksum e o
token de fim $1F aparece por acaso. Sem gabarito, um descompressor errado
passaria despercebido e contaminaria toda a extracao de dados a jusante.

O gabarito vem do proprio jogo: rodando com TINYTOON_CODECS=1, o runtime anota
de onde cada codec leu e o que escreveu (excluindo o anel, que compartilha o
mesmo caminho de escrita). Ver a justificativa em runtime/memory.c.

Uso:
    tools/codecdiff.sh                    # captura e compara
    python tools/codecdiff.py             # so' compara, com build/codecs.bin
"""

from __future__ import annotations

import argparse
import collections
import importlib.util
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import Rom, project_root  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "codecs_tt", Path(__file__).resolve().parent / "codecs.py")
cod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cod)

NOME = {0x001DAE: "LzDecompressB", 0x001E7C: "LzDecompressA", 0x01AD02: "RleUnpack"}

# RleUnpack tem tres variantes que so' diferem por uma mascara aplicada a cada
# word escrita, escolhida pelo slot de destino (d0). O gabarito nao registra d0,
# entao a comparacao aceita qualquer uma das tres — e o relatorio mostra quantas
# corridas de cada tipo apareceram, que e' informacao util por si so'.
MASCARAS = (("sem mascara", 0xFFFF), ("$BBBB", 0xBBBB), ("$7777", 0x7777))


def mascarar(dados: bytes, m: int) -> bytes:
    if m == 0xFFFF:
        return dados
    fora = bytearray()
    for i in range(0, len(dados) - 1, 2):
        w = ((dados[i] << 8) | dados[i + 1]) & m
        fora += bytes([w >> 8, w & 0xFF])
    return bytes(fora)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--gabarito", default="build/codecs.bin")
    args = p.parse_args(argv)

    raiz = project_root()
    caminho = Path(args.gabarito)
    if not caminho.is_absolute():
        caminho = raiz / caminho
    if not caminho.exists():
        print(f"sem {caminho}. Gere com tools/codecdiff.sh")
        return 1

    d = Rom.load().data
    b = caminho.read_bytes()

    p_ = 0
    batem = collections.Counter()
    variantes = collections.Counter()
    divergem = []
    truncadas = 0

    while p_ + 16 <= len(b):
        leitor, origem, consumido, produzido = struct.unpack_from(">4I", b, p_)
        p_ += 16
        esperado = b[p_:p_ + produzido]
        p_ += produzido
        nome = NOME.get(leitor, f"${leitor:06X}")

        # Corrida que encostou no teto do buffer nao e' comparavel: o gabarito
        # esta' cortado, e o Python decodificaria o resto. Contada a parte em vez
        # de virar falso negativo.
        if produzido >= 65536:
            truncadas += 1
            continue

        try:
            if leitor == 0x001DAE:
                meu, meu_cons = cod.lz_b(d, origem)
            elif leitor == 0x001E7C:
                meu, meu_cons = cod.lz_a(d, origem)
            else:
                meu, meu_cons = cod.rle(d, origem, produzido)
        except cod.Falha as e:
            divergem.append((nome, origem, f"nao decodificou: {e}"))
            continue

        if meu_cons != consumido:
            divergem.append((nome, origem,
                             f"consumo {meu_cons} != {consumido} do jogo"))
            continue

        for rotulo, m in MASCARAS:
            if mascarar(meu, m) == esperado:
                batem[nome] += 1
                variantes[(nome, rotulo)] += 1
                break
        else:
            i = next((k for k in range(min(len(meu), len(esperado)))
                      if meu[k] != esperado[k]), min(len(meu), len(esperado)))
            divergem.append((nome, origem, f"saida difere no byte {i}"))

    total = sum(batem.values()) + len(divergem)
    print(f"{total} corridas comparaveis "
          f"({truncadas} truncadas pelo buffer, ignoradas)\n")
    for nome in sorted(set(NOME.values())):
        n = batem[nome]
        d_ = sum(1 for x in divergem if x[0] == nome)
        print(f"  {nome:<16} {n:>4} batem, {d_:>3} divergem")
    print("\n  variantes de mascara vistas:")
    for (nome, rot), n in sorted(variantes.items()):
        print(f"    {nome:<16} {rot:<12} {n}")

    if divergem:
        print(f"\n{len(divergem)} DIVERGENCIAS:")
        for nome, origem, motivo in divergem[:10]:
            print(f"  {nome} em ${origem:06X}: {motivo}")
        return 1

    print("\nTODAS BATEM byte a byte contra o jogo.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
