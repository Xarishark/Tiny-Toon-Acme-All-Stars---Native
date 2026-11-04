"""Gera runtime/versao.h com a identidade do build.

Existe porque passar a versao por -DTINYTOON_VERSAO=\\"...\\" nao sobrevive: o
valor atravessa ssh, depois docker, depois make, depois a shell da regra de
compilacao, e cada camada come um nivel de aspas. O sintoma foi
`error: invalid suffix "b98d" on integer constant` — o compilador recebendo o
hash do git sem aspas nenhuma.

Header gerado nao tem camada: e' um arquivo, e o arquivo chega igual.

A marca "-sujo" nao e' cosmetica. Um build feito com mudanca nao commitada nao
existe em lugar nenhum alem da maquina que o gerou, e um relato de bug sobre ele
nao da' para reproduzir. Melhor que isso esteja escrito no proprio executavel.

Uso:
    python tools/gerar_versao.py
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import project_root  # noqa: E402


def git(raiz: Path, *args: str) -> str | None:
    try:
        r = subprocess.run(["git", "-C", str(raiz), *args],
                           capture_output=True, text=True, timeout=15)
        return r.stdout.strip() if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def main() -> int:
    raiz = project_root()
    # A TAG, quando o commit tem uma: e' o nome com que a release aparece no
    # GitHub e no RecompLauncher, e um relato de bug que diz "v1.0.0" casa com
    # o que o jogador baixou. Sem tag, o hash — e' um build de desenvolvimento.
    # Com mudanca nao commitada a tag nao vale: o binario nao e' aquela release.
    tag = git(raiz, "describe", "--tags", "--exact-match", "HEAD")
    versao = tag or git(raiz, "rev-parse", "--short", "HEAD") or "desconhecida"
    data = git(raiz, "log", "-1", "--format=%cs") or "0000-00-00"
    if git(raiz, "status", "--porcelain"):
        versao = (git(raiz, "rev-parse", "--short", "HEAD") or versao) + "-sujo"

    destino = raiz / "runtime" / "versao.h"
    destino.write_text(f'''/* GERADO por tools/gerar_versao.py. Nao editar a mao.
 *
 * "-sujo" quer dizer build feito com mudanca nao commitada: ele nao existe em
 * nenhum outro lugar, e nada relatado sobre ele e' reproduzivel.
 */

#ifndef TINYTOON_VERSAO_H
#define TINYTOON_VERSAO_H

#define TINYTOON_VERSAO "{versao}"
#define TINYTOON_DATA   "{data}"

#endif /* TINYTOON_VERSAO_H */
''', encoding="ascii")
    print(f"versao {versao} ({data})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
