"""Biblioteca base de leitura da ROM de Mega Drive.

Toda ferramenta em ``tools/`` importa daqui: carregamento da ROM, parsing do
header, tabela de vetores, checksum, hashes e classificacao de enderecos no
mapa de memoria do console.

Nao depende de nada alem da stdlib. O disassembler (capstone) vive em
``m68k.py`` para que ``rom_info.py`` funcione sem ele instalado.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path

# ---------------------------------------------------------------------------
# Constantes do formato
# ---------------------------------------------------------------------------

VECTOR_TABLE_SIZE = 0x100          # 64 vetores de 4 bytes
HEADER_OFFSET = 0x100
HEADER_SIZE = 0x100
CHECKSUM_OFFSET = 0x18E
CHECKSUM_RANGE_START = 0x200       # checksum cobre $200 ate o fim da ROM

# (chave, offset, tamanho, rotulo legivel)
HEADER_FIELDS = [
    ("console",        0x100, 16, "Console"),
    ("copyright",      0x110, 16, "Copyright"),
    ("title_domestic", 0x120, 48, "Titulo domestico"),
    ("title_overseas", 0x150, 48, "Titulo overseas"),
    ("serial",         0x180, 14, "Serial"),
    ("io_support",     0x190, 16, "Suporte de I/O"),
    ("modem",          0x1BC, 12, "Modem"),
    ("notes",          0x1C8, 40, "Notas"),
    ("region",         0x1F0, 16, "Regiao"),
]

# Campos numericos do header: (chave, offset, tamanho em bytes)
HEADER_NUMERIC = [
    ("checksum",   0x18E, 2),
    ("rom_start",  0x1A0, 4),
    ("rom_end",    0x1A4, 4),
    ("ram_start",  0x1A8, 4),
    ("ram_end",    0x1AC, 4),
    ("sram_type",  0x1B2, 2),
    ("sram_start", 0x1B4, 4),
    ("sram_end",   0x1B8, 4),
]

# Nomes dos 64 vetores de excecao do 68000. Indices nao listados sao
# reservados pela Motorola e nao devem ser usados por software.
VECTOR_NAMES = {
    0: "initial_sp",
    1: "reset_pc",
    2: "bus_error",
    3: "address_error",
    4: "illegal_instruction",
    5: "divide_by_zero",
    6: "chk_instruction",
    7: "trapv_instruction",
    8: "privilege_violation",
    9: "trace",
    10: "line_1010_emulator",
    11: "line_1111_emulator",
    14: "format_error",
    15: "uninitialized_interrupt",
    24: "spurious_interrupt",
    25: "level_1_irq",
    26: "level_2_irq_ext",       # controle externo (porta de expansao)
    27: "level_3_irq",
    28: "level_4_irq_hblank",    # H-blank, gerado pelo VDP
    29: "level_5_irq",
    30: "level_6_irq_vblank",    # V-blank, gerado pelo VDP
    31: "level_7_irq_nmi",
}
VECTOR_NAMES.update({32 + i: f"trap_{i:02d}" for i in range(16)})

# Regioes do mapa de memoria do Mega Drive vistas pelo 68000.
# (inicio, fim inclusivo, chave, descricao)
MEMORY_REGIONS = [
    (0x000000, 0x3FFFFF, "rom",       "ROM do cartucho"),
    (0x400000, 0x7FFFFF, "expansion", "Area de expansao / 32X / Mega CD"),
    (0x800000, 0x9FFFFF, "reserved",  "Reservado"),
    (0xA00000, 0xA0FFFF, "z80",       "Espaco de enderecos do Z80"),
    (0xA10000, 0xA1001F, "io",        "Registradores de I/O (versao, portas de controle)"),
    (0xA11000, 0xA110FF, "memmode",   "Registrador de modo de memoria"),
    (0xA11100, 0xA111FF, "z80_bus",   "Z80 BUSREQ"),
    (0xA11200, 0xA112FF, "z80_reset", "Z80 RESET"),
    (0xA12000, 0xA130FF, "cart_ctrl", "Controle de cartucho / Mega CD"),
    (0xA14000, 0xA140FF, "tmss",      "Registrador TMSS"),
    (0xC00000, 0xC0001F, "vdp",       "Portas do VDP (data, control, HV counter, PSG)"),
    (0xFF0000, 0xFFFFFF, "ram",       "RAM de trabalho (64 KB)"),
]

# Registradores de hardware com nome conhecido, para rotular referencias.
HARDWARE_SYMBOLS = {
    0xA00000: "Z80_RAM",
    0xA04000: "YM2612_A0",
    0xA04001: "YM2612_D0",
    0xA04002: "YM2612_A1",
    0xA04003: "YM2612_D1",
    0xA10000: "IO_VERSION",
    0xA10002: "IO_DATA_1",
    0xA10004: "IO_DATA_2",
    0xA10006: "IO_DATA_EXP",
    0xA10008: "IO_CTRL_1",
    0xA1000A: "IO_CTRL_2",
    0xA1000C: "IO_CTRL_EXP",
    0xA11100: "Z80_BUSREQ",
    0xA11200: "Z80_RESET",
    0xA14000: "TMSS_REG",
    0xC00000: "VDP_DATA",
    0xC00002: "VDP_DATA_M",
    0xC00004: "VDP_CTRL",
    0xC00006: "VDP_CTRL_M",
    0xC00008: "VDP_HV_COUNTER",
    0xC00011: "PSG_OUT",
}


# ---------------------------------------------------------------------------
# Modelo
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Vector:
    index: int
    offset: int      # posicao do vetor na tabela
    value: int       # conteudo (endereco de destino, ou SP inicial no indice 0)
    name: str


class Rom:
    """ROM carregada em memoria, com header e vetores ja interpretados."""

    def __init__(self, path: Path, data: bytes):
        self.path = Path(path)
        self.data = data

    # -- carregamento ------------------------------------------------------

    @classmethod
    def load(cls, path: str | Path | None = None) -> "Rom":
        rom_path = Path(path) if path else find_rom()
        data = rom_path.read_bytes()
        if len(data) < 0x200:
            raise ValueError(f"{rom_path}: arquivo pequeno demais para ser uma ROM de Mega Drive")
        if data[0x100:0x104] not in (b"SEGA", b" SEG"):
            raise ValueError(
                f"{rom_path}: sem assinatura 'SEGA' em $100 — arquivo interleaved (.smd) "
                "ou nao e' uma ROM de Mega Drive"
            )
        return cls(rom_path, data)

    # -- propriedades basicas ---------------------------------------------

    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def md5(self) -> str:
        return hashlib.md5(self.data).hexdigest()

    @property
    def sha1(self) -> str:
        return hashlib.sha1(self.data).hexdigest()

    def word(self, offset: int) -> int:
        return int.from_bytes(self.data[offset:offset + 2], "big")

    def long(self, offset: int) -> int:
        return int.from_bytes(self.data[offset:offset + 4], "big")

    # -- header ------------------------------------------------------------

    def header_text(self) -> dict[str, str]:
        out = {}
        for key, off, length, _label in HEADER_FIELDS:
            raw = self.data[off:off + length]
            out[key] = raw.decode("ascii", errors="replace").strip()
        return out

    def header_numeric(self) -> dict[str, int]:
        return {key: int.from_bytes(self.data[off:off + n], "big")
                for key, off, n in HEADER_NUMERIC}

    def has_sram(self) -> bool:
        return self.data[0x1B0:0x1B2] == b"RA"

    # -- checksum ----------------------------------------------------------

    def checksum_stored(self) -> int:
        return self.word(CHECKSUM_OFFSET)

    def checksum_computed(self) -> int:
        """Soma de words big-endian de $200 ate o fim, truncada em 16 bits."""
        total = 0
        data = self.data
        for i in range(CHECKSUM_RANGE_START, len(data) - 1, 2):
            total += (data[i] << 8) | data[i + 1]
        return total & 0xFFFF

    def checksum_ok(self) -> bool:
        return self.checksum_stored() == self.checksum_computed()

    # -- vetores -----------------------------------------------------------

    def vectors(self) -> list[Vector]:
        out = []
        for i in range(64):
            off = i * 4
            out.append(Vector(
                index=i,
                offset=off,
                value=self.long(off),
                name=VECTOR_NAMES.get(i, f"reserved_{i:02d}"),
            ))
        return out

    @property
    def entry_point(self) -> int:
        return self.long(4)

    @property
    def initial_sp(self) -> int:
        return self.long(0)

    def contains(self, addr: int) -> bool:
        """True se o endereco cai dentro dos bytes desta ROM."""
        return 0 <= addr < len(self.data)


# ---------------------------------------------------------------------------
# Utilidades
# ---------------------------------------------------------------------------


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def find_rom() -> Path:
    """Localiza a ROM sem hardcode do nome do arquivo.

    A ROM e' entrada local do usuario e nao e' versionada (ver .gitignore),
    entao procuramos por qualquer .bin em bin/ e depois na raiz.
    """
    root = project_root()
    for directory in (root / "bin", root):
        candidates = sorted(p for p in directory.glob("*.bin") if p.is_file())
        for cand in candidates:
            try:
                head = cand.open("rb").read(0x104)
            except OSError:
                continue
            if len(head) >= 0x104 and head[0x100:0x104] in (b"SEGA", b" SEG"):
                return cand
    raise FileNotFoundError(
        "ROM nao encontrada. Coloque o dump .bin em bin/ ou passe --rom CAMINHO."
    )


def load_symbol_names(path: Path | None = None) -> dict[int, str]:
    """Le analysis/symbol-names.csv: nomes de simbolo verificados a mao.

    Arquivo de *entrada*, versionado, no mesmo espirito de jump-tables.csv.
    Toda linha registra a evidencia que sustenta o nome, porque nome sem
    evidencia e' palpite disfarcado de fato.
    """
    import csv

    path = path or (project_root() / "analysis" / "symbol-names.csv")
    if not path.exists():
        return {}
    names: dict[int, str] = {}
    with path.open(encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            raw = row.get("address", "").strip()
            name = row.get("name", "").strip()
            if not raw or not name:
                continue
            if not name.isidentifier():
                raise ValueError(f"symbol-names.csv: {name!r} nao e' um identificador valido")
            addr = int(raw.lstrip("$"), 16)
            if addr in names:
                raise ValueError(f"symbol-names.csv: endereco {hexa(addr)} duplicado")
            names[addr] = name
    if len(set(names.values())) != len(names):
        raise ValueError("symbol-names.csv: nomes precisam ser unicos")
    return names


def load_data_regions(path: Path | None = None) -> list[dict]:
    """Le analysis/data-regions.csv: faixas de dado ja' identificadas.

    Entrada versionada. Cada faixa tira bytes do balaio "unknown" e passa a ser
    emitida com label e tipo no disassembly, em vez de dc.b anonimo.
    """
    import csv

    path = path or (project_root() / "analysis" / "data-regions.csv")
    if not path.exists():
        return []
    regions = []
    with path.open(encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            if not row.get("start", "").strip():
                continue
            name = row.get("name", "").strip()
            if not name.isidentifier():
                raise ValueError(f"data-regions.csv: {name!r} nao e' um identificador valido")
            regions.append({
                "start": int(row["start"].lstrip("$"), 16),
                "end": int(row["end"].lstrip("$"), 16),
                "kind": row["kind"].strip(),
                "name": name,
            })
    regions.sort(key=lambda r: r["start"])
    for a, b in zip(regions, regions[1:]):
        if b["start"] <= a["end"]:
            raise ValueError(
                f"data-regions.csv: {hexa(a['start'])} e {hexa(b['start'])} se sobrepoem")
    return regions


def load_stack_returns(path: Path | None = None) -> set[int]:
    """Le analysis/stack-returns.csv: sites que desempilham o endereco de retorno.

    Entrada versionada. Sao rotinas que desempilham um long para um registrador de dados seguido de
    ``rts`` para retornar dois niveis acima — idioma que a traducao de ``jsr``
    para chamada C nao reproduz sozinha.
    """
    import csv

    path = path or (project_root() / "analysis" / "stack-returns.csv")
    if not path.exists():
        return set()
    sites = set()
    with path.open(encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            raw = row.get("address", "").strip()
            if raw:
                sites.add(int(raw.lstrip("$"), 16))
    return sites


def load_ram_names(path: Path | None = None) -> dict[int, str]:
    """Le analysis/ram-names.csv: nomes de variaveis de RAM verificados a mao.

    Mesmas regras de symbol-names.csv: entrada versionada, evidencia
    obrigatoria, nome tem de ser um identificador valido e unico.
    """
    import csv

    path = path or (project_root() / "analysis" / "ram-names.csv")
    if not path.exists():
        return {}
    names: dict[int, str] = {}
    with path.open(encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            raw = row.get("address", "").strip()
            name = row.get("name", "").strip()
            if not raw or not name:
                continue
            if not name.isidentifier():
                raise ValueError(f"ram-names.csv: {name!r} nao e' um identificador valido")
            addr = int(raw.lstrip("$"), 16) & 0xFFFFFF
            if region_of(addr)[0] != "ram":
                raise ValueError(f"ram-names.csv: {hexa(addr)} nao esta' na RAM de trabalho")
            if addr in names:
                raise ValueError(f"ram-names.csv: endereco {hexa(addr)} duplicado")
            names[addr] = name
    if len(set(names.values())) != len(names):
        raise ValueError("ram-names.csv: nomes precisam ser unicos")
    return names


def region_of(addr: int) -> tuple[str, str]:
    """Retorna (chave, descricao) da regiao de memoria do endereco."""
    addr &= 0xFFFFFF
    for start, end, key, desc in MEMORY_REGIONS:
        if start <= addr <= end:
            return key, desc
    return "unmapped", "Nao mapeado"


def hardware_name(addr: int) -> str | None:
    """Nome do registrador de hardware, se o endereco for conhecido."""
    addr &= 0xFFFFFF
    if addr in HARDWARE_SYMBOLS:
        return HARDWARE_SYMBOLS[addr]
    # Acessos byte a portas de dados do VDP/IO caem em enderecos impares.
    if addr & 1 and (addr - 1) in HARDWARE_SYMBOLS:
        return HARDWARE_SYMBOLS[addr - 1] + "+1"
    return None


def hexa(addr: int, width: int = 6) -> str:
    return f"${addr:0{width}X}"
