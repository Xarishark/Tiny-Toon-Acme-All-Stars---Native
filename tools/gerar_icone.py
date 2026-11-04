"""Gera o icone do executavel, do zero.

O icone NAO pode sair da arte do jogo. Um icone recortado da ROM seria dado
protegido embutido no distribuivel — exatamente o que este projeto nao faz. A
disassembly, as ferramentas e o codigo sao os artefatos distribuiveis; a ROM e'
entrada local do usuario, e nada derivado dela entra no .exe.

Entao o desenho e' original e feito por aritmetica: um cartucho de Mega Drive
estilizado, que e' o que o programa e' — o jogo do cartucho rodando nativo.

Sai um .ico com seis tamanhos, do 16 ao 256. O Windows escolhe conforme o
contexto, e um .ico com um tamanho so' fica borrado em todos os outros.

Uso:
    python tools/gerar_icone.py            # build/icone.ico
    python tools/gerar_icone.py --png      # tambem grava um PNG para conferir
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import project_root  # noqa: E402

TAMANHOS = (16, 32, 48, 64, 128, 256)

FUNDO      = (24, 26, 38, 255)      # azul quase preto
CORPO      = (206, 210, 220, 255)   # plastico cinza
CORPO_ESC  = (150, 156, 170, 255)   # sombra do corpo
ROTULO     = (232, 78, 60, 255)     # vermelho do rotulo
ROTULO_ESC = (176, 50, 38, 255)
CONTATO    = (198, 156, 62, 255)    # dourado dos contatos
VAZIO      = (0, 0, 0, 0)


def dentro_arredondado(x, y, x0, y0, x1, y1, r):
    if not (x0 <= x < x1 and y0 <= y < y1):
        return False
    for cx, cy in ((x0 + r, y0 + r), (x1 - 1 - r, y0 + r),
                   (x0 + r, y1 - 1 - r), (x1 - 1 - r, y1 - 1 - r)):
        dx, dy = x - cx, y - cy
        fora_x = (x < x0 + r and cx == x0 + r) or (x > x1 - 1 - r and cx == x1 - 1 - r)
        fora_y = (y < y0 + r and cy == y0 + r) or (y > y1 - 1 - r and cy == y1 - 1 - r)
        if fora_x and fora_y and dx * dx + dy * dy > r * r:
            return False
    return True


def desenhar(n: int) -> bytes:
    """RGBA de n x n. Tudo em fracao do lado, para escalar sem caso especial."""
    px = [VAZIO] * (n * n)

    def por(x, y, cor):
        if 0 <= x < n and 0 <= y < n:
            px[y * n + x] = cor

    f = n / 256.0
    raio_fundo = max(2, int(48 * f))
    # corpo do cartucho
    cx0, cy0, cx1, cy1 = int(56 * f), int(40 * f), int(200 * f), int(216 * f)
    raio_corpo = max(1, int(10 * f))
    # rotulo
    rx0, ry0, rx1, ry1 = int(72 * f), int(64 * f), int(184 * f), int(140 * f)
    # contatos
    ct0, ct1 = int(178 * f), int(210 * f)

    for y in range(n):
        for x in range(n):
            if not dentro_arredondado(x, y, 0, 0, n, n, raio_fundo):
                continue
            cor = FUNDO
            if dentro_arredondado(x, y, cx0, cy0, cx1, cy1, raio_corpo):
                # Sem chanfro: a primeira versao cortava o canto superior
                # direito e a 128px o resultado lia como pagina dobrada, nao
                # como cartucho. Silhueta simples resolve, e a 16px e' a unica
                # coisa que sobrevive de qualquer jeito.
                cor = CORPO if x < cx1 - max(1, int(8 * f)) else CORPO_ESC
                if rx0 <= x < rx1 and ry0 <= y < ry1:
                    cor = ROTULO if y < ry1 - max(1, int(6 * f)) else ROTULO_ESC
                elif ct0 <= y < ct1:
                    # dentes dourados do conector
                    passo = max(2, int(14 * f))
                    cor = CONTATO if ((x - cx0) % passo) < passo * 0.6 else CORPO_ESC
            por(x, y, cor)
    return b"".join(bytes(c) for c in px)


def png_rgba(largura: int, altura: int, rgba: bytes) -> bytes:
    linhas = b"".join(b"\x00" + rgba[y * largura * 4:(y + 1) * largura * 4]
                      for y in range(altura))

    def bloco(tipo: bytes, dados: bytes) -> bytes:
        return (struct.pack(">I", len(dados)) + tipo + dados
                + struct.pack(">I", zlib.crc32(tipo + dados) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + bloco(b"IHDR", struct.pack(">IIBBBBB", largura, altura, 8, 6, 0, 0, 0))
            + bloco(b"IDAT", zlib.compress(linhas, 9))
            + bloco(b"IEND", b""))


def ico(imagens: list[tuple[int, bytes]]) -> bytes:
    """ICO com cada tamanho guardado como PNG (suportado desde o Vista)."""
    cab = struct.pack("<HHH", 0, 1, len(imagens))
    entradas = b""
    dados = b""
    deslocamento = 6 + 16 * len(imagens)
    for n, png in imagens:
        entradas += struct.pack("<BBBBHHII",
                                0 if n >= 256 else n, 0 if n >= 256 else n,
                                0, 0, 1, 32, len(png), deslocamento + len(dados))
        dados += png
    return cab + entradas + dados


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--png", action="store_true")
    args = p.parse_args(argv)

    raiz = project_root()
    destino = raiz / "build"
    destino.mkdir(parents=True, exist_ok=True)

    imagens = []
    for n in TAMANHOS:
        rgba = desenhar(n)
        imagens.append((n, png_rgba(n, n, rgba)))
        if args.png:
            (destino / f"icone-{n}.png").write_bytes(imagens[-1][1])

    alvo = destino / "icone.ico"
    alvo.write_bytes(ico(imagens))
    print(f"gravado {alvo.relative_to(raiz)} "
          f"({len(TAMANHOS)} tamanhos, {alvo.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
