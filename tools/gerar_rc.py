"""Gera o recurso do Windows: icone e informacao de versao.

Escreve BUILD/tinytoon.rc e BUILD/icone.ico, que o windres compila num objeto e
o linker embute no .exe. E' o que faz o programa ter icone no Explorer e mostrar
algo util em Propriedades em vez de nada.

Gerado, e nao mantido a mao, porque carrega a versao do build. Um .rc fixo com a
versao escrita a mao daria a divergencia silenciosa de sempre: alguem commita,
a versao muda, o .rc continua dizendo a antiga.

E gerado em PYTHON, e nao por um heredoc no script de build, por uma razao
aprendida na pratica: o conteudo atravessava ssh, docker, make e a shell da
regra de compilacao, e cada camada comia um nivel de aspas.

Uso:
    python tools/gerar_rc.py [--saida build]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import project_root  # noqa: E402


def ler_versao(raiz: Path) -> tuple[str, str]:
    """Le runtime/versao.h, que gerar_versao.py acabou de escrever."""
    h = raiz / "runtime" / "versao.h"
    versao, data = "unknown", "0000-00-00"
    if h.exists():
        texto = h.read_text(encoding="utf-8")
        m = re.search(r'#define TINYTOON_VERSAO\s+"([^"]*)"', texto)
        if m:
            versao = m.group(1)
        m = re.search(r'#define TINYTOON_DATA\s+"([^"]*)"', texto)
        if m:
            data = m.group(1)
    return versao, data


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--saida", default="build")
    args = p.parse_args(argv)

    raiz = project_root()
    saida = Path(args.saida)
    if not saida.is_absolute():
        saida = raiz / saida
    saida.mkdir(parents=True, exist_ok=True)

    # O icone vai para o lado do .rc porque o windres resolve o caminho
    # relativo ao arquivo de recurso, nao ao diretorio corrente.
    subprocess.run([sys.executable, str(raiz / "tools" / "gerar_icone.py")],
                   check=True, capture_output=True)
    origem = raiz / "build" / "icone.ico"
    if origem.resolve() != (saida / "icone.ico").resolve():
        (saida / "icone.ico").write_bytes(origem.read_bytes())

    versao, data = ler_versao(raiz)
    # vX.Y.Z vira X,Y,Z,0 nos campos numericos que o Explorer mostra. Build sem
    # tag fica 0,0,0,0 — honesto: nao e' release nenhuma. Antes era 1,0,0,0
    # fixo, que fazia todo build de desenvolvimento se dizer 1.0.
    m = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", versao)
    numerica = f"{m[1]},{m[2]},{m[3]},0" if m else "0,0,0,0"
    (saida / "tinytoon.rc").write_text(f'''#include <winver.h>

1 ICON "icone.ico"

VS_VERSION_INFO VERSIONINFO
FILEVERSION     {numerica}
PRODUCTVERSION  {numerica}
FILEOS          VOS__WINDOWS32
FILETYPE        VFT_APP
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904E4"
    BEGIN
      VALUE "ProductName",      "Tiny Toon Adventures: ACME All-Stars"
      VALUE "FileDescription",  "Native port, statically recompiled from the original 68000 code"
      VALUE "FileVersion",      "{versao}"
      VALUE "ProductVersion",   "{versao} ({data})"
      VALUE "Comments",         "Requires the original ROM, which is not distributed with this program."
      VALUE "InternalName",     "tinytoon"
      VALUE "OriginalFilename", "tinytoon.exe"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1252
  END
END
''', encoding="ascii", newline="\n")

    print(f"gravado {saida.name}/tinytoon.rc e {saida.name}/icone.ico "
          f"(versao {versao})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
