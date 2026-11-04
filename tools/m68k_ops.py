"""Traducao de instrucoes 68000 para C, instrucao a instrucao.

Separado de ``recompile.py`` porque e' a parte que mais cresce: cada modo de
enderecamento e cada mnemonico novo entra aqui, e o resto do recompilador
(descoberta de funcao, labels, fluxo) fica estavel.

Convencao: cada operando vira uma expressao C, possivelmente precedida de
statements de preparacao. Modos com efeito colateral — ``(aN)+`` e ``-(aN)`` —
sao a razao dessa separacao: o incremento tem de acontecer na ordem certa em
relacao a leitura, e num unico ponto mesmo quando o operando e' lido e escrito.
"""

from __future__ import annotations

import re

SIZE_BITS = {"b": 8, "w": 16, "l": 32}


class Unsupported(Exception):
    """Instrucao ou modo de enderecamento que o tradutor ainda nao cobre."""


# --- formas de operando que o capstone imprime ---------------------------

IMMEDIATE = re.compile(r"^#\$?(-?[0-9a-fA-F]+)$")
DATA_REG = re.compile(r"^d([0-7])$")
ADDR_REG = re.compile(r"^a([0-7])$")
ABSOLUTE = re.compile(r"^\$([0-9a-fA-F]+)\.([wl])$")
INDIRECT = re.compile(r"^\(a([0-7])\)$")
POSTINC = re.compile(r"^\(a([0-7])\)\+$")
PREDEC = re.compile(r"^-\(a([0-7])\)$")
DISP = re.compile(r"^(-?)\$([0-9a-fA-F]+)\(a([0-7])\)$")
INDEX = re.compile(r"^(-?)\$([0-9a-fA-F]+)\(a([0-7]), *([ad])([0-7])\.([wl])\)$")
# Indexado sem deslocamento: "(a6, d0.w)". O capstone omite o "$0(".
INDEX_NO_DISP = re.compile(r"^\(a([0-7]), *([ad])([0-7])\.([wl])\)$")
PC_DISP = re.compile(r"^\$([0-9a-fA-F]+)\(pc\)$")
PC_INDEX = re.compile(r"^\$([0-9a-fA-F]+)\(pc, *([ad])([0-7])\.([wl])\)$")


def absolute_address(match: re.Match) -> int:
    """Endereco absoluto ja' estendido com sinal.

    O 68000 estende a forma ``.w`` com sinal para 32 bits: ``$FB02.w`` e'
    $FFFFFB02, na RAM de trabalho. Sem estender, o acesso cairia em $00FB02,
    dentro da ROM. O vasm recusa esse erro na fase de reassembly; aqui ele
    passaria silencioso.
    """
    value = int(match.group(1), 16)
    if match.group(2) == "w" and value & 0x8000:
        value |= 0xFFFF0000
    return value & 0xFFFFFFFF


def _index_expr(kind: str, number: str, size: str) -> str:
    """Registrador de indice, estendido com sinal quando usado como .w."""
    base = f"cpu->{'d' if kind == 'd' else 'a'}[{number}]"
    return base if size == "l" else f"(uint32_t)(int32_t)(int16_t){base}"


class Operand:
    """Um operando decodificado, pronto para virar leitura ou escrita.

    ``prepare`` sao os statements que precisam rodar antes de usar ``read()``
    ou ``write()``. Para ``(aN)+`` e ``-(aN)`` e' onde o ponteiro anda, e o
    endereco efetivo fica guardado numa variavel local para que ler e escrever
    o mesmo operando nao ande duas vezes.
    """

    def __init__(self, text: str, size: str, temp: str):
        self.text = text
        self.size = size
        self.bits = SIZE_BITS[size]
        self.prepare: list[str] = []
        self._reg: tuple[str, int] | None = None   # ("d"|"a", numero)
        self._addr: str | None = None              # expressao de endereco

        step = self.bits // 8
        m = DATA_REG.match(text)
        if m:
            self._reg = ("d", int(m.group(1)))
            return
        m = ADDR_REG.match(text)
        if m:
            self._reg = ("a", int(m.group(1)))
            return
        m = IMMEDIATE.match(text)
        if m:
            self._addr = None
            self._immediate = int(m.group(1), 16) & ((1 << self.bits) - 1)
            return
        m = ABSOLUTE.match(text)
        if m:
            self._addr = f"0x{absolute_address(m):X}u"
            return
        m = INDIRECT.match(text)
        if m:
            self._addr = f"cpu->a[{m.group(1)}]"
            return
        m = POSTINC.match(text)
        if m:
            reg = int(m.group(1))
            # (a7)+ com byte anda 2, para manter a pilha alinhada.
            delta = 2 if reg == 7 and step == 1 else step
            self.prepare = [f"uint32_t {temp} = cpu->a[{reg}];",
                            f"cpu->a[{reg}] += {delta};"]
            self._addr = temp
            return
        m = PREDEC.match(text)
        if m:
            reg = int(m.group(1))
            delta = 2 if reg == 7 and step == 1 else step
            self.prepare = [f"cpu->a[{reg}] -= {delta};",
                            f"uint32_t {temp} = cpu->a[{reg}];"]
            self._addr = temp
            return
        m = DISP.match(text)
        if m:
            sign, value, reg = m.group(1), int(m.group(2), 16), m.group(3)
            self._addr = f"(cpu->a[{reg}] + {'-' if sign else ''}0x{value:X})"
            return
        m = INDEX.match(text)
        if m:
            sign, value, reg = m.group(1), int(m.group(2), 16), m.group(3)
            index = _index_expr(m.group(4), m.group(5), m.group(6))
            self._addr = f"(cpu->a[{reg}] + {'-' if sign else ''}0x{value:X} + {index})"
            return
        m = INDEX_NO_DISP.match(text)
        if m:
            index = _index_expr(m.group(2), m.group(3), m.group(4))
            self._addr = f"(cpu->a[{m.group(1)}] + {index})"
            return
        m = PC_DISP.match(text)
        if m:
            # O capstone ja' resolve o deslocamento PC-relativo para o
            # endereco final, entao vira absoluto.
            self._addr = f"0x{int(m.group(1), 16):X}u"
            return
        m = PC_INDEX.match(text)
        if m:
            index = _index_expr(m.group(2), m.group(3), m.group(4))
            self._addr = f"(0x{int(m.group(1), 16):X}u + {index})"
            return
        raise Unsupported(f"modo de enderecamento {text!r}")

    @property
    def is_immediate(self) -> bool:
        return self._reg is None and self._addr is None

    def read(self) -> str:
        if self._reg:
            kind, number = self._reg
            if kind == "d":
                return f"d{self.bits}(cpu, {number})"
            if self.bits == 32:
                return f"cpu->a[{number}]"
            return f"(uint{self.bits}_t)cpu->a[{number}]"
        if self.is_immediate:
            return f"0x{self._immediate:X}u"
        return f"md_read{self.bits}({self._addr})"

    def address(self) -> str:
        """Endereco efetivo, para instrucoes como LEA que nao leem a memoria."""
        if self._addr is None:
            raise Unsupported(f"{self.text!r} nao tem endereco efetivo")
        return self._addr

    def write(self, value: str) -> str:
        if self._reg:
            kind, number = self._reg
            if kind == "d":
                return f"set_d{self.bits}(cpu, {number}, {value});"
            return f"set_a{32 if self.bits == 32 else 16}(cpu, {number}, {value});"
        if self.is_immediate:
            raise Unsupported("escrita em operando imediato")
        return f"md_write{self.bits}({self._addr}, {value});"


# --- listas de registrador (MOVEM) ---------------------------------------

REG_RANGE = re.compile(r"^([ad])([0-7])-([ad])([0-7])$")
REG_SINGLE = re.compile(r"^([ad])([0-7])$")


def parse_register_list(text: str) -> list[tuple[str, int]]:
    """``d0-d7/a0-a6`` -> [('d',0), ..., ('d',7), ('a',0), ..., ('a',6)].

    A ordem devolvida e' sempre d0..d7 depois a0..a7, que e' a ordem dos bits
    na mascara do MOVEM. Quem inverte, quando precisa, e' o gerador.
    """
    selecionados: set[tuple[str, int]] = set()
    for parte in text.split("/"):
        parte = parte.strip()
        m = REG_RANGE.match(parte)
        if m:
            if m.group(1) != m.group(3):
                raise Unsupported(f"faixa de registradores mista: {parte!r}")
            for n in range(int(m.group(2)), int(m.group(4)) + 1):
                selecionados.add((m.group(1), n))
            continue
        m = REG_SINGLE.match(parte)
        if m:
            selecionados.add((m.group(1), int(m.group(2))))
            continue
        raise Unsupported(f"lista de registradores {text!r}")
    return sorted(selecionados, key=lambda r: (r[0] == "a", r[1]))


def register_expr(kind: str, number: int, bits: int) -> str:
    campo = f"cpu->{kind}[{number}]"
    return campo if bits == 32 else f"(uint16_t){campo}"


def register_store(kind: str, number: int, bits: int, value: str) -> str:
    """Carga de MOVEM.

    Em ``movem.w``, o valor lido e' estendido com sinal para 32 bits **mesmo em
    registrador de dados** — MOVEM e' a excecao a regra de que escrita .w
    preserva a metade alta.
    """
    if bits == 32:
        return f"cpu->{kind}[{number}] = {value};"
    return f"cpu->{kind}[{number}] = (uint32_t)(int32_t)(int16_t)({value});"


# --- condicoes de desvio --------------------------------------------------

CONDITIONS = {
    "bra": "1", "bt": "1", "bf": "0",
    "beq": "cpu->z", "bne": "!cpu->z",
    "bcs": "cpu->c", "bcc": "!cpu->c",
    "bmi": "cpu->n", "bpl": "!cpu->n",
    "bvs": "cpu->v", "bvc": "!cpu->v",
    "bhi": "(!cpu->c && !cpu->z)", "bls": "(cpu->c || cpu->z)",
    "bge": "(cpu->n == cpu->v)", "blt": "(cpu->n != cpu->v)",
    "bgt": "(!cpu->z && cpu->n == cpu->v)", "ble": "(cpu->z || cpu->n != cpu->v)",
}

# Mnemonicos que so' leem o destino e nao o escrevem.
COMPARE_ONLY = {"cmp", "cmpi", "cmpa", "tst"}
