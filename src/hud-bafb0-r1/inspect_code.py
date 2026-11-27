"""Small read-only Thumb inspection helper for the pinned BAFB0 program."""
import argparse
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
TOOLS = Path.home() / 'Desktop/信界HUD绕路升级项目/tools'
sys.path[:0] = [str(TOOLS / 'firmware_disasm_runtime/capstone_pkg')]
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS

BASE = 0x08020200
ROM = (HERE.parent / 'hud-bafb0-201012-audit/system_BAFB0_201012.bin').read_bytes()


def dump(start, end):
    for x in Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS).disasm(ROM[start-BASE:end-BASE], start):
        comment = ''
        if x.mnemonic.startswith('ldr') and '[pc, #' in x.op_str:
            n = x.op_str.split('[pc, #')[1].split(']')[0]
            at = ((x.address + 4) & ~3) + int(n, 0)
            if BASE <= at <= BASE + len(ROM)-4:
                value, = struct.unpack_from('<I', ROM, at-BASE)
                comment = f' ; [{at:#x}]={value:#x}'
                if BASE <= value < BASE + len(ROM):
                    s = ROM[value-BASE:value-BASE+90].split(b'\0')[0]
                    if s and all(32 <= c < 127 for c in s):
                        comment += ' ' + repr(s.decode())
        print(f'{x.address:08x} {x.bytes.hex():10s} {x.mnemonic:8s} {x.op_str}{comment}')


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('ranges', nargs='*', help='hex_start:hex_end')
    p.add_argument('--xref', type=lambda v: int(v,16))
    p.add_argument('--rom', type=Path)
    a = p.parse_args()
    if a.rom:
        ROM = a.rom.read_bytes()
    if a.xref is not None:
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        for i in range(0,len(ROM)-4,2):
            if ROM[i+1] & 0xf8 != 0xf0 or ROM[i+3] & 0xd0 != 0xd0:
                continue
            ins = next(md.disasm(ROM[i:i+4],BASE+i),None)
            if ins and ins.mnemonic in ('bl','b.w') and ins.op_str == f'#{a.xref:#x}':
                print(f'{ins.address:#x}: {ins.mnemonic} {ins.op_str}')
    for r in a.ranges:
        s, e = (int(v, 16) for v in r.split(':'))
        print(f'RANGE {s:#x}:{e:#x}')
        dump(s, e)
