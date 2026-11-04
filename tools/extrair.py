"""Extrai os dados comprimidos da ROM para arquivos.

Esta e' a trilha que o diagrama do projeto chama de "extracao -> dados do jogo"
e que ate' agora so' existia dentro do port rodando. Com os tres codecs em
Python (tools/codecs.py, verificados contra o jogo por tools/codecdiff.py), da'
para tirar a arte e os mapas da ROM sem emular nada.

So' extrai regiao CONFIRMADA: um bloco entra se os fluxos comprimidos LADRILHAM
exatamente o trecho que o jogo leu — decodificar do inicio e cair, fluxo apos
fluxo, no ultimo byte da regiao, nem um a mais nem um a menos. Isso importa
porque o LzA aceita 99,5% dos offsets aleatorios da ROM: "decodificou" nao e'
evidencia de nada, "ladrilhou o que o jogo leu" e'.

A folha de tiles em PNG existe para conferir com o olho. Os tiles do Mega Drive
sao 8x8 de 4 bits por pixel, dois pixels por byte, e sem a paleta certa a cor
nao significa nada — entao sai em rampa de cinza pelo indice. Serve para
responder "isto e' arte ou e' tabela?", que e' a pergunta que a extracao precisa
responder primeiro.

Uso:
    python tools/extrair.py                  # relatorio, nao grava nada
    python tools/extrair.py --gravar         # build/extraido/*.bin e *.png
    python tools/extrair.py --gravar --png 0 # so' os .bin
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import Rom, hexa, project_root  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "codecs_tt", Path(__file__).resolve().parent / "codecs.py")
cod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cod)

CODECS = {"sub_001DAE": ("lzb", cod.lz_b), "LzDecompressB": ("lzb", cod.lz_b),
          "sub_001E7C": ("lza", cod.lz_a), "LzDecompressA": ("lza", cod.lz_a)}


def ladrilhar(fn, dados: bytes, ini: int, fim: int):
    """Decodifica fluxos encostados de ini ate' fim.

    Devolve (lista de saidas, posicao final). Quem chama compara a posicao final
    com fim+1 — so' isso conta como confirmacao.
    """
    p, saidas = ini, []
    while p <= fim:
        saida, consumido = fn(dados, p)
        if consumido <= 0:
            raise cod.Falha("consumo zero")
        saidas.append(saida)
        p += consumido
        if len(saidas) > 4000:
            raise cod.Falha("fluxos demais para uma regiao so'")
    return saidas, p


def png_cinza(caminho: Path, largura: int, altura: int, pixels: bytes) -> None:
    """PNG de 8 bits em tons de cinza, sem dependencia externa."""
    linhas = b"".join(b"\x00" + pixels[y * largura:(y + 1) * largura]
                      for y in range(altura))

    def bloco(tipo: bytes, dados: bytes) -> bytes:
        return (struct.pack(">I", len(dados)) + tipo + dados
                + struct.pack(">I", zlib.crc32(tipo + dados) & 0xFFFFFFFF))

    cabecalho = struct.pack(">IIBBBBB", largura, altura, 8, 0, 0, 0, 0)
    caminho.write_bytes(b"\x89PNG\r\n\x1a\n"
                        + bloco(b"IHDR", cabecalho)
                        + bloco(b"IDAT", zlib.compress(linhas, 9))
                        + bloco(b"IEND", b""))


def folha_de_tiles(dados: bytes, por_linha: int = 16) -> tuple[int, int, bytes]:
    """Interpreta os bytes como tiles 4bpp de 8x8 e monta uma folha."""
    n_tiles = len(dados) // 32
    if n_tiles == 0:
        return 0, 0, b""
    linhas_de_tile = (n_tiles + por_linha - 1) // por_linha
    largura, altura = por_linha * 8, linhas_de_tile * 8
    buf = bytearray(largura * altura)
    for t in range(n_tiles):
        tx, ty = (t % por_linha) * 8, (t // por_linha) * 8
        for y in range(8):
            for xb in range(4):
                b = dados[t * 32 + y * 4 + xb]
                for meio in (0, 1):
                    idx = (b >> 4) if meio == 0 else (b & 15)
                    px, py = tx + xb * 2 + meio, ty + y
                    buf[py * largura + px] = idx * 17    # 0..15 -> 0..255
    return largura, altura, bytes(buf)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--gravar", action="store_true")
    p.add_argument("--png", type=int, default=1)
    p.add_argument("--destino", default="build/extraido")
    args = p.parse_args(argv)

    raiz = project_root()
    dados = Rom.load().data
    csv_leituras = raiz / "analysis" / "rom-leituras.csv"
    if not csv_leituras.exists():
        print("sem analysis/rom-leituras.csv — rode tools/rom_leituras.py --csv")
        return 1

    destino = raiz / args.destino
    if args.gravar:
        destino.mkdir(parents=True, exist_ok=True)

    confirmadas = falharam = 0
    total_comprimido = total_expandido = 0
    blocos = 0
    por_codec: dict[str, list[int]] = {"lza": [], "lzb": []}

    with csv_leituras.open(encoding="utf-8", newline="") as fh:
        regioes = list(csv.DictReader(fh))

    for r in regioes:
        if r.get("confianca") != "execucao":
            continue
        alvo = CODECS.get(r["reader"])
        if not alvo:
            continue
        rotulo, fn = alvo
        ini = int(r["start"].lstrip("$"), 16)
        fim = int(r["end"].lstrip("$"), 16)
        try:
            saidas, p_fim = ladrilhar(fn, dados, ini, fim)
        except cod.Falha:
            falharam += 1
            continue
        if p_fim != fim + 1:
            falharam += 1
            continue

        confirmadas += 1
        total_comprimido += fim - ini + 1
        for k, saida in enumerate(saidas):
            blocos += 1
            total_expandido += len(saida)
            por_codec[rotulo].append(len(saida))
            if args.gravar:
                base = destino / f"{rotulo}-{hexa(ini).lstrip('$')}-{k:03d}"
                base.with_suffix(".bin").write_bytes(saida)
                if args.png and len(saida) >= 32:
                    w, h, px = folha_de_tiles(saida)
                    if w:
                        png_cinza(base.with_suffix(".png"), w, h, px)

    print(f"regioes confirmadas (ladrilham exato): {confirmadas}, "
          f"nao confirmadas: {falharam}")
    print(f"blocos extraidos: {blocos}")
    print(f"comprimido na ROM: {total_comprimido} bytes")
    print(f"descomprimido:     {total_expandido} bytes "
          f"(razao {total_expandido / max(total_comprimido, 1):.2f}x)")
    for k, v in por_codec.items():
        if v:
            print(f"  {k}: {len(v)} blocos, "
                  f"menor {min(v)}, maior {max(v)}, medio {sum(v)//len(v)}")
    if args.gravar:
        print(f"\ngravado em {destino.relative_to(raiz)}")
    else:
        print("\n(nada gravado; use --gravar)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
