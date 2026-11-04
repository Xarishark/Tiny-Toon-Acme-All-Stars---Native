"""Funde os mapas de leitura de varias corridas num so'.

Cobertura de ROM e' monotona: byte que alguem ja' viu o jogo ler nao volta a ser
desconhecido. Entao a fusao e' um OU logico, e ACUMULA sobre build/romlido.bin
em vez de sobrescrever — a sessao de hoje soma com a de ontem.

Para o mapa de leitor a regra e' outra: o primeiro leitor vence. Um bloco
costuma ter um dono e varios visitantes, e o dono e' quem chega primeiro.
Como as corridas rodam em paralelo, "primeiro" aqui e' por ordem de diretorio,
que e' estavel — o resultado da fusao nao depende de quem terminou antes.

Uso:
    python tools/fundir_leituras.py build/cob/cob
    python tools/fundir_leituras.py build/cob/cob --zerar   # comeca do zero
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import project_root  # noqa: E402

TAM = 0x100000


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("dir", help="diretorio com subpastas contendo romlido/romleitor")
    p.add_argument("--zerar", action="store_true",
                   help="ignora o mapa acumulado em build/ e recomeca")
    args = p.parse_args(argv)

    raiz = project_root()
    alvo_lido = raiz / "build" / "romlido.bin"
    alvo_leitor = raiz / "build" / "romleitor.bin"

    lido = bytearray(TAM)
    leitor = [0] * TAM
    fontes = 0

    if not args.zerar and alvo_lido.exists() and alvo_leitor.exists():
        lido = bytearray(alvo_lido.read_bytes()[:TAM])
        lido.extend(b"\0" * (TAM - len(lido)))
        leitor = list(struct.unpack(f">{TAM}I", alvo_leitor.read_bytes()[:TAM * 4]))
        fontes += 1

    entradas = sorted(Path(args.dir).glob("*/romlido.bin"),
                      key=lambda q: (len(q.parent.name), q.parent.name))
    for cam in entradas:
        cam2 = cam.parent / "romleitor.bin"
        if not cam2.exists():
            continue
        d = cam.read_bytes()
        q = struct.unpack(f">{len(cam2.read_bytes())//4}I", cam2.read_bytes())
        for i in range(min(TAM, len(d))):
            if d[i]:
                lido[i] |= d[i]
                if not leitor[i] and i < len(q):
                    leitor[i] = q[i]
        fontes += 1

    alvo_lido.parent.mkdir(parents=True, exist_ok=True)
    alvo_lido.write_bytes(bytes(lido))
    alvo_leitor.write_bytes(struct.pack(f">{TAM}I", *leitor))

    n = sum(1 for b in lido if b)
    print(f"fundidas {fontes} fontes -> {n} bytes lidos ({100*n/TAM:.2f}% da ROM)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
