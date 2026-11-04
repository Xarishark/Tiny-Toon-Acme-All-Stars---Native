"""Os tres codecs da ROM, em Python.

Reimplementacao instrucao a instrucao de `LzDecompressA` ($001E3A), de
`LzDecompressB` ($001DAC) e de `RleUnpack` ($01AD02). Serve a duas coisas:

**Extrair dados.** E' a trilha que o diagrama do projeto chama de "extracao ->
dados do jogo" e que ate' agora quase nao existia. Sem descompressor em Python,
a arte e os mapas do jogo so' existem dentro do port rodando.

**Provar fronteira de bloco.** Os dois LZ se AUTO-DELIMITAM: o B carrega um campo
de tamanho no inicio, o A termina num token de fim. Entao decodificar responde
exatamente onde o bloco acaba — e isso e' prova, nao palpite. Um bloco que o
mapa de leitura marcou como desconhecido, se decodificar limpo e terminar
exatamente onde o bloco seguinte comeca, deixa de ser inferencia.

O anel de 1024 bytes mora em $FFFF8800 na RAM do jogo e e' o mesmo nos dois LZ,
com inicializacoes diferentes:

    LzDecompressA   1024 bytes zerados, posicao inicial $3DF
    LzDecompressB   960 bytes de $20,   posicao inicial $3C0

Os 64 bytes finais do anel do B ficam com o que estava na RAM. Num fluxo bem
formado eles sao escritos antes de lidos; se algum stream ler de la', o decoder
avisa em vez de inventar — ver `ANEL_SUJO`.
"""

from __future__ import annotations

TAM_ANEL = 0x400
MASCARA = TAM_ANEL - 1

# O B so' preenche 960 dos 1024 bytes do anel. Ler as 64 posicoes de cima antes
# de escrever nelas significaria que o resultado depende de lixo de RAM — e o
# decoder tem que denunciar isso, nao fingir que sabe.
ANEL_SUJO = range(0x3C0, 0x400)


class Falha(Exception):
    """O fluxo nao decodifica. Nunca 'quase decodifica'."""


def lz_b(dados: bytes, pos: int, limite: int = 1 << 20) -> tuple[bytes, int]:
    """LzDecompressB ($001DAC). Devolve (saida, bytes consumidos).

    Delimitado por TAMANHO: a primeira word diz quantos bytes de fluxo vem
    depois dela. E' isso que torna o bloco reconhecivel sem contexto nenhum.
    """
    if pos + 2 > len(dados):
        raise Falha("acabou a ROM antes do campo de tamanho")
    comprimento = (dados[pos] << 8) | dados[pos + 1]
    p = pos + 2
    fim = p + comprimento
    if fim > len(dados):
        raise Falha(f"tamanho {comprimento} passa do fim da ROM")

    anel = bytearray(b"\x20" * 0x3C0 + b"\x20" * 0x40)
    escrita = 0x3C0
    lidas_sujas = 0
    escrito = set()
    saida = bytearray()
    flags = 0

    while p < fim:
        flags >>= 1
        if not (flags & 0x100):
            if p >= fim:
                break
            flags = 0xFF00 | dados[p]
            p += 1
        if flags & 1:                       # literal
            if p >= fim:
                raise Falha("literal truncado")
            b = dados[p]; p += 1
            saida.append(b)
            anel[escrita] = b
            escrito.add(escrita)
            escrita = (escrita + 1) & MASCARA
        else:                               # referencia ao anel
            if p + 2 > fim:
                raise Falha("referencia truncada")
            b0, b1 = dados[p], dados[p + 1]; p += 2
            deslocamento = ((b0 << 8) | b1) & MASCARA
            n = (b0 >> 2) + 1
            for k in range(n):
                origem = (deslocamento + k) & MASCARA
                if origem in ANEL_SUJO and origem not in escrito:
                    lidas_sujas += 1
                b = anel[origem]
                saida.append(b)
                anel[escrita] = b
                escrito.add(escrita)
                escrita = (escrita + 1) & MASCARA
        if len(saida) > limite:
            raise Falha("saida grande demais; provavelmente nao e' um fluxo B")

    if p != fim:
        raise Falha(f"parou em {p}, esperado {fim}")
    if lidas_sujas:
        raise Falha(f"{lidas_sujas} leituras da parte nao inicializada do anel")
    return bytes(saida), fim - pos


def lz_a(dados: bytes, pos: int, limite: int = 1 << 20) -> tuple[bytes, int]:
    """LzDecompressA ($001E3A). Devolve (saida, bytes consumidos).

    Delimitado por TOKEN DE FIM: o comando $1F encerra. Quatro formas de token,
    escolhidas pela faixa do byte:

        bit de flag 0        literal de 1 byte
        $00..$1E             copia do anel, deslocamento absoluto de 10 bits,
                             comprimento (b & $1F) + 3
        $1F                  fim
        $80..$C0             copia relativa, distancia b & $0F,
                             comprimento ((b >> 4) & 3) + 2
        $C1..$FF             corrida literal de (b & $3F) + 8 bytes
    """
    anel = bytearray(TAM_ANEL)
    escrita = 0x3DF
    saida = bytearray()
    p = pos
    flags = 0
    restantes = 0

    while True:
        if p >= len(dados):
            raise Falha("acabou a ROM sem token de fim")
        if restantes == 0:
            flags = dados[p]; p += 1
            restantes = 8
            if p >= len(dados):
                raise Falha("acabou a ROM depois do byte de flags")
        restantes -= 1
        b = dados[p]; p += 1
        bit = flags & 1
        flags >>= 1

        if not bit:                                   # literal
            saida.append(b)
            anel[escrita] = b
            escrita = (escrita + 1) & MASCARA
        elif b < 0x80:
            if b == 0x1F:
                break                                 # fim do fluxo
            if p >= len(dados):
                raise Falha("comando de copia truncado")
            baixo = dados[p]; p += 1
            origem = (((b << 3) & 0xFF00) | baixo) & MASCARA
            n = (b & 0x1F) + 3
            for _ in range(n):
                v = anel[origem]
                saida.append(v)
                anel[escrita] = v
                escrita = (escrita + 1) & MASCARA
                origem = (origem + 1) & MASCARA
        elif b <= 0xC0:                               # copia relativa curta
            n = ((b >> 4) & 3) + 2
            dist = b & 0x0F
            for _ in range(n):
                v = anel[(escrita - dist) & MASCARA]
                saida.append(v)
                anel[escrita] = v
                escrita = (escrita + 1) & MASCARA
        else:                                         # corrida de literais
            n = (b & 0x3F) + 8
            if p + n > len(dados):
                raise Falha("corrida literal passa do fim da ROM")
            for _ in range(n):
                v = dados[p]; p += 1
                saida.append(v)
                anel[escrita] = v
                escrita = (escrita + 1) & MASCARA

        if len(saida) > limite:
            raise Falha("saida grande demais; provavelmente nao e' um fluxo A")

    return bytes(saida), p - pos


def rle(dados: bytes, pos: int, bytes_destino: int) -> tuple[bytes, int]:
    """RleUnpack ($01AD02), variante sem mascara. Devolve (saida, consumidos).

    Delimitado pelo DESTINO, nao pela origem: para quando o buffer enche. O
    comprimento vem do chamador (d1 * 32 bytes, teto de 2 KB por fatia), entao
    este e' o unico dos tres que nao da' para reconhecer sozinho na ROM.
    """
    saida = bytearray()
    p = pos
    while len(saida) < bytes_destino:
        if p + 2 > len(dados):
            raise Falha("acabou a ROM no meio do fluxo RLE")
        w = (dados[p] << 8) | dados[p + 1]; p += 2
        if w & 0x8000:                                # literais
            n = w - 0x8000
            if p + n * 2 > len(dados):
                raise Falha("literais RLE passam do fim da ROM")
            saida += dados[p:p + n * 2]
            p += n * 2
        else:                                         # corrida
            n = w >> 8
            v = w & 0xFF
            if n == 0:
                raise Falha("corrida de comprimento zero")
            saida += bytes([v, v]) * n
    return bytes(saida[:bytes_destino]), p - pos
