"""Monta o pacote de release para Windows e o zip, com barreira.

O pacote NAO LEVA O JOGO. O C recompilado e' derivado da ROM e o projeto nao o
distribui (public/LEGAL.md). O que vai e' o runtime (tinytoon.exe, so' codigo
nosso e o SDL3) e o MONTADOR: o recompilador, os CSVs da analise, um Python
embarcado e o TinyCC. Na maquina de quem joga, o montador le' a ROM dele e gera
jogo.dll — pelo passo `prepare` do RecompLauncher, ou na primeira partida.

O pacote e' montado do ZERO em build/pacote/, a partir de uma lista explicita
de origens. build/release/ nao serve de base: e' a pasta de TESTE, e tem
rom.bin, jogo.dll montada, tinytoon.toml com o caminho da ROM de quem testou,
log e DLL de mod. Montar por lista e' o que garante que nada disso entra; a
barreira no fim confere de novo, por conteudo, para o dia em que a lista errar.

Terceiros, todos redistribuiveis, com hash fixado:

    Python 3.12.10 embarcado   PSF        MD5 conferido com o python.org
    capstone 5.0.7 (wheel)     BSD-3      SHA-256 conferido com o PyPI
    TinyCC 0.9.27 win64        LGPL-2.1   hash do primeiro download; o
                                          COPYING vem do fonte oficial
    SDL3 (estatico no exe)     zlib       licenca copiada da imagem mingw

Uso:
    python tools/montar_pacote.py v1.0.0
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from montar_jogo import hash_do_c  # noqa: E402
from preparar_publico import INSTRUCAO_68K  # noqa: E402
from romlib import project_root  # noqa: E402

TERCEIROS = {
    "python": ("https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
               "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"),
    "capstone": ("https://files.pythonhosted.org/packages/70/39/2138d890a8e827636b9de9924fbc8527fe83e38b6d26605b30ac55e30ebe/capstone-5.0.7-py3-none-win_amd64.whl",
                 "4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1"),
    "tcc": ("http://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win64-bin.zip",
            "34a721949a2583fdff725312da092fa0f5f1f284b702e6f811c6954714faabb2"),
    "tcc-src": ("http://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.bz2",
                "de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c"),
}

# O que o montador usa de tools/ (medido com modulefinder a partir de
# montar_jogo.py e recompile.py). codecs.py fica de fora de proposito: alem de
# nao ser usado, tem o nome de um modulo da biblioteca padrao.
MONTADOR_TOOLS = ["montar_jogo.py", "recompile.py", "trace_code.py", "romlib.py",
                  "m68k.py", "m68k_ops.py", "ciclos.py"]
# So' as ENTRADAS que o recompilador le' (romlib.py e trace_code.py). As saidas
# do tracer — mapas, paletas, instrucoes desmontadas — nao servem ao montador
# e algumas guardam conteudo do jogo; ficam fora do zip.
MONTADOR_ANALYSIS = ["data-regions.csv", "dispatchers.csv", "jump-tables.csv",
                     "ram-names.csv", "stack-returns.csv", "symbol-names.csv",
                     "rom-baseline.json"]
# O C gerado inclui m68k.h e memory.h; a ponte e' o lado do jogo da fronteira.
MONTADOR_RUNTIME = ["m68k.h", "memory.h", "jogo_api.h", "ponte_jogo.c"]

PROIBIDO_NOME = ["jogo.dll", "jogo.so", "*.bin", "*.smd", "*.gen", "*.md1",
                 "rom.*", "tinytoon.toml", "*.log", "*.pyc"]
TAMANHO_ROM = 1_048_576
# Marca de C gerado: toda funcao recompilada abre com MD_FUNCAO(0x<endereco>).
# Nao e' a frase "Gerado por tools/recompile.py" porque o proprio
# recompile.py, que vai no pacote, contem essa frase — e' ele que a escreve.
# Medido: o padrao abaixo esta' em 1.947 dos 1.948 .c gerados e em nenhum
# arquivo fonte do projeto.
MARCA_C_GERADO = re.compile(rb"MD_FUNCAO\(0x[0-9A-F]{6}\)")
PROIBIDO_TEXTO = [MARCA_C_GERADO]


def terceiro(raiz: Path, nome: str) -> Path:
    url, sha = TERCEIROS[nome]
    destino = raiz / "build" / "terceiros" / nome / url.rsplit("/", 1)[-1]
    if not destino.is_file():
        destino.parent.mkdir(parents=True, exist_ok=True)
        print(f"  baixando {url}")
        with urllib.request.urlopen(url) as r, destino.open("wb") as fh:
            shutil.copyfileobj(r, fh)
    obtido = hashlib.sha256(destino.read_bytes()).hexdigest()
    if obtido != sha:
        raise SystemExit(f"{destino.name}: SHA-256 {obtido} != fixado {sha}")
    return destino


def montar(raiz: Path, versao: str) -> Path:
    rel = raiz / "build" / "release"
    pac = raiz / "build" / "pacote"
    if pac.exists():
        shutil.rmtree(pac)
    pac.mkdir(parents=True)

    exe = (rel / "tinytoon.exe").read_bytes()
    # So' o build TINYTOON_JOGO_DLL referencia jogo_ligar. Sem isto, um
    # tinytoon.exe monolitico (com o jogo dentro) passaria pela barreira.
    if b"jogo_ligar" not in exe:
        raise SystemExit("build/release/tinytoon.exe nao e' o build so' de runtime")
    (pac / "tinytoon.exe").write_bytes(exe)
    for nome in ("README.txt", "recomplauncher.json"):
        shutil.copyfile(rel / nome, pac / nome)
    (pac / "mods").mkdir()
    shutil.copyfile(rel / "mods-LEIA.txt", pac / "mods" / "README.txt")

    # montador/: o recompilador e o que ele le'.
    m = pac / "montador"
    for sub in ("tools", "analysis", "runtime"):
        (m / sub).mkdir(parents=True)
    for nome in MONTADOR_TOOLS:
        shutil.copyfile(raiz / "tools" / nome, m / "tools" / nome)
    for nome in MONTADOR_ANALYSIS:
        shutil.copyfile(raiz / "analysis" / nome, m / "analysis" / nome)
    for nome in MONTADOR_RUNTIME:
        shutil.copyfile(raiz / "runtime" / nome, m / "runtime" / nome)
    # O hash do C verificado aqui. O montador confere o que gerar contra ele.
    recomp = raiz / "build" / "recomp" / "all"
    (m / "recomp.sha256").write_text(
        f"{hash_do_c(recomp)}  C recompilado verificado, build {versao}\n",
        encoding="ascii")

    # python/: embarcado + capstone. O ._pth e' o sys.path inteiro do Python
    # embarcado (ele ignora PYTHONPATH e o registro de proposito).
    py = pac / "python"
    zipfile.ZipFile(terceiro(raiz, "python")).extractall(py)
    zipfile.ZipFile(terceiro(raiz, "capstone")).extractall(py / "Lib" / "site-packages")
    (py / "python312._pth").write_text("python312.zip\n.\nLib\\site-packages\n",
                                       encoding="ascii")

    # tcc/: o binario oficial; examples/ nao serve a ninguem aqui.
    zipfile.ZipFile(terceiro(raiz, "tcc")).extractall(pac)
    shutil.rmtree(pac / "tcc" / "examples", ignore_errors=True)

    # Licencas. A nossa e a de cada terceiro, num lugar so'.
    lic = pac / "LICENSES"
    lic.mkdir()
    # public/LICENSE no repositorio de trabalho; LICENSE na raiz do publico.
    nossa = raiz / "public" / "LICENSE"
    if not nossa.is_file():
        nossa = raiz / "LICENSE"
    shutil.copyfile(nossa, lic / "tinytoon-UNLICENSE.txt")
    shutil.copyfile(py / "LICENSE.txt", lic / "python-PSF.txt")
    shutil.copyfile(next((py / "Lib" / "site-packages").glob("capstone-*.dist-info"))
                    / "LICENSE.TXT", lic / "capstone-BSD.txt")
    shutil.copyfile(raiz / "build" / "terceiros" / "sdl3" / "LICENSE.txt",
                    lic / "SDL3-zlib.txt")
    import tarfile
    with tarfile.open(terceiro(raiz, "tcc-src")) as t:
        copying = t.extractfile("tcc-0.9.27/COPYING").read()
    (lic / "tinycc-LGPL-2.1.txt").write_bytes(
        b"TinyCC 0.9.27, unmodified official Windows binaries.\n"
        b"Source: http://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.bz2\n\n"
        + copying)
    return pac


def barreira(pac: Path) -> list[str]:
    import fnmatch
    problemas = []
    for f in sorted(p for p in pac.rglob("*") if p.is_file()):
        rel = f.relative_to(pac).as_posix()
        if any(fnmatch.fnmatch(f.name.lower(), x) for x in PROIBIDO_NOME):
            problemas.append(f"{rel}: nome proibido")
            continue
        if f.stat().st_size == TAMANHO_ROM:
            problemas.append(f"{rel}: tem exatamente o tamanho da ROM")
            continue
        dados = f.read_bytes()
        for agulha in PROIBIDO_TEXTO:
            if agulha.search(dados):
                problemas.append(f"{rel}: contem {agulha.pattern[:30]!r}")
        # Codigo do jogo citado em texto: a mesma regra do repositorio publico,
        # mas so' no que e' nosso (montador/). Python e TinyCC sao de terceiros e
        # nao tem nada do jogo; os headers do tcc, por exemplo, tem nomes de
        # registrador em comentario que casariam sem significar nada.
        if rel.startswith("montador/") or rel in ("README.txt", "recomplauncher.json"):
            for n, linha in enumerate(dados.decode("utf-8", "replace").split("\n"), 1):
                achado = INSTRUCAO_68K.search(linha)
                if achado:
                    problemas.append(f"{rel}:{n}: cita codigo do jogo ({achado.group()!r})")
                    break
        # Uma jogo.dll RENOMEADA nao tem nome nem marca de texto: e' codigo
        # compilado. Mas toda jogo.dll exporta jogo_ligar, e o unico binario
        # do pacote que pode citar esse nome e' o tinytoon.exe, que a procura.
        if (rel != "tinytoon.exe" and f.suffix.lower() in (".dll", ".pyd", ".exe")
                and b"jogo_ligar" in dados):
            problemas.append(f"{rel}: binario que exporta jogo_ligar (o jogo)")
    return problemas


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("versao")
    args = p.parse_args(argv)
    raiz = project_root()

    pac = montar(raiz, args.versao)
    problemas = barreira(pac)
    if problemas:
        print("PACOTE RECUSADO:")
        for x in problemas:
            print(f"  {x}")
        return 1

    destino = raiz / "build" / "release" / f"TinyToon-{args.versao}-Windows-x64.zip"
    destino.unlink(missing_ok=True)
    arquivos = sorted(f for f in pac.rglob("*") if f.is_file())
    with zipfile.ZipFile(destino, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in arquivos:
            z.write(f, f.relative_to(pac).as_posix())
    print(f"{destino.name}: {destino.stat().st_size:,} bytes, {len(arquivos)} arquivos")
    for topo in sorted({f.relative_to(pac).parts[0] for f in arquivos}):
        print(f"  {topo}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
