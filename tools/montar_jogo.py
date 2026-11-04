"""Monta o jogo a partir da ROM do jogador: recompila e compila em jogo.dll.

Roda NA MAQUINA DE QUEM JOGA. O C recompilado e' traducao mecanica do codigo
da Konami — obra derivada da ROM — e o projeto nao o distribui
(public/LEGAL.md). O zip de release leva o runtime, este montador, um Python
embarcado e o TinyCC; o jogo propriamente dito nasce aqui, do dump de quem
joga. Ver runtime/jogo_api.h para a fronteira entre os dois binarios.

Quem chama:
    - o RecompLauncher, no passo `prepare` do recomplauncher.json, sem janela;
    - o tinytoon.exe, na primeira partida, numa janela de console.

Fala com o launcher pelo protocolo dele: linhas `progress <0..1> <texto>` na
saida, codigo 0 quando deu certo. Os codigos de erro sao os mesmos do
executavel (rom_check.h, jogo_carga.h), para o exit_codes do
recomplauncher.json servir aos dois.

Uso:
    python tools/montar_jogo.py --rom CAMINHO --saida PASTA [--tcc TCC]
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from romlib import project_root  # noqa: E402

# Os mesmos de runtime/rom_check.h (SAIDA_ROM_BASE + veredito) e
# runtime/jogo_carga.h.
SAIDA_OUTRA_REVISAO = 11
SAIDA_VAZIA = 12
SAIDA_TAMANHO = 13
SAIDA_SMD = 14
SAIDA_NAO_MONTADO = 20

TAMANHO_ROM = 1_048_576


def progresso(fracao: float, texto: str) -> None:
    print(f"progress {fracao:.2f} {texto}", flush=True)


def falhar(codigo: int, texto: str) -> int:
    print(f"\nERRO: {texto}", flush=True)
    return codigo


def conferir_rom(dados: bytes, raiz: Path) -> tuple[int, str]:
    """(codigo de saida ou 0, sha1). Os casos que importam aqui sao os que
    impedem MONTAR; o executavel repete a verificacao completa na partida."""
    sha1 = hashlib.sha1(dados).hexdigest()
    if not dados:
        return SAIDA_VAZIA, sha1
    if len(dados) % 16384 == 512:
        return SAIDA_SMD, sha1
    if len(dados) != TAMANHO_ROM:
        return SAIDA_TAMANHO, sha1
    esperado = json.loads((raiz / "analysis" / "rom-baseline.json")
                          .read_text(encoding="utf-8"))["sha1"]
    # Revisao exata, e nao "o jogo certo": o recompilador traduz os enderecos
    # da revisao analisada. Montar de outra produziria um jogo que compila e
    # roda com codigo nos lugares errados.
    if sha1 != esperado:
        return SAIDA_OUTRA_REVISAO, sha1
    return 0, sha1


def hash_do_c(pasta: Path) -> str:
    """SHA-256 do C gerado, nome e conteudo, em ordem.

    O recompilador grava com write_text, que no Windows poe CRLF: o hash de
    referencia (recomp.sha256, gravado pelo empacotar.sh) e' do build feito
    no Windows, e e' com ele que o montador do zip — que so' roda no Windows —
    compara. Em outro sistema o arquivo de referencia nao existe e a
    conferencia e' pulada."""
    h = hashlib.sha256()
    for f in sorted(pasta.glob("*.c")):
        h.update(f.name.encode() + b"\0" + f.read_bytes() + b"\0")
    return h.hexdigest()


def achar_tcc(raiz: Path, pedido: str | None) -> str | None:
    if pedido:
        return pedido
    embarcado = raiz.parent / "tcc" / ("tcc.exe" if os.name == "nt" else "tcc")
    if embarcado.is_file():
        return str(embarcado)
    return shutil.which("tcc")


def montar(args: argparse.Namespace) -> int:
    raiz = project_root()
    saida = Path(args.saida).resolve()
    nome = "jogo.dll" if os.name == "nt" else "jogo.so"

    progresso(0.02, "Checking your ROM")
    try:
        dados = Path(args.rom).read_bytes()
    except OSError as exc:
        return falhar(SAIDA_VAZIA, f"could not read the ROM: {exc}")
    codigo, sha1 = conferir_rom(dados, raiz)
    if codigo:
        motivos = {
            SAIDA_VAZIA: "the ROM file is empty.",
            SAIDA_SMD: "this is an SMD dump; this port needs the raw .bin.",
            SAIDA_TAMANHO: f"the ROM must be exactly {TAMANHO_ROM:,} bytes.",
            SAIDA_OUTRA_REVISAO: (
                "this is not the European cartridge this port is made from "
                f"(SHA-1 {sha1}). Only GM T-95146-00, CRC32 1227b2b2, works."),
        }
        return falhar(codigo, motivos[codigo])

    tcc = achar_tcc(raiz, args.tcc)
    if not tcc:
        return falhar(SAIDA_NAO_MONTADO, "TinyCC not found (tcc/ next to montador/).")

    with tempfile.TemporaryDirectory(prefix="tinytoon-") as tmp:
        tmp = Path(tmp)
        gerado = tmp / "c"

        progresso(0.10, "Translating the game's 68000 code to C")
        t0 = time.monotonic()
        import recompile  # noqa: E402  (pesado: so' depois de a ROM passar)
        rom_tmp = tmp / "rom.bin"
        rom_tmp.write_bytes(dados)
        registro = io.StringIO()
        with contextlib.redirect_stdout(registro):
            r = recompile.main(["--rom", str(rom_tmp), "--all", str(gerado)])
        rom_tmp.unlink()
        if r != 0:
            print(registro.getvalue())
            return falhar(SAIDA_NAO_MONTADO, "the recompiler failed.")
        n = len(list(gerado.glob("*.c")))
        print(f"  {n} C files in {time.monotonic() - t0:.1f} s", flush=True)

        # O C tem de ser EXATAMENTE o que foi verificado no desenvolvimento.
        # Outra versao do capstone ou do Python que mudasse uma linha passaria
        # despercebida ate' um bug estranho no meio de uma partida.
        esperado_arq = raiz / "recomp.sha256"
        esperado = ""
        if esperado_arq.is_file():
            esperado = esperado_arq.read_text(encoding="ascii").split()[0]
            obtido = hash_do_c(gerado)
            if obtido != esperado:
                return falhar(SAIDA_NAO_MONTADO,
                    f"the generated C does not match the verified one "
                    f"({obtido[:12]} != {esperado[:12]}).")
            print("  generated C matches the verified build", flush=True)

        # Um arquivo so', incluindo os 1.948. O TinyCC compila tudo de uma vez
        # em poucos segundos, e a linha de comando do Windows (32.767
        # caracteres) nao comportaria os nomes. O C gerado nao tem `static`,
        # entao nada colide.
        unico = tmp / "jogo_unico.c"
        with unico.open("w", encoding="ascii", newline="\n") as fh:
            fh.write("/* gerado por montar_jogo.py */\n")
            for f in sorted(gerado.glob("*.c")):
                fh.write(f'#include "{f.name}"\n')
            fh.write('#include "ponte_jogo.c"\n')

        progresso(0.55, "Compiling the game")
        t0 = time.monotonic()
        destino_tmp = tmp / nome
        cmd = [tcc, "-shared", "-std=c99", "-w",
               "-DTINYTOON_JOGADOR", f'-DJOGO_ROM_SHA1="{sha1}"',
               f'-DJOGO_RECOMP_SHA256="{esperado}"',
               "-I" + str(gerado), "-I" + str(raiz / "runtime"),
               "-o", str(destino_tmp), str(unico)]
        if os.name != "nt":
            cmd.insert(1, "-fPIC")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not destino_tmp.is_file():
            print(r.stdout, r.stderr, sep="\n")
            return falhar(SAIDA_NAO_MONTADO, "the C compiler failed.")
        print(f"  compiled in {time.monotonic() - t0:.1f} s", flush=True)

        progresso(0.95, "Installing")
        saida.mkdir(parents=True, exist_ok=True)
        final = saida / nome
        # Trocar por rename: um jogo.dll pela metade, se o processo morrer no
        # meio da copia, carregaria e quebraria; o rename e' tudo ou nada.
        provisorio = saida / (nome + ".novo")
        shutil.copyfile(destino_tmp, provisorio)
        os.replace(provisorio, final)

    progresso(1.0, "Ready")
    print(f"montado: {final}")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--rom", required=True)
    p.add_argument("--saida", required=True, help="pasta do tinytoon.exe")
    p.add_argument("--tcc", help="caminho do TinyCC (padrao: ../tcc/ ou o PATH)")
    p.add_argument("--pausar-se-falhar", action="store_true",
                   help="espera Enter antes de fechar quando falha (janela de console)")
    args = p.parse_args(argv)
    try:
        codigo = montar(args)
    except Exception as exc:  # noqa: BLE001 — a janela precisa mostrar o motivo
        import traceback
        traceback.print_exc()
        codigo = falhar(SAIDA_NAO_MONTADO, f"unexpected error: {exc}")
    if codigo and args.pausar_se_falhar:
        with contextlib.suppress(EOFError):
            input("\nPress Enter to close this window.")
    return codigo


if __name__ == "__main__":
    raise SystemExit(main())
