"""Varredura recursiva de codigo a partir dos vetores do 68000.

Percorre a ROM seguindo o fluxo de execucao (branches, bsr/jsr, jmp) a partir
dos vetores de excecao e do entry point, separando o que e' comprovadamente
codigo do que ainda e' desconhecido, e registrando as referencias a memoria.

Uso:
    python tools/trace_code.py              # traca e grava analysis/*.csv
    python tools/trace_code.py --dry-run    # so o relatorio, nao escreve nada
    python tools/trace_code.py --seed 1234  # semente extra (hex ou decimal)

Principio: **nao adivinhar**. Um endereco so vira "code" se houver um caminho
de execucao ate ele partindo de um vetor. Todo destino indireto que o tracer
nao consegue resolver (jmp indireto por registrador, tabelas de salto) vai para
``analysis/unresolved-flow.csv`` para investigacao manual — sao esses os pontos
onde a cobertura para de crescer sozinha.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from m68k import Disassembler, Insn  # noqa: E402
from romlib import (Rom, hardware_name, hexa, load_data_regions,  # noqa: E402
                    load_ram_names, load_symbol_names, project_root, region_of)

ANALYSIS_DIR = project_root() / "analysis"
JUMP_TABLES_PATH = ANALYSIS_DIR / "jump-tables.csv"
DISPATCHERS_PATH = ANALYSIS_DIR / "dispatchers.csv"

# Limites de sanidade para tabelas inline de offset16 (ver dispatchers.csv).
# O indice e' mascarado com $FF no despachante, entao 256 e' o teto real.
MAX_INLINE_ENTRIES = 256

# Quantas instrucoes olhar para tras ao procurar quem carregou o registrador
# de um salto indireto.
LOOKBACK = 4


def load_jump_tables(path: Path = JUMP_TABLES_PATH) -> list[dict]:
    """Le as tabelas de salto ja verificadas a mao.

    Esse arquivo e' *entrada*, nao saida: cada linha e' um destino indireto que
    alguem confirmou manualmente. E' assim que a cobertura da varredura cresce
    sem que a ferramenta precise adivinhar.
    """
    if not path.exists():
        return []
    tables = []
    with path.open(encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            if not row.get("site", "").strip():
                continue
            tables.append({
                "site": int(row["site"].lstrip("$"), 16),
                "base": int(row["base"].lstrip("$"), 16),
                "count": int(row["count"]),
                "stride": int(row["stride"]),
                "kind": row["kind"].strip(),
                "name": row.get("name", "").strip(),
            })
    return tables


def load_dispatchers(path: Path = DISPATCHERS_PATH) -> dict[int, dict]:
    """Le os despachantes genericos verificados a mao.

    Um despachante e' uma sub-rotina que consome uma tabela colocada *inline*
    logo apos o ``jsr`` que a chama. Declarar o padrao uma vez resolve todos os
    seus call sites de uma so vez.
    """
    if not path.exists():
        return {}
    out = {}
    with path.open(encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            if not row.get("address", "").strip():
                continue
            addr = int(row["address"].lstrip("$"), 16)
            site = row.get("jump_site", "").strip()
            out[addr] = {"address": addr, "kind": row["kind"].strip(),
                         "name": row.get("name", "").strip(),
                         "jump_site": int(site.lstrip("$"), 16) if site else None}
    return out


class Trace:
    """Resultado de uma varredura: instrucoes, funcoes, referencias, pendencias."""

    def __init__(self, rom: Rom, tables: list[dict] | None = None,
                 dispatchers: dict[int, dict] | None = None,
                 detect: bool = True):
        self.rom = rom
        self.names = load_symbol_names()
        # Faixas de dado ja' identificadas saem do balaio 'unknown'.
        self.data_regions = load_data_regions()
        self.detect = detect
        self.detected_tables: list[dict] = []
        self.tables = tables or []
        self.dispatchers = dispatchers or {}
        self.inline_tables: list[dict] = []
        # O jmp indireto interno de um despachante e' indireto por construcao;
        # quem o resolve e' a tabela inline de cada call site, nao ele mesmo.
        self.dispatcher_cores = {d["jump_site"] for d in self.dispatchers.values()
                                 if d.get("jump_site")}
        self.resolved_sites = {t["site"]: t for t in self.tables}
        self.table_bytes: set[int] = set()
        self.dis = Disassembler(rom.data)
        self.insns: dict[int, Insn] = {}
        self.code = bytearray(rom.size)            # 1 = byte pertence a uma instrucao
        self.starts = bytearray(rom.size)          # 1 = inicio de instrucao
        self.entries: dict[int, str] = {}          # endereco -> tipo de entrada
        self.callers: dict[int, set[int]] = defaultdict(set)
        self.branch_targets: set[int] = set()
        self.unresolved: list[tuple[int, str, str]] = []   # (addr, tipo, texto)
        self.bad_decodes: list[tuple[int, int]] = []       # (addr, origem)
        self.overlaps: list[tuple[int, int]] = []          # (addr, instrucao ja existente)
        self.refs: dict[int, dict] = {}                    # alvo -> agregado

    # -- varredura ---------------------------------------------------------

    def run(self, extra_seeds: list[int] | None = None) -> None:
        stack: list[tuple[int, int | None]] = []

        def seed(addr: int, kind: str, origin: int | None = None) -> None:
            if not self.rom.contains(addr):
                return
            self.entries.setdefault(addr, kind)
            stack.append((addr, origin))

        seed(self.rom.entry_point, "reset")
        for vec in self.rom.vectors()[1:]:
            if self.rom.contains(vec.value):
                seed(vec.value, "vector")
        for table in self.tables:
            if table["kind"] == "offset16":
                self._expand_offset16(table["base"], table["site"], seed,
                                      table["name"], table["count"] or None)
                continue
            if table["kind"] == "fixed":
                # Salto indireto de destino unico: o registrador e' carregado
                # com um endereco fixo, entao nao ha' tabela nenhuma.
                seed(table["base"], "table", table["site"])
                self.callers[table["base"]].add(table["site"])
                continue
            if table["kind"] == "pointer":
                self._expand_pointer_table(table["base"], table["site"], seed,
                                           table["name"], table["count"] or None)
                continue
            for target in self._table_targets(table):
                seed(target, "table", table["site"])
                self.callers[target].add(table["site"])
        for addr in extra_seeds or []:
            seed(addr, "manual")

        while stack:
            addr, origin = stack.pop()
            history: list[Insn] = []       # instrucoes recentes desta caminhada linear
            while True:
                if not self.rom.contains(addr) or addr + 1 >= self.rom.size:
                    break
                if self.starts[addr]:
                    break                      # ja percorrido a partir daqui
                if self.code[addr]:
                    # Cai no meio de uma instrucao ja decodificada: sinal de que
                    # um dos dois caminhos esta' interpretando dados como codigo.
                    self.overlaps.append((addr, origin or -1))
                    break

                insn = self.dis.decode(addr)
                if insn is None:
                    self.bad_decodes.append((addr, origin or -1))
                    break

                self.insns[addr] = insn
                self.starts[addr] = 1
                for i in range(addr, insn.end):
                    self.code[i] = 1
                self._record_refs(insn)

                if insn.target is not None and self.rom.contains(insn.target):
                    if insn.flow == "call":
                        self.entries.setdefault(insn.target, "sub")
                        self.callers[insn.target].add(addr)
                        stack.append((insn.target, addr))
                        if insn.target in self.dispatchers and self._expand_inline_table(insn, seed):
                            break   # a tabela ocupa o fallthrough: o jsr nao retorna aqui
                    else:
                        self.branch_targets.add(insn.target)
                        stack.append((insn.target, addr))
                elif insn.flow in ("indirect_call", "indirect_jump") \
                        and addr not in self.resolved_sites \
                        and addr not in self.dispatcher_cores:
                    if not (self._detect_pc_table(insn, history, seed)
                            or self._detect_pointer_table(insn, history, seed)):
                        self.unresolved.append((addr, insn.flow, insn.text))

                if insn.stops_flow:
                    break
                history.append(insn)
                del history[:-LOOKBACK]
                addr = insn.end

    def _find_loader(self, history: list[Insn], match, reg: int) -> Insn | None:
        """Procura, para tras, a instrucao que carregou ``reg``.

        Percorre no maximo LOOKBACK instrucoes e para assim que alguma
        instrucao intermediaria escreve em ``reg`` — se algo sobrescreveu o
        registrador, o candidato anterior nao e' mais quem manda no salto.
        """
        for insn in reversed(history[-LOOKBACK:]):
            if match(insn):
                return insn
            if insn.dest_reg == reg:
                return None
        return None

    def _detect_pc_table(self, insn: Insn, history: list[Insn], seed) -> bool:
        """Reconhece o idioma de tabela de offsets16 indexada por PC.

            move.w  BASE(pc, dN.w), dN     <- carrega o offset da propria tabela
            jmp     BASE(pc, dN.w)         <- salta para BASE + offset

        A assinatura e' estrita: tem de existir um ``move.w`` recente com a
        *mesma* base de tabela e destino no *mesmo* registrador usado como
        indice do salto. Isso distingue esse caso do outro uso de ``d(pc,Xn)``,
        em que as entradas sao codigo inline (como $00077E e $0009E4) e nao
        offsets.

        A busca aceita instrucoes no meio (em $00DB70 ha' uma troca de a0 com a1 (exg)
        entre o loader e o ``jsr``), desde que nenhuma delas escreva no
        registrador de indice.

        Nao e' adivinhacao: a contagem sai da regra geometrica e todo alvo e'
        validado antes de ser semeado. As tabelas reconhecidas assim vao para
        analysis/jump-tables-detected.csv para revisao humana.
        """
        if not self.detect or insn.pc_index_base is None:
            return False
        base, index = insn.pc_index_base, insn.pc_index_reg
        loader = self._find_loader(
            history,
            lambda i: (i.mnemonic == "move" and i.suffix == "w"
                       and i.pc_index_base == base and i.dest_reg == index),
            index)
        if loader is None:
            return False

        accepted = self._expand_offset16(base, insn.address, seed, "auto-detectada")
        if not accepted:
            return False
        self.detected_tables.append({
            "site": insn.address, "base": base, "count": accepted,
            "kind": "offset16", "instruction": insn.text, "loader": loader.text,
        })
        return True

    def _detect_pointer_table(self, insn: Insn, history: list[Insn], seed) -> bool:
        """Reconhece o idioma de tabela de ponteiros indexada por PC.

            movea.l BASE(pc, dN.w), aN     <- carrega o ponteiro
            jmp     (aN)                   <- salta para ele

        Mesmo formato das tabelas ja' declaradas em $0004B6 e $00154A: longs
        absolutos de 32 bits. A contagem vem da mesma regra geometrica dos
        offsets16, com passo 4.
        """
        if not self.detect or insn.indirect_reg is None:
            return False
        reg = insn.indirect_reg
        loader = self._find_loader(
            history,
            lambda i: (i.mnemonic == "movea" and i.suffix == "l"
                       and i.pc_index_base is not None and i.dest_reg == reg),
            reg)
        if loader is None:
            return False

        base = loader.pc_index_base
        accepted = self._expand_pointer_table(base, insn.address, seed, "auto-detectada")
        if not accepted:
            return False
        self.detected_tables.append({
            "site": insn.address, "base": base, "count": accepted,
            "kind": "pointer", "instruction": insn.text, "loader": loader.text,
        })
        return True

    def _pointer_extent(self, base: int) -> int:
        """Mesma regra geometrica dos offsets16, com entradas de 4 bytes."""
        n, limit = 0, self.rom.size
        while base + 4 * (n + 1) <= self.rom.size and n < MAX_INLINE_ENTRIES:
            target = self.rom.long(base + 4 * n)
            new_limit = limit if target < base else min(limit, target)
            if base + 4 * (n + 1) > new_limit:
                break
            limit, n = new_limit, n + 1
        return n

    def _expand_pointer_table(self, base: int, site: int, seed, name: str,
                              declared_count: int | None = None) -> int:
        """Semeia uma tabela de ponteiros absolutos. Validacao tudo-ou-nada."""
        count = declared_count or self._pointer_extent(base)
        targets = []
        for k in range(count):
            slot = base + 4 * k
            if slot + 4 > self.rom.size:
                count = k
                break
            target = self.rom.long(slot)
            if (base <= target < base + 4 * count
                    or not self.rom.contains(target) or target & 1
                    or self.dis.decode(target) is None):
                self.unresolved.append((
                    site, "pointer_table_invalid",
                    f"tabela em {hexa(base)} ({name}) com count={count}: entrada "
                    f"[{k}] aponta para {hexa(target)}; tabela inteira recusada"))
                return 0
            targets.append(target)

        if not targets:
            return 0
        for k, target in enumerate(targets):
            for b in range(base + 4 * k, base + 4 * k + 4):
                self.table_bytes.add(b)
            seed(target, "table", site)
            self.callers[target].add(site)
        self.inline_tables.append({
            "site": site, "base": base, "count": len(targets),
            "declared": count, "dispatcher": name, "targets": targets,
        })
        return len(targets)

    def _offset16_extent(self, base: int) -> int:
        """Deriva quantas entradas tem uma tabela de offsets16.

        Regra geometrica: a tabela se auto-limita. Como as entradas apontam
        para codigo que vem *depois* dela, o fim da tabela nunca pode passar do
        menor alvo proprio. Cresce-se ``n`` enquanto ``base + 2n`` couber antes
        do menor alvo visto ate' ali.

        Alvos *antes* da base (offset negativo) sao handlers compartilhados
        colocados acima da tabela e nao limitam nada.

        Isso substitui a regra ingenua ``count = primeiro_offset / 2``, que
        assume que a entrada [0] aponta para logo apos a tabela — verdade em
        varias tabelas, falsa por exemplo em $0204D6, onde as entradas [1] e [2]
        apontam para instrucoes ``jmp`` posicionadas logo depois da tabela.
        """
        n, limit = 0, self.rom.size
        while base + 2 * (n + 1) <= self.rom.size and n < MAX_INLINE_ENTRIES:
            raw = self.rom.word(base + 2 * n)
            offset = raw - 0x10000 if raw & 0x8000 else raw
            target = base + offset
            new_limit = limit if target < base else min(limit, target)
            if base + 2 * (n + 1) > new_limit:
                break
            limit, n = new_limit, n + 1
        return n

    def _expand_offset16(self, base: int, site: int, seed, name: str,
                         declared_count: int | None = None) -> int:
        """Semeia os alvos de uma tabela de offsets16 e devolve quantos entraram.

        Validacao e' tudo-ou-nada: se *qualquer* entrada falha (fora da ROM,
        endereco impar, nao decodifica, ou cai dentro da propria tabela), a
        tabela inteira e' recusada e o site vai para unresolved-flow.csv.

        Aceitar o prefixo ate' a primeira entrada invalida foi tentado e
        contamina: as entradas anteriores a falha nao sao necessariamente boas.
        Na pratica, duas tabelas assim produziram todos os decodes invalidos e
        todas as sobreposicoes da varredura.
        """
        count = declared_count or self._offset16_extent(base)
        targets: list[int] = []
        invalid_at = None
        for k in range(count):
            slot = base + 2 * k
            if slot + 2 > self.rom.size:
                invalid_at = k
                break
            raw = self.rom.word(slot)
            offset = raw - 0x10000 if raw & 0x8000 else raw
            target = base + offset
            if (base <= target < base + 2 * count
                    or not self.rom.contains(target) or target & 1
                    or self.dis.decode(target) is None):
                invalid_at = k
                break
            targets.append(target)

        if invalid_at is not None:
            self.unresolved.append((
                site, "offset16_table_invalid",
                f"tabela em {hexa(base)} ({name}) com count={count}: entrada "
                f"[{invalid_at}] nao valida; tabela inteira recusada"))
            return 0

        for k, target in enumerate(targets):
            self.table_bytes.add(base + 2 * k)
            self.table_bytes.add(base + 2 * k + 1)
            seed(target, "table", site)
            self.callers[target].add(site)

        self.inline_tables.append({
            "site": site, "base": base, "count": len(targets),
            "declared": count, "dispatcher": name, "targets": targets,
        })
        return len(targets)

    def _expand_inline_table(self, insn: Insn, seed) -> bool:
        """Expande a tabela inline que segue um ``jsr`` para um despachante.

        Ver analysis/dispatchers.csv. Retorna True sempre que o despachante e'
        do tipo esperado: o ``jsr`` nao retorna para a instrucao seguinte,
        entao o fluxo para aqui mesmo que a tabela nao valide.
        """
        dispatcher = self.dispatchers[insn.target]
        if dispatcher["kind"] != "inline_offset16":
            return False
        if insn.address in self.resolved_sites:
            # Count declarado a mao em jump-tables.csv tem precedencia sobre a
            # regra geometrica; a tabela ja' foi semeada no inicio da varredura.
            return True
        self._expand_offset16(insn.end, insn.address, seed, dispatcher["name"])
        return True

    def _table_targets(self, table: dict) -> list[int]:
        """Enderecos alcancaveis por uma tabela de salto declarada."""
        targets = []
        base, stride, kind = table["base"], table["stride"], table["kind"]
        for i in range(table["count"]):
            slot = base + i * stride
            if kind == "pointer":
                if slot + 4 > self.rom.size:
                    continue
                target = self.rom.long(slot)
                for b in range(slot, slot + stride):
                    self.table_bytes.add(b)
            else:                      # "inline": a propria entrada e' codigo
                target = slot
            if self.rom.contains(target):
                targets.append(target)
        return targets

    def _record_refs(self, insn: Insn) -> None:
        for ref in insn.refs:
            entry = self.refs.get(ref.target)
            if entry is None:
                entry = {
                    "target": ref.target,
                    "count": 0,
                    "access": set(),
                    "sizes": set(),
                    "kinds": set(),
                    "first": insn.address,
                    "mnemonics": set(),
                }
                self.refs[ref.target] = entry
            entry["count"] += 1
            entry["access"].add(ref.access)
            entry["kinds"].add(ref.kind)
            entry["mnemonics"].add(insn.mnemonic)
            if insn.suffix:
                entry["sizes"].add(insn.suffix)

    # -- derivacoes --------------------------------------------------------

    def functions(self) -> list[dict]:
        """Delimita funcoes.

        Heuristica documentada: uma funcao vai do seu ponto de entrada ate a
        ultima instrucao *contigua* antes do proximo ponto de entrada. Codigo
        entrelacado ou funcoes com corpo espalhado aparecem como
        ``contiguous=no`` e precisam de revisao manual.
        """
        entries = sorted(self.entries)
        by_index = {addr: i for i, addr in enumerate(entries)}
        out = []
        for addr in entries:
            nxt = entries[by_index[addr] + 1] if by_index[addr] + 1 < len(entries) else self.rom.size
            cur, count, last = addr, 0, addr
            ends_with = ""
            while cur < nxt and cur in self.insns:
                insn = self.insns[cur]
                count += 1
                last = cur
                ends_with = insn.mnemonic
                if insn.stops_flow and insn.end >= nxt:
                    break
                cur = insn.end
            end = self.insns[last].end if last in self.insns else addr
            out.append({
                "address": addr,
                "name": self.label_for(addr),
                "end": end,
                "size": end - addr,
                "instructions": count,
                "callers": len(self.callers.get(addr, ())),
                "kind": self.entries[addr],
                "ends_with": ends_with,
                "contiguous": "yes" if end <= nxt else "no",
            })
        return out

    def label_for(self, addr: int) -> str:
        # Nome verificado a mao tem precedencia sobre o nome sintetico.
        if addr in self.names:
            return self.names[addr]
        kind = self.entries.get(addr)
        if kind == "reset":
            return "Entry"
        if kind == "vector":
            names = [v.name for v in self.rom.vectors() if v.value == addr and v.index > 0]
            return f"Vec_{names[0]}" if names else f"sub_{addr:06X}"
        return f"sub_{addr:06X}"

    def coverage(self) -> tuple[int, int]:
        return sum(self.code), self.rom.size

    def ranges(self) -> list[dict]:
        """Coalesce a ROM em faixas contiguas de mesmo tipo."""
        fixed = [
            (0x000, 0x0FF, "vectors", "Tabela de vetores do 68000"),
            (0x100, 0x1FF, "header", "Header do cartucho"),
        ]
        out = [{"start": s, "end": e, "size": e - s + 1, "kind": k, "notes": n}
               for s, e, k, n in fixed]

        declared = {}
        for region in self.data_regions:
            for addr in range(region["start"], region["end"] + 1):
                declared[addr] = region["kind"]

        def kind_of(addr: int) -> str:
            if self.code[addr]:
                return "code"
            if addr in self.table_bytes:
                return "table"
            return declared.get(addr, "unknown")

        pos = 0x200
        size = self.rom.size
        while pos < size:
            kind = kind_of(pos)
            start = pos
            while pos < size and kind_of(pos) == kind:
                pos += 1
            refs = sum(1 for t in self.refs if start <= t < pos)
            note = ""
            if kind == "unknown":
                note = f"{refs} referencia(s) de codigo apontam para dentro" if refs else "sem referencias diretas"
            out.append({"start": start, "end": pos - 1, "size": pos - start,
                        "kind": kind, "notes": note})
        return out


# ---------------------------------------------------------------------------
# Escrita dos CSVs
# ---------------------------------------------------------------------------


def _write(path: Path, header: list[str], rows: list[list]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(header)
        writer.writerows(rows)


def write_csvs(trace: Trace) -> None:
    rom = trace.rom

    # -- functions.csv
    _write(ANALYSIS_DIR / "functions.csv",
           ["address", "name", "end", "size", "instructions", "callers", "kind",
            "ends_with", "contiguous"],
           [[hexa(f["address"]), f["name"], hexa(f["end"]), f["size"], f["instructions"],
             f["callers"], f["kind"], f["ends_with"], f["contiguous"]]
            for f in trace.functions()])

    # -- rom-map.csv
    _write(ANALYSIS_DIR / "rom-map.csv",
           ["start", "end", "size", "kind", "notes"],
           [[hexa(r["start"]), hexa(r["end"]), r["size"], r["kind"], r["notes"]]
            for r in trace.ranges()])

    # -- ram-map.csv  (referencias em $FF0000-$FFFFFF)
    ram_names = load_ram_names()
    ram_rows = []
    for target, info in sorted(trace.refs.items()):
        if region_of(target)[0] != "ram":
            continue
        ram_rows.append([
            hexa(target), "".join(sorted(info["sizes"])) or "?",
            "".join(sorted(info["access"])), info["count"],
            " ".join(sorted(info["mnemonics"])[:4]), hexa(info["first"]),
            ram_names.get(target, ""),
        ])
    _write(ANALYSIS_DIR / "ram-map.csv",
           ["address", "sizes", "access", "refs", "mnemonics", "first_ref", "name"],
           ram_rows)

    # -- symbols.csv  (funcoes + vetores + registradores de hardware referenciados)
    sym_rows = [[hexa(v.offset, 3), f"Vector_{v.name}", "vector", hexa(v.value, 8), ""]
                for v in rom.vectors()]
    sym_rows += [[hexa(f["address"]), f["name"], "function", "", f"{f['callers']} chamador(es)"]
                 for f in trace.functions()]
    hw = {}
    for target, info in trace.refs.items():
        name = hardware_name(target)
        if name:
            hw[target] = (name, info["count"], region_of(target)[0])
    sym_rows += [[hexa(addr), name, f"hw_{region}", "", f"{count} referencia(s)"]
                 for addr, (name, count, region) in sorted(hw.items())]
    _write(ANALYSIS_DIR / "symbols.csv",
           ["address", "name", "kind", "value", "notes"], sym_rows)

    # -- jump-tables-detected.csv  (saida: reconhecidas pelo idioma, revisar)
    _write(ANALYSIS_DIR / "jump-tables-detected.csv",
           ["site", "base", "count", "kind", "instruction", "loader"],
           [[hexa(t["site"]), hexa(t["base"]), t["count"], t["kind"],
             t["instruction"], t["loader"]]
            for t in sorted(trace.detected_tables, key=lambda t: t["site"])])

    # -- unresolved-flow.csv
    _write(ANALYSIS_DIR / "unresolved-flow.csv",
           ["address", "kind", "instruction", "note"],
           [[hexa(a), k, t, "destino nao resolvido estaticamente"]
            for a, k, t in sorted(trace.unresolved)]
           + [[hexa(a), "bad_decode", "", f"alcancado a partir de {hexa(o)}" if o >= 0 else ""]
              for a, o in sorted(trace.bad_decodes)]
           + [[hexa(a), "overlap", "", f"colide com instrucao ja tracada (origem {hexa(o)})"
               if o >= 0 else "colide com instrucao ja tracada"]
              for a, o in sorted(trace.overlaps)])


# ---------------------------------------------------------------------------
# Relatorio
# ---------------------------------------------------------------------------


def report(trace: Trace) -> None:
    covered, total = trace.coverage()
    funcs = trace.functions()
    ram_refs = [t for t in trace.refs if region_of(t)[0] == "ram"]
    hw_refs = [t for t in trace.refs if region_of(t)[0] in ("vdp", "io", "z80", "z80_bus",
                                                            "z80_reset", "tmss")]
    rom_refs = [t for t in trace.refs if region_of(t)[0] == "rom" and not trace.code[t]]

    print(f"Instrucoes tracadas : {len(trace.insns)}")
    print(f"Cobertura de codigo : {covered} / {total} bytes ({covered / total:.1%})")
    print(f"Funcoes             : {len(funcs)}  "
          f"(entradas: {sum(1 for f in funcs if f['kind'] == 'vector')} de vetor, "
          f"{sum(1 for f in funcs if f['kind'] == 'sub')} de bsr/jsr, "
          f"{sum(1 for f in funcs if f['kind'] == 'table')} de tabela)")
    print(f"Tabelas de salto    : {len(trace.tables)} declaradas em "
          f"{JUMP_TABLES_PATH.name} (sites resolvidos: {len(trace.resolved_sites)})")
    print(f"Tabelas expandidas  : {len(trace.inline_tables)} "
          f"({sum(t['count'] for t in trace.inline_tables)} entradas aceitas)")
    print(f"  reconhecidas auto : {len(trace.detected_tables)} pelo idioma "
          f"move.w BASE(pc,dN.w),dN + jmp BASE(pc,dN.w)")
    print(f"Fluxo nao resolvido : {len(trace.unresolved)} "
          f"(indirect_jump: {sum(1 for _, k, _ in trace.unresolved if k == 'indirect_jump')}, "
          f"indirect_call: {sum(1 for _, k, _ in trace.unresolved if k == 'indirect_call')})")
    print(f"Decodes invalidos   : {len(trace.bad_decodes)}")
    print(f"Sobreposicoes       : {len(trace.overlaps)}")
    print(f"Enderecos de RAM    : {len(ram_refs)} distintos referenciados")
    print(f"Registradores de HW : {len(hw_refs)} distintos referenciados")
    print(f"Refs a dados na ROM : {len(rom_refs)} alvos fora do codigo tracado")

    if trace.unresolved:
        print("\nPrimeiros pontos de fluxo nao resolvido (onde a cobertura trava):")
        for addr, kind, text in sorted(trace.unresolved)[:10]:
            print(f"  {hexa(addr)}  {kind:14} {text}")

    gaps = [r for r in trace.ranges() if r["kind"] == "unknown"]
    gaps.sort(key=lambda r: -r["size"])
    if gaps:
        print("\nMaiores faixas ainda nao classificadas:")
        for r in gaps[:10]:
            print(f"  {hexa(r['start'])}-{hexa(r['end'])}  {r['size']:>7} bytes  {r['notes']}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Varredura recursiva de codigo 68000")
    parser.add_argument("--rom", help="caminho da ROM (padrao: autodetecta em bin/)")
    parser.add_argument("--seed", action="append", default=[],
                        help="endereco extra para iniciar a varredura (0x... ou decimal)")
    parser.add_argument("--dry-run", action="store_true", help="nao escreve os CSVs")
    parser.add_argument("--no-tables", action="store_true",
                        help="ignora jump-tables.csv e dispatchers.csv (so os vetores)")
    args = parser.parse_args(argv)

    rom = Rom.load(args.rom)
    seeds = [int(s, 0) for s in args.seed]

    trace = Trace(rom,
                  [] if args.no_tables else load_jump_tables(),
                  {} if args.no_tables else load_dispatchers(),
                  detect=not args.no_tables)
    trace.run(seeds)
    report(trace)

    if not args.dry_run:
        write_csvs(trace)
        print(f"\nCSVs gravados em {ANALYSIS_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
