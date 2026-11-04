"""Ciclos de relogio por instrucao do 68000.

Por que isto existe: ate' agora o runtime media o tempo em ACESSOS A MEMORIA, e
um quadro valia 46.000 deles — numero que eu estimei, nao medi. Todo o relogio
do port pendurava nesse palpite: V-blank, velocidade do Z80, temporizadores do
YM2612 e taxa de audio.

O problema nao e' o numero estar impreciso; e' a GRANDEZA estar errada. Uma
instrucao que gasta 20 ciclos e faz um acesso adiantava o relogio tanto quanto
uma de 4 ciclos que tambem faz um. Codigo que trabalha em registrador quase nao
tocava o relogio; codigo que varre memoria adiantava demais. O efeito observavel
e' QUANTO TRABALHO CABE NUM QUADRO — ou seja, se o jogo engasga nos mesmos
lugares que o original engasga.

Contar ciclos troca o palpite por medida.

PRECISAO: as tabelas abaixo sao as do manual do 68000 — tempo base da instrucao
mais o tempo de calculo do endereco efetivo. Nao modelam espera de barramento,
nem o prefetch, nem o custo variavel de MULU/DIVU (que dependem dos operandos e
aqui entram pela media). O erro que sobra e' de poucos por cento, contra uma
grandeza inteiramente errada antes.
"""

from __future__ import annotations

import re

# Tempo de CALCULO do endereco efetivo, somado ao tempo base da instrucao.
# Duas colunas porque operando longo custa um acesso de barramento a mais.
#            byte/word, long
EA = {
    "dn":      (0, 0),
    "an":      (0, 0),
    "(an)":    (4, 8),
    "(an)+":   (4, 8),
    "-(an)":   (6, 10),
    "d(an)":   (8, 12),
    "d(an,x)": (10, 14),
    "abs.w":   (8, 12),
    "abs.l":   (12, 16),
    "d(pc)":   (8, 12),
    "d(pc,x)": (10, 14),
    "imm":     (4, 8),
}


def modo(op: str) -> str:
    """Classifica o operando em modo de enderecamento."""
    op = op.strip()
    if re.fullmatch(r"d[0-7]", op):
        return "dn"
    if re.fullmatch(r"(a[0-7]|sp)", op):
        return "an"
    if op.startswith("#"):
        return "imm"
    if op.startswith("-("):
        return "-(an)"
    if op.endswith(")+"):
        return "(an)+"
    if "pc" in op:
        return "d(pc,x)" if op.count(",") >= 1 else "d(pc)"
    if op.startswith("("):
        return "(an)"
    if "(" in op:
        return "d(an,x)" if "," in op[op.index("("):] else "d(an)"
    # Absoluto: o emissor ja' distingue curto de longo pelo tamanho do texto.
    return "abs.w" if len(op.lstrip("$")) <= 4 else "abs.l"


def custo_ea(op: str, longo: bool) -> int:
    return EA[modo(op)][1 if longo else 0]


def _ops(insn) -> list[str]:
    return [o for o in (insn.op_str or "").split(",") if o.strip()]


# Instrucoes cujo tempo nao segue "base + EA". Valores do manual.
FIXO = {
    "nop": 4, "swap": 4, "ext": 4, "exg": 6, "unlk": 12, "link": 16,
    "rts": 16, "rte": 20, "rtr": 20, "trapv": 4, "illegal": 34,
    "reset": 132, "stop": 4, "trap": 34,
    # Media: o custo real depende dos operandos, e varia ~2x.
    "mulu": 70, "muls": 70, "divu": 140, "divs": 158,
}


def ciclos(insn) -> int:
    """Ciclos de relogio da instrucao. Nunca zero — parar o relogio trava tudo."""
    mn = insn.mnemonic
    longo = insn.suffix == "l"
    ops = _ops(insn)

    if mn in FIXO:
        return FIXO[mn]

    # --- controle de fluxo -------------------------------------------------
    if mn == "bra":
        return 10
    if mn == "bsr":
        return 18
    if mn.startswith("b") and len(mn) == 3:
        # Bcc: o tomado custa mais. Sem saber o resultado, a media honesta e' a
        # do tomado, porque desvio de laco e' tomado quase sempre.
        return 10
    if mn.startswith("db"):
        return 10
    if mn == "jmp":
        return 8 + (custo_ea(ops[0], False) if ops else 0)
    if mn == "jsr":
        return 16 + (custo_ea(ops[0], False) if ops else 0)

    # --- movimentacao ------------------------------------------------------
    if mn in ("move", "movea") and len(ops) == 2:
        return 4 + custo_ea(ops[0], longo) + custo_ea(ops[1], longo)
    if mn == "movem" and len(ops) == 2:
        # 12 de base mais 4 (word) ou 8 (long) por registrador transferido.
        regs = _contar_regs(ops[0] if "/" in ops[0] or "-" in ops[0] else ops[1])
        return 12 + regs * (8 if longo else 4)
    if mn == "movep":
        return 16 if longo else 24
    if mn == "moveq":
        return 4
    if mn == "lea":
        return 4 + custo_ea(ops[0], False)
    if mn == "pea":
        return 12 + custo_ea(ops[0], False)

    # --- deslocamento e rotacao -------------------------------------------
    if mn in ("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr"):
        if len(ops) == 1:                      # forma de memoria, 1 bit
            return 8 + custo_ea(ops[0], False)
        n = 1
        if ops[0].startswith("#"):
            try:
                n = int(ops[0].lstrip("#$"), 16)
            except ValueError:
                n = 1
        else:
            n = 8                              # conta em registrador: media
        return (8 if longo else 6) + 2 * n

    # --- aritmetica e logica ----------------------------------------------
    ARITH = {"add", "adda", "addi", "addq", "addx", "sub", "suba", "subi",
             "subq", "subx", "and", "andi", "or", "ori", "eor", "eori",
             "cmp", "cmpa", "cmpi", "cmpm", "abcd", "sbcd"}
    if mn in ARITH and len(ops) == 2:
        destino = modo(ops[1])
        if destino in ("dn", "an"):
            base = 6 if longo else 4
            # Operando longo vindo de registrador ou imediato custa 8.
            if longo and modo(ops[0]) in ("dn", "an", "imm"):
                base = 8
            return base + custo_ea(ops[0], longo)
        # Destino em memoria: le', opera e escreve.
        base = 12 if longo else 8
        return base + custo_ea(ops[0], longo) + custo_ea(ops[1], longo)

    # --- um operando -------------------------------------------------------
    UM = {"clr", "neg", "negx", "not", "tst", "nbcd", "tas", "scc", "sne",
          "seq", "shi", "sls", "scs", "spl", "smi", "svc", "svs", "sge",
          "slt", "sgt", "sle", "st", "sf"}
    if mn in UM and ops:
        base = 6 if longo else 4
        return base + custo_ea(ops[0], longo)
    if mn in ("btst", "bchg", "bclr", "bset") and len(ops) == 2:
        base = 6 if modo(ops[1]) == "dn" else 8
        return base + custo_ea(ops[0], False) + custo_ea(ops[1], False)
    if mn == "chk":
        return 10 + (custo_ea(ops[0], False) if ops else 0)

    # Desconhecida: quatro ciclos e' o minimo de qualquer instrucao do 68000.
    # Zero seria pior que impreciso — travaria o relogio.
    return 4


def _contar_regs(lista: str) -> int:
    """Quantos registradores numa lista tipo 'd0-d7/a0-a6'."""
    ordem = [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)]
    n = 0
    for parte in lista.strip().split("/"):
        parte = parte.strip()
        if "-" in parte:
            ini, fim = (p.strip() for p in parte.split("-", 1))
            if ini in ordem and fim in ordem:
                n += ordem.index(fim) - ordem.index(ini) + 1
        elif parte in ordem:
            n += 1
    return n or 1
