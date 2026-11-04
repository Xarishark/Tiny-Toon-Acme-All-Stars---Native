"""Descompressor do formato usado pelo jogo, transcrito de $001E3A.

E' uma variante de LZSS com buffer circular de 1024 bytes. O fluxo alterna um
byte de controle (8 bits, do menos significativo para o mais) e os tokens que
esses bits descrevem.

Bit de controle 0 -- **literal**: o byte lido vai para a saida e para o anel.

Bit de controle 1 -- o byte lido (aqui chamado ``b``) escolhe entre tres
formas, e essa e' a parte que foge do LZSS de manual:

- ``b == $1F``            fim do fluxo;
- ``b < $80``             copia de posicao **absoluta** no anel. Le mais um
                          byte do fluxo; ``offset = ((b << 3) & $300) | byte``
                          (10 bits) e ``tamanho = (b & $1F) + 3``. O ponteiro
                          de origem tambem anda e da' a volta no anel;
- ``$80 <= b <= $C0``     copia **relativa** a posicao corrente de escrita:
                          ``distancia = b & $F``, ``tamanho = ((b >> 4) & 3) + 2``.
                          A origem e' recalculada a cada byte a partir da
                          posicao de escrita, entao a copia e' deslizante e
                          funciona como RLE quando a distancia e' pequena;
- ``b > $C0``             bloco de ``(b & $3F) + 8`` literais direto do fluxo,
                          sem gastar um bit de controle por byte.

O anel comeca zerado com a posicao de escrita em $3DF. Ele fica em $FF8800 na
RAM do jogo; ver LzRingBuffer em analysis/ram-names.csv.

Uso:
    python tools/lzss.py 0x3e860              # descomprime e resume
    python tools/lzss.py 0x3e860 -o saida.bin
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import Rom, hexa  # noqa: E402

RING_SIZE = 1024
RING_MASK = RING_SIZE - 1
RING_START = 0x3DF          # posicao inicial de escrita: a rotina carrega $3DF em d4
END_TOKEN = 0x1F
HEADER_BYTES = 2           # word consumida pelo chamador antes de descomprimir
MAX_OUTPUT = 0x40000        # trava contra fluxo corrompido


class LzssError(Exception):
    pass


def decompress(data: bytes, src: int, max_output: int = MAX_OUTPUT) -> tuple[bytes, int]:
    """Descomprime a partir do inicio do fluxo. Devolve (saida, bytes consumidos).

    ``src`` e' o endereco que o jogo carrega com ``lea``. As duas primeiras
    words sao cabecalho: quem consome a primeira e' VdpUploadEntry ($001CFE),
    lendo uma word do fluxo para d0, antes de chamar $001E3A. Tratar o cabecalho aqui
    deixa as duas variantes com a mesma convencao de entrada.
    """
    ring = bytearray(RING_SIZE)
    write = RING_START
    out = bytearray()
    control = 0
    bits = 0
    start = src
    src += HEADER_BYTES

    def emit(byte: int) -> None:
        nonlocal write
        out.append(byte)
        ring[write] = byte
        write = (write + 1) & RING_MASK

    while True:
        if len(out) > max_output:
            raise LzssError(f"saida passou de {max_output} bytes a partir de {hexa(start)}")
        if src >= len(data):
            raise LzssError(f"fluxo acabou sem token de fim, a partir de {hexa(start)}")
        if bits == 0:
            control = data[src]
            src += 1
            bits = 8
        token = data[src]
        src += 1
        is_match = control & 1
        control >>= 1
        bits -= 1

        if not is_match:
            emit(token)
        elif token == END_TOKEN:
            break
        elif token < 0x80:
            offset = ((token << 3) & 0x300) | data[src]
            src += 1
            for _ in range((token & 0x1F) + 3):
                emit(ring[offset])
                offset = (offset + 1) & RING_MASK
        elif token <= 0xC0:
            distance = token & 0x0F
            for _ in range(((token >> 4) & 3) + 2):
                emit(ring[(write - distance) & RING_MASK])
        else:
            for _ in range((token & 0x3F) + 8):
                emit(data[src])
                src += 1

    return bytes(out), src - start


RING_START_B = 0x3C0        # a rotina B carrega $3C0 em d4, em $001DC6
RING_FILL_B = 0x20          # anel preenchido com espacos, LZSS de Okumura


def decompress_b(data: bytes, src: int, max_output: int = MAX_OUTPUT) -> tuple[bytes, int]:
    """Variante B, transcrita de $001DAC.

    Diferencas em relacao a variante A:

    - o anel comeca preenchido com ``$20`` e a escrita em ``$3C0``;
    - o fluxo comeca com **duas words**: uma que a rotina pula (avancando o ponteiro do fluxo em 2)
      e o tamanho em bytes do dado comprimido, que delimita o fim — nao ha'
      token de encerramento;
    - o bit de controle e' **invertido**: 1 significa literal, 0 significa copia;
    - a copia tem uma unica forma: dois bytes dao ``offset = ((b1 << 8) | b2) &
      $3FF`` e ``tamanho = (b1 >> 2) + 1``.
    """
    ring = bytearray([RING_FILL_B] * RING_SIZE)
    write = RING_START_B
    out = bytearray()
    start = src

    src += HEADER_BYTES                        # word que a rotina pula avancando o ponteiro em 2
    length = int.from_bytes(data[src:src + 2], "big")
    src += 2
    end = src + length

    def emit(byte: int) -> None:
        nonlocal write
        out.append(byte)
        ring[write] = byte
        write = (write + 1) & RING_MASK

    control = 0
    while src < end:
        if len(out) > max_output:
            raise LzssError(f"saida passou de {max_output} bytes a partir de {hexa(start)}")
        control >>= 1
        if not control & 0x100:                # marcador $FF00 esgotado
            control = 0xFF00 | data[src]
            src += 1
        if control & 1:
            emit(data[src])
            src += 1
        else:
            first, second = data[src], data[src + 1]
            src += 2
            offset = ((first << 8) | second) & RING_MASK
            for i in range((first >> 2) + 1):
                emit(ring[(offset + i) & RING_MASK])

    return bytes(out), src - start


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Descomprime um fluxo do jogo")
    parser.add_argument("address", help="endereco do fluxo comprimido (0x...)")
    parser.add_argument("--rom")
    parser.add_argument("-o", "--output", help="grava a saida crua neste arquivo")
    parser.add_argument("--variant", choices=("a", "b"), default="a",
                        help="a = $001E3A (padrao), b = $001DAC")
    args = parser.parse_args(argv)

    rom = Rom.load(args.rom)
    routine = decompress if args.variant == "a" else decompress_b
    out, consumed = routine(rom.data, int(args.address, 0))
    print(f"entrada : {hexa(int(args.address, 0))}, {consumed} bytes")
    print(f"saida   : {len(out)} bytes  (razao {len(out) / consumed:.2f}x)")
    print(f"tiles   : {len(out) / 32:.1f} de 32 bytes (4bpp 8x8)")
    if args.output:
        Path(args.output).write_bytes(out)
        print(f"gravado em {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
