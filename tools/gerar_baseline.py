"""Gera runtime/rom_baseline.h a partir de analysis/rom-baseline.json.

O runtime precisa saber que ROM ele espera, e essa informacao ja' existe — em
JSON, curada a mao, usada pelo portao de regressao. Copiar os valores para o C
a mao criaria uma segunda fonte da verdade, e duas fontes divergem: alguem
atualiza o JSON, o header fica para tras, e o executavel passa a aceitar uma
ROM que o projeto nao valida mais.

Entao o header e' GERADO. Nunca editar rom_baseline.h a mao.

Uso:
    python tools/gerar_baseline.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import project_root  # noqa: E402


def c_bytes(hexa: str) -> str:
    b = bytes.fromhex(hexa)
    return ", ".join(f"0x{x:02X}" for x in b)


def main() -> int:
    raiz = project_root()
    base = json.loads((raiz / "analysis" / "rom-baseline.json").read_text(encoding="utf-8"))
    h = base["header"]

    saida = f'''/* GERADO por tools/gerar_baseline.py a partir de analysis/rom-baseline.json.
 * Nao editar a mao: regenerar.
 *
 * E' a identidade da ROM que este port foi construido para executar. O runtime
 * usa isto para dizer ao usuario O QUE esta' errado com o arquivo dele, em vez
 * de bootar em cima de lixo e mostrar uma tela preta.
 *
 * Fonte da verdade: analysis/rom-baseline.json.
 */

#ifndef TINYTOON_ROM_BASELINE_H
#define TINYTOON_ROM_BASELINE_H

#define ROM_TAMANHO        {base["size"]}u
#define ROM_CHECKSUM       0x{base["checksum_stored"]:04X}u
#define ROM_CONSOLE        "{h["console"]}"
#define ROM_TITULO         "{h["title_overseas"]}"
#define ROM_SERIAL         "{h["serial"]}"
#define ROM_REGIAO         '{h["region"]}'
#define ROM_ENTRY          0x{base["entry_point"]:06X}u
#define ROM_SP_INICIAL     0x{base["initial_sp"]:08X}u
#define ROM_SHA1_TEXTO     "{base["sha1"]}"

static const unsigned char ROM_SHA1[20] = {{ {c_bytes(base["sha1"])} }};

#endif /* TINYTOON_ROM_BASELINE_H */
'''
    destino = raiz / "runtime" / "rom_baseline.h"
    destino.write_text(saida, encoding="ascii", errors="strict")
    print(f"gravado {destino.relative_to(raiz)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
