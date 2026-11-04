"""Recompilador estatico: traduz funcoes 68000 do trace para C.

Cada instrucao vira uma ou mais linhas de C sobre um ``M68K`` (ver
``runtime/m68k.h``). Acesso a memoria passa pelo barramento em
``runtime/memory.h`` — a logica recompilada nunca toca hardware direto.

**Instrucao nao suportada e' erro, nao aviso.** O tradutor levanta
``Unsupported`` e o modo ``--coverage`` transforma isso em roteiro: ele mede
que fracao das instrucoes tracadas ja' da' para traduzir e lista os
mnemonicos que faltam, em ordem de frequencia.

Uso:
    python tools/recompile.py --coverage        # o que falta, por frequencia
    python tools/recompile.py --function 0x3e6  # gera o C de uma funcao
"""

from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from m68k_ops import (CONDITIONS, DATA_REG as DATA_REG_RE,  # noqa: E402
                      Operand, SIZE_BITS, Unsupported, parse_register_list,
                      register_expr, register_store)
from ciclos import ciclos as ciclos_de
from romlib import Rom, hexa, load_stack_returns  # noqa: E402
from trace_code import Trace, load_dispatchers, load_jump_tables  # noqa: E402


def split_operands(op_str: str) -> list[str]:
    """Separa operandos pela virgula de topo, ignorando as de dentro de
    parenteses.

    Split ingenuo quebra ``$45ba(pc, d0.w),d2`` em tres pedacos — a virgula do
    modo indexado nao separa operandos.
    """
    partes, atual, nivel = [], [], 0
    for ch in op_str:
        if ch == "(":
            nivel += 1
        elif ch == ")":
            nivel -= 1
        if ch == "," and nivel == 0:
            partes.append("".join(atual).strip())
            atual = []
        else:
            atual.append(ch)
    if "".join(atual).strip():
        partes.append("".join(atual).strip())
    return partes


def label(addr: int) -> str:
    return f"L_{addr:06X}"


def func_name(trace: Trace, addr: int) -> str:
    """Nome C de uma funcao.

    Nome verificado a mao vale mesmo quando o endereco nao e' ponto de entrada:
    $00053C e' alvo de desvio, nao de chamada, mas se chama SetGameState.
    """
    if addr in trace.names:
        return trace.names[addr]
    return trace.label_for(addr) if addr in trace.entries else f"sub_{addr:06X}"


PREDEC_RE = re.compile(r"^-\(a([0-7])\)$")
POSTINC_RE = re.compile(r"^\(a([0-7])\)\+$")


def translate_movem(ops: list[str], bits: int) -> list[str]:
    """MOVEM: transferencia de multiplos registradores.

    Nao passa pela classe Operand de proposito. Em ``-(aN)`` normal o ponteiro
    anda uma vez; num MOVEM ele anda **uma vez por registrador**, e na ordem
    inversa — a7 primeiro, d0 por ultimo. Reusar Operand aqui geraria codigo
    silenciosamente errado.
    """
    passo = bits // 8
    fonte, destino = ops

    m = PREDEC_RE.match(destino)
    if m:
        reg = int(m.group(1))
        linhas = [f"{{ uint32_t ea = cpu->a[{reg}];"]
        # Ordem inversa: a7, a6, ..., a0, d7, ..., d0.
        for kind, number in reversed(parse_register_list(fonte)):
            linhas.append(f"  ea -= {passo};")
            linhas.append(f"  md_write{bits}(ea, {register_expr(kind, number, bits)});")
        linhas.append(f"  cpu->a[{reg}] = ea; }}")
        return linhas

    m = POSTINC_RE.match(fonte)
    if m:
        reg = int(m.group(1))
        linhas = [f"{{ uint32_t ea = cpu->a[{reg}];"]
        for kind, number in parse_register_list(destino):
            linhas.append(f"  {register_store(kind, number, bits, f'md_read{bits}(ea)')}")
            linhas.append(f"  ea += {passo};")
        linhas.append(f"  cpu->a[{reg}] = ea; }}")
        return linhas

    # Formas com endereco de controle: sem efeito colateral no ponteiro.
    try:
        registradores = parse_register_list(fonte)
        endereco = Operand(destino, "l", "ea_mv")
        escrita = True
    except Unsupported:
        registradores = parse_register_list(destino)
        endereco = Operand(fonte, "l", "ea_mv")
        escrita = False
    linhas = [*endereco.prepare, f"{{ uint32_t ea = {endereco.address()};"]
    for kind, number in registradores:
        if escrita:
            linhas.append(f"  md_write{bits}(ea, {register_expr(kind, number, bits)});")
        else:
            linhas.append(f"  {register_store(kind, number, bits, f'md_read{bits}(ea)')}")
        linhas.append(f"  ea += {passo};")
    linhas.append("}")
    return linhas


def translate_status(mn: str, ops: list[str], bits: int, addr: int) -> list[str]:
    """Instrucoes que tocam CCR, SR ou USP.

    Sao poucas e todas com forma fixa no jogo: mascarar interrupcao no
    V-blank, salvar e restaurar SR em volta de secao critica, e a inicializacao
    do USP no boot.
    """
    LOGIC = {"andi": "&", "ori": "|", "eori": "^"}
    destino = ops[-1]

    if destino == "usp":
        origem = Operand(ops[0], "l", f"ea_usp_{addr:06X}")
        return [*origem.prepare, f"cpu->usp = {origem.read()};"]
    if ops[0] == "usp":
        return [Operand(ops[1], "l", f"ea_usp_{addr:06X}").write("cpu->usp")]

    largura = 16 if destino == "sr" else 8
    getter = "get_sr(cpu)" if destino == "sr" else "get_ccr(cpu)"
    setter = "set_sr" if destino == "sr" else "set_ccr"

    if mn in LOGIC:
        origem = Operand(ops[0], "w" if largura == 16 else "b", f"ea_st_{addr:06X}")
        return [*origem.prepare,
                f"{setter}(cpu, (uint{largura}_t)({getter} {LOGIC[mn]} {origem.read()}));"]
    if mn == "move" and destino in ("sr", "ccr"):
        origem = Operand(ops[0], "w" if largura == 16 else "b", f"ea_st_{addr:06X}")
        return [*origem.prepare, f"{setter}(cpu, (uint{largura}_t){origem.read()});"]
    if mn == "move" and ops[0] in ("sr", "ccr"):
        alvo = Operand(ops[1], "w", f"ea_st_{addr:06X}")
        fonte = "get_sr(cpu)" if ops[0] == "sr" else "(uint16_t)get_ccr(cpu)"
        return [*alvo.prepare, alvo.write(fonte)]
    raise Unsupported(f"{mn} com {ops}")


def dispatch_tables(trace: Trace) -> dict[int, list[int]]:
    """site do salto indireto -> alvos catalogados.

    Junta o que veio de ``jump-tables.csv`` e o que o tracer reconheceu
    sozinho. E' a mesma informacao que fez a cobertura da varredura crescer;
    aqui ela vira ``switch``.
    """
    tabelas: dict[int, list[int]] = {}
    for t in trace.inline_tables:
        if t["targets"]:
            tabelas.setdefault(t["site"], []).extend(t["targets"])
    for t in trace.tables:
        if t["site"] not in tabelas:
            alvos = trace._table_targets(t) if t["kind"] not in ("offset16", "pointer") else []
            if t["kind"] == "fixed":
                alvos = [t["base"]]
            if alvos:
                tabelas[t["site"]] = alvos
    return {site: sorted(set(alvos)) for site, alvos in tabelas.items()}


def emit_switch(trace: Trace, site: int, seletor: str, casos: list[tuple[str, int]],
                retorna: bool) -> list[str]:
    """Gera o switch de um despacho. ``retorna`` diz se o site nao volta."""
    linhas = [f"switch ({seletor}) {{"]
    for rotulo, alvo in casos:
        acao = f"{func_name(trace, alvo)}(cpu);"
        linhas.append(f"  case {rotulo}: {acao} {'return;' if retorna else 'break;'}")
    linhas.append(f"  default: md_unresolved_dispatch(0x{site:X}u, "
                  f"(uint32_t)({seletor})); return;")
    linhas.append("}")
    return linhas


def translate_dispatch(insn, trace: Trace, ops: list[str],
                       tabelas: dict[int, list[int]]) -> list[str] | None:
    """Salto indireto com tabela conhecida vira switch sobre o endereco efetivo.

    Nao depende de reconhecer o idioma: seja um jmp indireto por registrador, um jsr indexado ou
    ``jmp BASE(pc,d0.w)``, o operando ja' da' a expressao do endereco de
    destino. Os casos sao os alvos que a analise catalogou; qualquer outro
    valor cai no ``default`` e aborta, porque seguir significaria executar o
    alvo errado.
    """
    alvos = tabelas.get(insn.address)
    if not alvos:
        return None
    operando = Operand(ops[0], "l", f"ea_disp_{insn.address:06X}")
    retorna = insn.mnemonic == "jmp"
    casos = [(f"0x{a:X}u", a) for a in alvos]
    linhas = [*operando.prepare,
              f"{{ uint32_t alvo = {operando.address()};",
              *[f"  {l}" for l in emit_switch(trace, insn.address, "alvo", casos, retorna)],
              "}"]
    if not retorna:
        linhas.append(CHECA_UNWIND)
    return linhas


def translate_substate(insn, trace: Trace, tabelas: dict[int, list[int]]) -> list[str] | None:
    """``jsr SubStateDispatch`` com tabela inline vira switch por indice.

    Esse e' o caso que quebra a traducao ingenua de ``jsr`` para chamada C: o
    despachante em $0005CE copia o topo da pilha para a0 e o retira, ou seja, **desempilha o
    endereco de retorno** para usa-lo como base da tabela. Traduzido como
    chamada, o C voltaria para a instrucao seguinte — que na verdade sao os
    bytes da tabela, nao codigo.

    O indice util e' ``d0 & $7F``: o despachante dobra d0 e o
    mascara com $FF, entao o offset de word e' par e o indice de entrada e'
    a metade disso.
    """
    if insn.target not in trace.dispatchers:
        return None
    alvos = tabelas.get(insn.address)
    if not alvos:
        return None
    entradas = next((t["targets"] for t in trace.inline_tables
                     if t["site"] == insn.address), None)
    if not entradas:
        return None
    casos = [(str(i), alvo) for i, alvo in enumerate(entradas)]
    return emit_switch(trace, insn.address, "d16(cpu, 0) & 0x7F", casos, True)


# Sites que desempilham o proprio endereco de retorno; ver
# analysis/stack-returns.csv e o campo unwind em runtime/m68k.h.
STACK_RETURNS = load_stack_returns()

# Depois de toda chamada: se a rotina chamada pediu para pular um nivel de
# retorno, o chamador tambem retorna. E' o que reproduz o "rts que volta dois
# niveis" num modelo onde jsr virou chamada de funcao C.
CHECA_UNWIND = "if (cpu->unwind) { cpu->unwind--; return; }"


def translate(insn, trace: Trace, scope: range | None = None,
              tabelas: dict[int, list[int]] | None = None) -> list[str]:
    """Traduz uma instrucao para statements C.

    ``scope`` e' a faixa de enderecos da funcao sendo gerada: desvio para
    dentro dela vira ``goto``; desvio para fora vira chamada seguida de
    ``return``, ou seja, uma tail call.
    """
    mn = insn.mnemonic
    size = insn.suffix or "w"
    if size not in SIZE_BITS:
        raise Unsupported(f"tamanho {size!r}")
    bits = SIZE_BITS[size]
    ops = split_operands(insn.op_str)

    def operand(text: str, sz: str, tag: str) -> Operand:
        # O nome do temporario leva o endereco da instrucao: os modos com
        # efeito colateral declaram uma variavel no escopo da funcao, e duas
        # instrucoes com (aN)+ colidiriam se o nome fosse fixo.
        return Operand(text, sz, f"ea_{tag}_{insn.address:06X}")

    def emit(o: Operand, *lines: str) -> list[str]:
        return [*o.prepare, *lines]

    # --- fluxo -----------------------------------------------------------
    if mn == "rts":
        return ["return;"]
    if mn == "nop":
        return [";"]

    if mn in CONDITIONS and insn.target is not None:
        destino = (f"goto {label(insn.target)};" if scope and insn.target in scope
                   else f"{{ {func_name(trace, insn.target)}(cpu); return; }}")
        if mn == "bra":
            if insn.target == insn.address:
                # "bra *": o laco ocioso da ROM, onde o jogo espera o V-blank.
                # Vira o laco de frames do port.
                return ["md_idle();", destino]
            return [destino]
        return [f"if ({CONDITIONS[mn]}) {destino}"]

    if mn.startswith("db") and insn.target is not None:
        # DBcc: sai do laco se a condicao for verdadeira; senao decrementa o
        # contador de 16 bits e desvia enquanto ele nao passar de -1.
        cond = CONDITIONS.get("b" + mn[2:], None)
        contador = Operand(ops[0], "w", f"ea_db_{insn.address:06X}")
        salto = (f"goto {label(insn.target)};" if scope and insn.target in scope
                 else f"{{ {func_name(trace, insn.target)}(cpu); return; }}")
        guarda = f"if (!({cond})) " if cond and cond not in ("1", "0") else ""
        return [f"{guarda}{{ uint16_t c = {contador.read()};",
                f"  {contador.write('(uint16_t)(c - 1)')}",
                f"  if (c != 0) {salto} }}"]

    tabelas = tabelas or {}

    if mn in ("bsr", "jsr") and insn.target is not None:
        # SubStateDispatch tem de vir antes: o jsr existe, mas nao retorna.
        substate = translate_substate(insn, trace, tabelas)
        if substate is not None:
            return substate
        return [f"{func_name(trace, insn.target)}(cpu);", CHECA_UNWIND]
    if mn == "jmp" and insn.target is not None:
        return [f"{{ {func_name(trace, insn.target)}(cpu); return; }}"]

    if mn in ("jmp", "jsr") and insn.target is None:
        despacho = translate_dispatch(insn, trace, ops, tabelas)
        if despacho is not None:
            return despacho

    if mn == "rte":
        # O jogo so' usa rte nos handlers de interrupcao, que aqui viram
        # funcoes normais: quem chama e' o laco do runtime, nao a CPU.
        return ["return;"]

    # --- registrador de status -------------------------------------------
    ESPECIAIS = {"ccr", "sr", "usp"}
    if ops and any(o in ESPECIAIS for o in ops):
        return translate_status(mn, ops, bits, insn.address)

    # --- movimentacao ----------------------------------------------------
    if mn in ("move", "movea") and len(ops) == 2 and insn.address in STACK_RETURNS:
        # Desempilha o proprio endereco de retorno para voltar dois niveis.
        #
        # O pop NAO pode ser traduzido: no 68000 ele remove um endereco de
        # retorno que o bsr empilhou, mas aqui bsr virou chamada de funcao C e
        # esse endereco nunca existiu na pilha emulada. Traduzir o pop somava 4
        # bytes a a7 a cada passagem — no handler de V-blank isso e' +4 por
        # frame, e depois de ~570 frames a7 saia da RAM e entrava na ROM, onde
        # escrita e' ignorada. Sintoma: movem empilhava no vazio e desempilhava
        # lixo, muito longe daqui.
        #
        # O que resta e' o efeito observavel: o registrador destino fica com o
        # endereco de retorno (valor que o jogo descarta) e a marca faz o
        # chamador retornar junto — ver analysis/stack-returns.csv.
        dst = operand(ops[1], size, "d")
        return [*dst.prepare,
                f"{dst.write('0')}   /* valor descartado pelo jogo */",
                "cpu->unwind = 1;"]

    if mn in ("move", "movea") and len(ops) == 2:
        src = operand(ops[0], size, "s")
        dst = operand(ops[1], size, "d")
        linhas = [*src.prepare, *dst.prepare, f"{{ uint{bits}_t v = {src.read()};",
                  f"  {dst.write('v')}"]
        linhas.append(f"  flags_logic{bits}(cpu, v); }}" if mn == "move" else "}")
        return linhas

    if mn == "moveq" and len(ops) == 2:
        # MOVEQ e' sempre long, com o imediato de 8 bits estendido com sinal —
        # nao existe forma .b nem .w, entao o sufixo do capstone e' ignorado.
        src = operand(ops[0], "b", "s")
        dst = operand(ops[1], "l", "d")
        valor = f"(uint32_t)(int32_t)(int8_t){src.read()}"
        return [f"{{ uint32_t v = {valor};", f"  {dst.write('v')}",
                "  flags_logic32(cpu, v); }"]

    if mn == "movem" and len(ops) == 2:
        return translate_movem(ops, bits)

    if mn == "lea" and len(ops) == 2:
        src = operand(ops[0], "l", "s")
        dst = operand(ops[1], "l", "d")
        return emit(src, dst.write(src.address()))

    if mn == "clr" and len(ops) == 1:
        dst = operand(ops[0], size, "d")
        return emit(dst, dst.write("0"),
                    "cpu->n = 0; cpu->z = 1; cpu->v = 0; cpu->c = 0;")

    if mn == "tst" and len(ops) == 1:
        src = operand(ops[0], size, "s")
        return emit(src, f"flags_logic{bits}(cpu, {src.read()});")

    if mn == "swap" and len(ops) == 1:
        dst = operand(ops[0], "l", "d")
        return [dst.write(f"op_swap(cpu, {dst.read()})")]

    if mn == "ext" and len(ops) == 1:
        dst = operand(ops[0], size, "d")
        origem = "8" if bits == 16 else "16"
        return [f"{dst.write(f'(uint{bits}_t)(int{bits}_t)(int{origem}_t){dst.read()}')}",
                f"flags_logic{bits}(cpu, {dst.read()});"]

    # --- aritmetica e logica ---------------------------------------------
    ARITH = {"add": "add", "addi": "add", "addq": "add", "adda": "add",
             "sub": "sub", "subi": "sub", "subq": "sub", "suba": "sub"}
    if mn in ARITH and len(ops) == 2:
        src = operand(ops[0], size, "s")
        dst = operand(ops[1], size, "d")
        # Destino em registrador de endereco muda a instrucao inteira: a
        # operacao e' sempre de 32 bits e NAO afeta flags, mesmo escrita como
        # um addq.w de 1 em a1. Nao e' caso especial de ADDA/SUBA — e' a regra do
        # 68000 para qualquer destino An.
        #
        # O teste diferencial pegou os dois erros que isso causava: somar 1
        # a a1 com addq.w dava $FFFF8973 em vez de $00FF8973, porque eu somava em 16 bits
        # e estendia o sinal na escrita; e somar 4 com addq.l zerava o CCR que
        # deveria ficar intacto.
        destino_endereco = re.fullmatch(r"(a[0-7]|sp)", ops[1].strip())
        if mn in ("adda", "suba") or destino_endereco:
            sinal = "+" if ARITH[mn] == "add" else "-"
            n_reg = "7" if ops[1].strip() == "sp" else ops[1].strip()[1]
            return [*src.prepare, *dst.prepare,
                    dst.write(f"(cpu->a[{n_reg}] {sinal} "
                              f"(uint32_t)(int32_t)(int{bits}_t){src.read()})")
                    .replace("set_a16", "set_a32").replace("set_a8", "set_a32")]
        return [*src.prepare, *dst.prepare,
                dst.write(f"flags_{ARITH[mn]}{bits}(cpu, {dst.read()}, {src.read()})")]

    LOGIC = {"and": "&", "andi": "&", "or": "|", "ori": "|", "eor": "^", "eori": "^"}
    if mn in LOGIC and len(ops) == 2:
        src = operand(ops[0], size, "s")
        dst = operand(ops[1], size, "d")
        return [*src.prepare, *dst.prepare,
                f"{{ uint{bits}_t v = (uint{bits}_t)({dst.read()} {LOGIC[mn]} {src.read()});",
                f"  {dst.write('v')}",
                f"  flags_logic{bits}(cpu, v); }}"]

    if mn == "cmpa" and len(ops) == 2:
        # CMPA compara em 32 bits, mas le' a origem no TAMANHO DA INSTRUCAO e
        # a estende com sinal — a mesma regra de ADDA/SUBA/MOVEA.
        #
        # Ler a origem como long era o bug: um cmpa.w de $AA00 contra a0 virava
        # comparacao contra $0000AA00 em vez de $FFFFAA00, e como todo endereco
        # de RAM do Mega Drive tem o topo em $FFFF, o teste dava sempre o
        # resultado oposto. Na tela de selecao isso punha as duas rodas de
        # personagem no mesmo canto: o codigo em $01CB1E escolhe o centro
        # comparando o ponteiro do objeto com $FFAA00, e a comparacao errada
        # mandava todos para o mesmo lado.
        src = operand(ops[0], size, "s")
        dst = operand(ops[1], "l", "d")
        return [*src.prepare, *dst.prepare,
                f"flags_cmp32(cpu, {dst.read()}, "
                f"(uint32_t)(int32_t)(int{bits}_t){src.read()});"]

    if mn in ("cmp", "cmpi") and len(ops) == 2:
        src = operand(ops[0], size, "s")
        dst = operand(ops[1], size, "d")
        return [*src.prepare, *dst.prepare,
                f"flags_cmp{bits}(cpu, {dst.read()}, {src.read()});"]

    BCD = {"abcd": "op_abcd", "sbcd": "op_sbcd", "subx": "op_subx16"}
    if mn in BCD and len(ops) == 2:
        src = operand(ops[0], "b" if mn != "subx" else size, "s")
        dst = operand(ops[1], "b" if mn != "subx" else size, "d")
        return [*src.prepare, *dst.prepare,
                dst.write(f"{BCD[mn]}(cpu, {dst.read()}, {src.read()})")]

    if mn in ("neg",) and len(ops) == 1 and bits in (8, 16, 32):
        dst = operand(ops[0], size, "d")
        return emit(dst, dst.write(f"op_neg{bits}(cpu, {dst.read()})"))

    if mn == "not" and len(ops) == 1:
        dst = operand(ops[0], size, "d")
        return [*dst.prepare, f"{{ uint{bits}_t v = (uint{bits}_t)~{dst.read()};",
                f"  {dst.write('v')}", f"  flags_logic{bits}(cpu, v); }}"]

    # ASL e LSL sao a mesma coisa nos bits; a diferenca esta' so' na flag V,
    # que o jogo nao testa depois de deslocar. ROXL/ROXR ainda nao aparecem.
    # ASL tem helper proprio: o padrao de bits e' igual ao do LSL, mas o flag de
    # overflow nao. Mapear um no outro custou V errado em toda instrucao asl da
    # ROM — ver op_asl em runtime/m68k.h.
    SHIFT = {"lsl": "lsl", "lsr": "lsr", "asl": "asl", "asr": "asr",
             "rol": "rol", "ror": "ror"}
    if mn in SHIFT and len(ops) in (1, 2):
        if len(ops) == 1:
            # Forma de um operando: desloca a memoria em 1 bit, sempre word.
            dst = operand(ops[0], "w", "d")
            return [*dst.prepare, dst.write(f"op_{SHIFT[mn]}16(cpu, {dst.read()}, 1)")]
        conta = operand(ops[0], "w", "s")
        dst = operand(ops[1], size, "d")
        n = conta.read() if conta.is_immediate else f"(d32(cpu, {ops[0][1]}) & 63)"
        return [*dst.prepare, dst.write(f"op_{SHIFT[mn]}{bits}(cpu, {dst.read()}, {n})")]

    if mn == "exg" and len(ops) == 2:
        a, b = (operand(o, "l", t) for o, t in zip(ops, "sd"))
        return [f"{{ uint32_t t = {a.read()};", f"  {a.write(b.read())}",
                f"  {b.write('t')} }}"]

    if mn == "muls" and len(ops) == 2:
        src = operand(ops[0], "w", "s")
        dst = operand(ops[1], "l", "d")
        return emit(src, dst.write(f"op_muls(cpu, {src.read()}, (uint16_t){dst.read()})"))

    if mn == "divs" and len(ops) == 2:
        src = operand(ops[0], "w", "s")
        dst = operand(ops[1], "l", "d")
        return emit(src, dst.write(
            f"op_divs(cpu, {dst.read()}, {src.read()}, &trap_div0)"))

    # Scc: escreve $FF ou $00 num byte conforme a condicao.
    if mn == "st" or (len(mn) == 3 and mn[0] == "s" and f"b{mn[1:]}" in CONDITIONS):
        if len(ops) != 1:
            raise Unsupported(f"{mn} com {ops}")
        dst = operand(ops[0], "b", "d")
        cond = "1" if mn == "st" else CONDITIONS[f"b{mn[1:]}"]
        return [*dst.prepare, dst.write(f"({cond}) ? 0xFFu : 0x00u")]

    if mn == "mulu" and len(ops) == 2:
        src = operand(ops[0], "w", "s")
        dst = operand(ops[1], "l", "d")
        return emit(src, dst.write(f"op_mulu(cpu, {src.read()}, (uint16_t){dst.read()})"))

    if mn == "divu" and len(ops) == 2:
        src = operand(ops[0], "w", "s")
        dst = operand(ops[1], "l", "d")
        return emit(src, dst.write(
            f"op_divu(cpu, {dst.read()}, {src.read()}, &trap_div0)"))

    # --- bits -------------------------------------------------------------
    # Operadores binarios, nao atribuicao composta: o destino pode ser uma
    # chamada (md_read8(...)), que nao e' lvalue.
    BIT = {"btst": None, "bset": "|", "bclr": "& ~", "bchg": "^"}
    if mn in BIT and len(ops) == 2:
        dst = operand(ops[1], "b" if not DATA_REG_RE.match(ops[1]) else "l", "d")
        src = operand(ops[0], "b", "s")
        largura = dst.bits
        n = f"({src.read()} % {largura})"
        linhas = [*src.prepare, *dst.prepare,
                  f"cpu->z = (({dst.read()} >> {n}) & 1) == 0;"]
        if BIT[mn]:
            linhas.append(dst.write(
                f"(uint{largura}_t)({dst.read()} {BIT[mn]} (1u << {n}))"))
        return linhas

    raise Unsupported(mn)


def function_body(trace: Trace, start: int) -> list:
    """Instrucoes da funcao: contiguas a partir de ``start`` ate' o proximo
    ponto de entrada, no mesmo criterio de ``Trace.functions()``."""
    entradas = sorted(trace.entries)
    seguinte = next((a for a in entradas if a > start), trace.rom.size)
    corpo = []
    addr = start
    while addr in trace.insns and addr < seguinte:
        corpo.append(trace.insns[addr])
        addr = trace.insns[addr].end
    return corpo


def referenced_functions(trace: Trace, start: int) -> set[int]:
    """Enderecos que a funcao chama ou para os quais desvia por fora do corpo."""
    corpo = function_body(trace, start)
    if not corpo:
        return set()
    scope = range(start, corpo[-1].end)
    tabelas = dispatch_tables(trace)
    fora = {i.target for i in corpo
            if i.target is not None and i.target not in scope
            and i.target not in trace.dispatchers}
    for insn in corpo:
        fora.update(a for a in tabelas.get(insn.address, []) if a not in scope)
    return {a for a in fora if a is not None}


def emit_function(trace: Trace, start: int, name: str) -> str:
    tabelas = dispatch_tables(trace)
    corpo = function_body(trace, start)
    if not corpo:
        raise Unsupported(f"{hexa(start)} nao tem corpo tracado")
    scope = range(start, corpo[-1].end)

    # Label so' onde alguem desvia. Marcar toda instrucao deixaria o C
    # ilegivel sem ganhar nada.
    alvos = {i.target for i in corpo if i.target is not None and i.target in scope}
    chamadas = {i.target for i in corpo
                if i.target is not None and i.target not in scope
                and i.target not in trace.dispatchers}
    if not corpo[-1].stops_flow and corpo[-1].end in trace.insns:
        chamadas.add(corpo[-1].end)
    for insn in corpo:
        for alvo in tabelas.get(insn.address, []):
            if alvo not in scope:
                chamadas.add(alvo)
    chamadas = sorted(chamadas)

    linhas = ["/* Gerado por tools/recompile.py. Nao editar a mao. */",
              '#include "m68k.h"', '#include "memory.h"', ""]
    for alvo in chamadas:
        linhas.append(f"void {func_name(trace, alvo)}(M68K *cpu);")
    linhas += ["",
               f"/* {name} - recompilado de {hexa(start)}, {len(corpo)} instrucoes */",
               f"void {name}(M68K *cpu) {{",
               # Quem esta' executando, para o mapa de leitura da ROM saber a
               # QUEM atribuir cada byte lido. O cleanup restaura no retorno —
               # inclusive nos retornos no meio da funcao, que sao a maioria.
               f"    MD_FUNCAO(0x{start:06X});",
               "    int trap_div0 = 0; (void)trap_div0;"]
    for insn in corpo:
        if insn.address in alvos:
            linhas.append(f"{label(insn.address)}:")
        linhas.append(f"    /* {hexa(insn.address)}  {insn.text} */")
        # O relogio do runtime anda em CICLOS DO 68000, nao em acessos a
        # memoria. Ver tools/ciclos.py para a razao e a precisao.
        linhas.append(f"    cpu->ciclos += {ciclos_de(insn)};")
        for linha in translate(insn, trace, scope, tabelas):
            linhas.append(f"    {linha}")

    # Queda para a funcao seguinte.
    #
    # 120 funcoes terminam sem rts/bra/jmp: no original a execucao continua no
    # proximo endereco, que por acaso e' o inicio de outra funcao. Sem isto o C
    # retornaria ali e rotinas inteiras sumiriam — LzDecompressB virava uma
    # instrucao so'.
    if not corpo[-1].stops_flow and corpo[-1].end in trace.insns:
        seguinte = corpo[-1].end
        linhas.append(f"    /* cai em {hexa(seguinte)} */")
        linhas.append(f"    {func_name(trace, seguinte)}(cpu);")
    linhas.append("}")
    return "\n".join(linhas) + "\n"


def coverage(trace: Trace) -> None:
    faltando: collections.Counter = collections.Counter()
    tabelas = dispatch_tables(trace)
    ok = 0
    for insn in trace.insns.values():
        try:
            translate(insn, trace, None, tabelas)
            ok += 1
        except Unsupported:
            faltando[insn.mnemonic] += 1
    total = len(trace.insns)
    print(f"instrucoes tracadas : {total}")
    print(f"ja' traduziveis     : {ok}  ({100 * ok / total:.1f}%)")
    print(f"mnemonicos faltando : {len(faltando)}\n")
    print("os 25 que mais aparecem:")
    acumulado = ok
    for mnemonic, count in faltando.most_common(25):
        acumulado += count
        print(f"  {mnemonic:<10} {count:6d}   se implementado, chega a {100 * acumulado / total:5.1f}%")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Recompila 68000 para C")
    parser.add_argument("--rom")
    parser.add_argument("--coverage", action="store_true")
    parser.add_argument("--function", help="endereco da funcao a gerar (0x...)")
    parser.add_argument("-o", "--output")
    parser.add_argument("--all", metavar="DIR",
                        help="gera um .c por funcao traduzivel neste diretorio")
    args = parser.parse_args(argv)

    rom = Rom.load(args.rom)
    trace = Trace(rom, load_jump_tables(), load_dispatchers())
    trace.run()

    if args.coverage:
        coverage(trace)
        return 0

    if args.all:
        destino = Path(args.all)
        destino.mkdir(parents=True, exist_ok=True)
        for antigo in destino.glob("*.c"):
            antigo.unlink()
        gerados = falhas = 0
        motivos: collections.Counter = collections.Counter()
        # Fecho transitivo: um desvio para fora da funcao vira chamada, e o
        # alvo pode nao ser ponto de entrada do trace. Gerar so' as entradas
        # deixa simbolo indefinido no link, entao a fila cresce ate' fechar.
        fila = sorted(a for a in trace.entries if a in trace.insns)
        vistos = set(fila)
        while fila:
            addr = fila.pop(0)
            nome = func_name(trace, addr)
            try:
                fonte = emit_function(trace, addr, nome)
            except Unsupported as exc:
                falhas += 1
                motivos[str(exc)[:40]] += 1
                stub = [
                    "/* Gerado por tools/recompile.py: nao traduzida. */",
                    '#include "m68k.h"', '#include "memory.h"', "",
                    f"void {nome}(M68K *cpu) {{",
                    "    (void)cpu;",
                    f'    md_untranslated(0x{addr:X}u, "{str(exc)[:60]}");',
                    "}", ""]
                (destino / f"{nome}.c").write_text("\n".join(stub), encoding="ascii")
                continue
            (destino / f"{nome}.c").write_text(fonte, encoding="ascii")
            gerados += 1
            for chamado in referenced_functions(trace, addr):
                if chamado not in vistos and chamado in trace.insns:
                    vistos.add(chamado)
                    fila.append(chamado)
        total = gerados + falhas
        print(f"{gerados} de {total} funcoes geradas ({100 * gerados / total:.1f}%)")
        print(f"{falhas} com instrucao ainda nao suportada; motivos mais comuns:")
        for motivo, n in motivos.most_common(6):
            print(f"  {n:4d}  {motivo}")
        return 0
    if not args.function:
        parser.error("use --coverage ou --function")

    addr = int(args.function, 0)
    if addr not in trace.insns:
        parser.error(f"{hexa(addr)} nao e' inicio de instrucao no trace")
    name = trace.label_for(addr) if addr in trace.entries else f"sub_{addr:06X}"
    source = emit_function(trace, addr, name)
    if args.output:
        Path(args.output).write_text(source, encoding="ascii")
        print(f"gravado em {args.output}")
    else:
        print(source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
