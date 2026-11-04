"""Inspeciona um trecho da ROM: disassembly ou hex.

Ferramenta de bancada para investigar os sites de `analysis/unresolved-flow.csv`.

Uso:
    python tools/dump.py 0x9c0                 # 24 instrucoes a partir de $9C0
    python tools/dump.py 0x9c0 -n 40           # 40 instrucoes
    python tools/dump.py 0x9e4 --hex 64        # 64 bytes em hex
    python tools/dump.py 0x9e4 --table 8       # le 8 words como offsets relativos
    python tools/dump.py 0x4b6 --ptr 16        # le 16 longs como ponteiros
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from m68k import Disassembler  # noqa: E402
from romlib import Rom, hexa, region_of  # noqa: E402


def show_disasm(rom: Rom, dis: Disassembler, start: int, count: int) -> None:
    addr = start
    for _ in range(count):
        insn = dis.decode(addr)
        if insn is None:
            print(f"{hexa(addr)}  <nao decodifica>  {rom.data[addr:addr + 8].hex()}")
            return
        flow = "" if insn.flow == "normal" else f"   ; {insn.flow}"
        print(f"{hexa(addr)}  {rom.data[addr:insn.end].hex():<20} {insn.text}{flow}")
        addr = insn.end


def show_hex(rom: Rom, start: int, count: int) -> None:
    for off in range(start, min(start + count, rom.size), 16):
        chunk = rom.data[off:off + 16]
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"{hexa(off)}  {' '.join(f'{b:02X}' for b in chunk):<47}  {text}")


def show_offset_table(rom: Rom, dis: Disassembler, base: int, count: int) -> None:
    """Le words como offsets com sinal relativos a ``base`` (formato do $5CE)."""
    print(f"Tabela de offsets16 em {hexa(base)} (relativos a base):")
    for k in range(count):
        slot = base + 2 * k
        raw = rom.word(slot)
        offset = raw - 0x10000 if raw & 0x8000 else raw
        target = base + offset
        insn = dis.decode(target) if rom.contains(target) else None
        inside = base <= target < base + 2 * count
        flag = "  <-- dentro da tabela" if inside else ("" if insn else "  <-- nao decodifica")
        text = insn.text if insn else ""
        print(f"  [{k:3d}] {hexa(slot)} = {raw:04X} ({offset:+6d}) -> {hexa(target)}  {text}{flag}")


def show_pointer_table(rom: Rom, dis: Disassembler, base: int, count: int) -> None:
    print(f"Tabela de ponteiros de 32 bits em {hexa(base)}:")
    for k in range(count):
        slot = base + 4 * k
        value = rom.long(slot)
        ok = rom.contains(value) and not value & 1
        insn = dis.decode(value) if ok else None
        text = insn.text if insn else "<fora da ROM ou impar>"
        print(f"  [{k:3d}] {hexa(slot)} -> {value:08X}  {text}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Inspeciona um trecho da ROM")
    parser.add_argument("address", help="endereco inicial (0x... ou decimal)")
    parser.add_argument("-n", "--count", type=int, default=24, help="numero de instrucoes")
    parser.add_argument("--hex", type=int, metavar="N", help="mostra N bytes em hex")
    parser.add_argument("--table", type=int, metavar="N", help="le N words como offsets16")
    parser.add_argument("--ptr", type=int, metavar="N", help="le N longs como ponteiros")
    parser.add_argument("--rom", help="caminho da ROM")
    args = parser.parse_args(argv)

    rom = Rom.load(args.rom)
    dis = Disassembler(rom.data)
    addr = int(args.address, 0)
    if not rom.contains(addr):
        print(f"{hexa(addr)} esta' fora da ROM ({region_of(addr)[1]})", file=sys.stderr)
        return 1

    if args.hex:
        show_hex(rom, addr, args.hex)
    elif args.table:
        show_offset_table(rom, dis, addr, args.table)
    elif args.ptr:
        show_pointer_table(rom, dis, addr, args.ptr)
    else:
        show_disasm(rom, dis, addr, args.count)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
