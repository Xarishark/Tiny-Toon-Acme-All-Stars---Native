"""Camada fina sobre o capstone para disassembly de 68000.

Existe por tres motivos:

1. **Bug de buffer truncado.** Se o capstone recebe um buffer que termina no
   meio de uma instrucao, ele *inventa* operandos em vez de parar (visto na
   pratica: um move.l com o imediato $53454741 decodificado com o imediato $AAAAAAAA).
   Aqui todo decode usa uma janela com folga garantida.
2. **Normalizar mnemonicos.** O capstone devolve ``bne.b`` / ``move.l``; a
   analise de fluxo precisa da base (``bne``) e do tamanho separados.
3. **Classificar fluxo e referencias** de forma unica para todas as ferramentas.

Requer ``capstone`` (``pip install capstone``).
"""

from __future__ import annotations

from dataclasses import dataclass, field

try:
    from capstone import CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_000, Cs
    from capstone import m68k as cs_m68k
except ImportError as exc:  # pragma: no cover - mensagem de setup
    raise SystemExit(
        "capstone nao encontrado. Instale com:  python -m pip install capstone"
    ) from exc

# Maior instrucao possivel no 68000 e' 10 bytes (move.l #imm32, ($xxxxxxxx).l).
# A janela usa folga bem acima disso para nunca disparar o bug de truncamento.
MAX_INSN_BYTES = 10
WINDOW = 32

# --- classificacao de fluxo -------------------------------------------------

RETURN_MNEMONICS = {"rts", "rte", "rtr", "rtd"}
HALT_MNEMONICS = {"illegal", "stop"}
CONDITIONAL_BRANCHES = {
    "bhi", "bls", "bcc", "bcs", "bne", "beq", "bvc", "bvs",
    "bpl", "bmi", "bge", "blt", "bgt", "ble",
}
DBCC_PREFIX = "db"          # dbra, dbf, dbeq, ...
ABSOLUTE_MODES = {cs_m68k.M68K_AM_ABSOLUTE_DATA_SHORT, cs_m68k.M68K_AM_ABSOLUTE_DATA_LONG}
PC_RELATIVE_MODES = {cs_m68k.M68K_AM_PCI_DISP, cs_m68k.M68K_AM_PCI_INDEX_8_BIT_DISP}

# Instrucoes cujo ultimo operando e' escrito (destino).
WRITES_LAST_OPERAND = {
    "move", "movea", "moveq", "movem", "lea", "add", "adda", "addi", "addq",
    "sub", "suba", "subi", "subq", "and", "andi", "or", "ori", "eor", "eori",
    "mulu", "muls", "divu", "divs", "asl", "asr", "lsl", "lsr", "rol", "ror",
    "roxl", "roxr", "bset", "bclr", "bchg", "abcd", "sbcd", "exg", "swap",
}
# Instrucoes de um operando que escrevem nele.
WRITES_SINGLE_OPERAND = {"clr", "neg", "negx", "not", "ext", "swap", "nbcd", "tas"}
# Le e escreve o mesmo operando.
READ_MODIFY_WRITE = {
    "add", "addi", "addq", "sub", "subi", "subq", "and", "andi", "or", "ori",
    "eor", "eori", "asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr",
    "bset", "bclr", "bchg", "neg", "negx", "not", "tas",
}


@dataclass(frozen=True)
class Ref:
    """Referencia a um endereco feita por um operando."""
    target: int
    kind: str        # "abs" | "pcrel"
    access: str      # "r" | "w" | "rw"


@dataclass
class Insn:
    address: int
    size: int
    mnemonic: str            # base, sem sufixo de tamanho: "move", "bne"
    suffix: str              # "b" | "w" | "l" | ""
    op_str: str
    flow: str                # ver FLOW_KINDS
    target: int | None = None       # destino resolvido de branch/call/jmp
    refs: list[Ref] = field(default_factory=list)
    # Preenchidos quando algum operando usa d(pc, Xn) — a assinatura de acesso
    # a tabela indexada. Ver Trace._detect_pc_table em trace_code.py.
    pc_index_base: int | None = None   # endereco base da tabela
    pc_index_reg: int | None = None    # registrador de indice (id do capstone)
    dest_reg: int | None = None        # ultimo operando, quando e' registrador
    indirect_reg: int | None = None    # registrador de "jmp (aN)" / "jsr (aN)"

    @property
    def end(self) -> int:
        return self.address + self.size

    @property
    def text(self) -> str:
        base = self.mnemonic + (f".{self.suffix}" if self.suffix else "")
        return f"{base} {self.op_str}".strip()

    @property
    def stops_flow(self) -> bool:
        """True se a execucao nao continua na instrucao seguinte."""
        return self.flow in ("return", "branch", "indirect_jump", "halt")


FLOW_KINDS = (
    "normal",          # segue para a proxima instrucao
    "cond_branch",     # bcc/dbcc: desvia OU cai na proxima
    "branch",          # bra/jmp absoluto: so desvia
    "call",            # bsr/jsr absoluto: chama e volta
    "indirect_call",   # jsr (An) etc: destino desconhecido, retorna
    "indirect_jump",   # jmp (An) etc: destino desconhecido, nao retorna
    "return",          # rts/rte/rtr
    "halt",            # illegal/stop
)


class Disassembler:
    """Decodifica instrucoes 68000 a partir de um buffer de ROM."""

    def __init__(self, data: bytes):
        self.data = data
        self._cs = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_000)
        self._cs.detail = True
        self._cache: dict[int, Insn | None] = {}

    def decode(self, addr: int) -> Insn | None:
        """Decodifica uma instrucao em ``addr``; None se invalida ou fora da ROM."""
        if addr in self._cache:
            return self._cache[addr]
        insn = self._decode_uncached(addr)
        self._cache[addr] = insn
        return insn

    def _decode_uncached(self, addr: int) -> Insn | None:
        if addr < 0 or addr + 2 > len(self.data) or addr & 1:
            return None  # 68000 exige instrucoes alinhadas em word
        chunk = self.data[addr:addr + WINDOW]
        if len(chunk) < WINDOW:
            chunk = chunk + b"\x00" * (WINDOW - len(chunk))
        try:
            raw = next(self._cs.disasm(chunk, addr, count=1), None)
        except Exception:
            return None
        if raw is None:
            return None
        if addr + raw.size > len(self.data):
            return None  # instrucao passaria do fim da ROM: nao e' codigo valido
        if raw.id == 0:
            # capstone emitiu um pseudo-item de dados (CS_ERR_SKIPDATA): os
            # bytes nao formam uma instrucao valida de 68000.
            return None

        mnemonic, _, suffix = raw.mnemonic.partition(".")
        insn = Insn(
            address=addr,
            size=raw.size,
            mnemonic=mnemonic,
            suffix=suffix,
            op_str=raw.op_str,
            flow="normal",
        )
        self._classify(insn, raw)
        self._collect_refs(insn, raw)
        self._collect_indexing(insn, raw)
        return insn

    # -- internals ---------------------------------------------------------

    def _classify(self, insn: Insn, raw) -> None:
        m = insn.mnemonic
        if m in RETURN_MNEMONICS:
            insn.flow = "return"
            return
        if m in HALT_MNEMONICS:
            insn.flow = "halt"
            return

        if m in ("bra", "bsr") or m in CONDITIONAL_BRANCHES or m.startswith(DBCC_PREFIX):
            insn.target = self._branch_target(insn, raw)
            if m == "bsr":
                insn.flow = "call"
            elif m == "bra":
                insn.flow = "branch"
            else:
                insn.flow = "cond_branch"
            if insn.target is None:
                # displacement nao resolvido: trata como fim de fluxo conhecido
                insn.flow = "indirect_jump" if m == "bra" else "normal"
            return

        if m in ("jmp", "jsr"):
            target = self._absolute_target(raw)
            insn.target = target
            if target is not None:
                insn.flow = "branch" if m == "jmp" else "call"
            else:
                insn.flow = "indirect_jump" if m == "jmp" else "indirect_call"
            return

        insn.flow = "normal"

    def _branch_target(self, insn: Insn, raw) -> int | None:
        for op in raw.operands:
            if op.type == cs_m68k.M68K_OP_BR_DISP:
                # Deslocamento e' relativo ao endereco da instrucao + 2.
                return insn.address + 2 + op.br_disp.disp
        return None

    def _absolute_target(self, raw) -> int | None:
        """Endereco absoluto do (unico) operando de jmp/jsr, se houver."""
        for op in raw.operands:
            if op.address_mode in ABSOLUTE_MODES:
                return _sign_extend_short(op.imm, op.address_mode)
            if op.address_mode == cs_m68k.M68K_AM_PCI_DISP:
                return raw.address + 2 + op.mem.disp
        return None

    def _collect_refs(self, insn: Insn, raw) -> None:
        ops = raw.operands
        if not ops:
            return
        last = len(ops) - 1
        for i, op in enumerate(ops):
            if op.address_mode in ABSOLUTE_MODES:
                target = _sign_extend_short(op.imm, op.address_mode)
                kind = "abs"
            elif op.address_mode in PC_RELATIVE_MODES:
                target = raw.address + 2 + op.mem.disp
                kind = "pcrel"
            else:
                continue
            insn.refs.append(Ref(target, kind, self._access(insn.mnemonic, i, last, len(ops))))

    @staticmethod
    def _collect_indexing(insn: Insn, raw) -> None:
        """Extrai a assinatura ``d(pc, Xn)`` e o registrador de destino."""
        ops = raw.operands
        if not ops:
            return
        for op in ops:
            if op.address_mode == cs_m68k.M68K_AM_PCI_INDEX_8_BIT_DISP:
                # Deslocamento e' relativo ao endereco da instrucao + 2.
                insn.pc_index_base = raw.address + 2 + op.mem.disp
                insn.pc_index_reg = op.mem.index_reg
                break
            if op.address_mode == cs_m68k.M68K_AM_REGI_ADDR and insn.mnemonic in ("jmp", "jsr"):
                # Nesse modo o capstone nao preenche mem.base_reg; o id do
                # registrador vem em op.imm (verificado: um jmp indireto por a6 traz 15 em imm).
                insn.indirect_reg = op.imm
                break
        last = ops[-1]
        if last.type == cs_m68k.M68K_OP_REG:
            insn.dest_reg = last.reg

    @staticmethod
    def _access(mnemonic: str, index: int, last: int, count: int) -> str:
        """Heuristica de leitura/escrita do operando.

        Baseada apenas na convencao do 68000 (destino = ultimo operando).
        Nao e' exata para todos os casos; ver docs/03-memory-map.md.
        """
        is_dest = index == last
        if count == 1:
            if mnemonic in WRITES_SINGLE_OPERAND:
                return "rw" if mnemonic in READ_MODIFY_WRITE else "w"
            return "r"
        if is_dest and mnemonic in WRITES_LAST_OPERAND:
            return "rw" if mnemonic in READ_MODIFY_WRITE else "w"
        return "r"


def _sign_extend_short(value: int, address_mode: int) -> int:
    """Enderecos absolutos .w sao estendidos com sinal para 24 bits pelo 68000."""
    value &= 0xFFFFFFFF
    if address_mode == cs_m68k.M68K_AM_ABSOLUTE_DATA_SHORT and value & 0x8000:
        value |= 0xFFFF0000
    return value & 0xFFFFFF
