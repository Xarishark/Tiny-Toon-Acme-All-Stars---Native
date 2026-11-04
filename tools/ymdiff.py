"""Compara dois WAV do mesmo trecho de jogo: o meu YM2612 e o Nuked-OPN2.

Nao compara amostra a amostra em valor absoluto — nao adianta. Os dois chips
partem do mesmo estado mas divergem em fase por pequenas diferencas de
arredondamento, e um deslocamento de uma amostra ja' zeraria a correlacao sem
que houvesse nada de errado.

O que interessa e' o que o ouvido percebe, medido:

  ENERGIA        se um esta' mais baixo que o outro, e' escala.
  CENTRO ESPECTRAL  onde a energia se concentra em frequencia. E' esta que
                 responde "abafado": som abafado tem o centro mais grave.
  ENERGIA POR FAIXA  onde exatamente o agudo esta' faltando.
  ESTEREO        quanto os dois lados diferem.

Uso: python tools/ymdiff.py meu.wav ref.wav
"""

from __future__ import annotations

import cmath
import math
import struct
import sys


def ler_wav(caminho: str):
    d = open(caminho, "rb").read()
    canais = struct.unpack("<H", d[22:24])[0]
    taxa = struct.unpack("<I", d[24:28])[0]
    # O bloco "data" nem sempre esta' no offset 36; procura pelo nome.
    i = d.find(b"data", 12)
    n = struct.unpack("<I", d[i + 4:i + 8])[0]
    ini = i + 8
    amostras = struct.unpack("<%dh" % (n // 2), d[ini:ini + n])
    if canais == 2:
        return taxa, amostras[0::2], amostras[1::2]
    return taxa, amostras, amostras


def dft_potencia(bloco, nbandas, taxa):
    """Potencia em frequencias espacadas logaritmicamente — como o ouvido ouve.

    Sem numpy nesta maquina, mas nao se decima o sinal: a primeira versao pulava
    duas de cada tres amostras "para ficar 3x mais rapido", o que ALIASA tudo
    acima de taxa/6 e transformava justamente as bandas agudas — as que a medicao
    existe para julgar — em lixo. Medir errado a coisa que se quer medir e' pior
    que nao medir.

    A velocidade vem de um fasor incremental em vez de exp() por amostra.
    """
    n = len(bloco)
    bandas = []
    for b in range(nbandas):
        f = 60.0 * (2.0 ** (b * 0.75))          # 60 Hz subindo ~meia oitava
        if f > taxa / 2.2:                      # com folga do Nyquist
            break
        passo = cmath.exp(2j * math.pi * f / taxa)
        fasor = 1 + 0j
        acc = 0j
        for x in bloco:
            acc += x * fasor
            fasor *= passo
        bandas.append((f, abs(acc) / n))
    return bandas


def centro_espectral(bandas):
    num = sum(f * p for f, p in bandas)
    den = sum(p for _, p in bandas)
    return num / den if den else 0.0


def rms(b):
    return math.sqrt(sum(x * x for x in b) / len(b)) if b else 0.0


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    taxa_a, ae, ad = ler_wav(argv[1])
    taxa_b, be, bd = ler_wav(argv[2])
    print(f"meu: {len(ae)/taxa_a:.1f} s a {taxa_a} Hz | "
          f"ref: {len(be)/taxa_b:.1f} s a {taxa_b} Hz")

    n = min(len(ae), len(be))
    # Trechos com som dos DOIS lados, para nao medir silencio.
    janela = taxa_a  # 1 s
    trechos = []
    for ini in range(0, n - janela, janela):
        ra = rms(ae[ini:ini + janela])
        rb = rms(be[ini:ini + janela])
        # Limiar baixo de proposito: a escala natural do Nuked e' ~10x menor que
        # a minha, e um limiar alto descarta a referencia inteira como silencio —
        # foi o que aconteceu na primeira tentativa.
        if ra > 20 and rb > 20:
            trechos.append(ini)
    if not trechos:
        print("nenhum trecho com som nos dois — nada a comparar")
        return 1
    print(f"{len(trechos)} trechos de 1 s com som nos dois lados\n")

    print(f"{'trecho':>7}  {'rms meu':>8} {'rms ref':>8} {'razao':>6}   "
          f"{'centro meu':>10} {'centro ref':>10}  {'razao':>6}")
    somas = [0.0, 0.0, 0.0, 0.0]
    escolhidos = trechos[::max(1, len(trechos) // 6)][:6]
    for ini in escolhidos:
        a = ae[ini:ini + janela]
        b = be[ini:ini + janela]
        ra, rb = rms(a), rms(b)
        ca = centro_espectral(dft_potencia(a, 14, taxa_a))
        cb = centro_espectral(dft_potencia(b, 14, taxa_b))
        print(f"{ini//taxa_a:5d}s  {ra:8.0f} {rb:8.0f} {ra/rb if rb else 0:6.2f}   "
              f"{ca:9.0f}Hz {cb:9.0f}Hz  {ca/cb if cb else 0:6.2f}")
        somas[0] += ra; somas[1] += rb; somas[2] += ca; somas[3] += cb

    k = len(escolhidos)
    print(f"\nmedia: energia meu/ref = {somas[0]/somas[1]:.2f}, "
          f"centro espectral meu/ref = {somas[2]/somas[3]:.2f}")
    if somas[2] / somas[3] < 0.85:
        print("  -> o meu tem o centro mais GRAVE: e' o 'abafado'. Falta agudo.")
    elif somas[2] / somas[3] > 1.18:
        print("  -> o meu tem o centro mais AGUDO que a referencia.")
    else:
        print("  -> centro espectral compativel.")

    # Onde exatamente difere, no trecho de maior energia.
    melhor = max(escolhidos, key=lambda i: rms(ae[i:i + janela]))
    ba = dft_potencia(ae[melhor:melhor + janela], 14, taxa_a)
    bb = dft_potencia(be[melhor:melhor + janela], 14, taxa_b)
    print(f"\nenergia por faixa no trecho de {melhor//taxa_a}s "
          f"(razao meu/ref por frequencia):")
    ta = sum(p for _, p in ba) or 1
    tb = sum(p for _, p in bb) or 1
    for (f, pa), (_, pb) in zip(ba, bb):
        # Normalizado pela energia total: mede TIMBRE, nao volume.
        pa, pb = pa / ta, pb / tb
        r = pa / pb if pb else 0
        barra = "#" * min(30, int(r * 10))
        print(f"  {f:7.0f} Hz  meu {pa*100:5.1f}%  ref {pb*100:5.1f}%  {r:5.2f} {barra}")

    def largura(e, d):
        return sum(abs(x - y) for x, y in zip(e, d)) / max(1, len(e))
    print(f"\nlargura estereo (media |L-R|): meu {largura(ae, ad):.0f}, "
          f"ref {largura(be, bd):.0f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
