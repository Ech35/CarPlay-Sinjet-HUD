"""Build and verify the 8952 R2 real-time Sinjet HUD relay image."""
from hashlib import sha256
from io import BytesIO
from pathlib import Path
from tempfile import TemporaryDirectory
import struct
import sys

HERE = Path(__file__).resolve().parent
WORK, ROOT = HERE.parent, HERE.parent.parent
sys.path[:0] = [str(WORK), str(WORK / "pylibs")]
from elftools.elf.elffile import ELFFile
from PySquashfsImage import SquashFsImage
from repack_u5 import make_tar, metadata, package_update, parse_update, rebuild_squashfs

STOCK = ROOT / "inputs/u6a-app"


def arm_elf(data):
    elf = ELFFile(BytesIO(data))
    assert elf.elfclass == 32 and elf.little_endian and elf["e_machine"] == "EM_ARM"
    for seg in elf.iter_segments():
        if seg["p_type"] == "PT_LOAD":
            assert seg["p_filesz"] <= seg["p_memsz"]
            assert seg["p_offset"] + seg["p_filesz"] <= len(data)
            assert (seg["p_vaddr"] - seg["p_offset"]) % seg["p_align"] == 0
    return elf


def arm_branch(address, target, linked):
    offset = target - address - 8
    assert offset % 4 == 0 and -(1 << 25) <= offset < 1 << 25
    return struct.pack("<I", (0xeb000000 if linked else 0xea000000) | ((offset >> 2) & 0xffffff))


def overlay(name, expected_hash, payload_path, address, data_end, hooks):
    stock = (STOCK / name).read_bytes()
    assert sha256(stock).hexdigest() == expected_hash
    old = arm_elf(stock)
    rx, rw = [seg for seg in old.iter_segments() if seg["p_type"] == "PT_LOAD"]
    assert rx["p_vaddr"] == rx["p_offset"] == 0 and rx["p_filesz"] == rx["p_memsz"]
    assert rx["p_flags"] == 5 and rw["p_flags"] == 6 and rw["p_align"] == 0x10000
    assert data_end > rw["p_vaddr"] + rw["p_memsz"]
    split, gap = rw["p_offset"], rw["p_align"]
    payload = arm_elf(payload_path.read_bytes())
    section = payload.get_section_by_name(".payload")
    symbols = payload.get_section_by_name(".symtab")
    assert section["sh_addr"] == address and address >= rx["p_filesz"]
    assert [s.name for s in payload.iter_sections() if s["sh_size"] and s["sh_flags"] & 2] == [".payload"]
    assert not [s for s in payload.iter_sections() if s["sh_size"] and s["sh_type"] in ("SHT_REL", "SHT_RELA")]
    content = section.data()
    end = address + len(content)
    assert end <= split + gap and end <= (rw["p_vaddr"] & ~0xfff)
    patched = bytearray(stock[:split] + bytes(gap) + stock[split:])
    patched[address:end] = content
    for at, opcode, handler, linked in hooks:
        assert stock[at:at+4].hex() == opcode, (name, hex(at))
        symbol, = symbols.get_symbol_by_name(handler)
        patched[at:at+4] = arm_branch(at, symbol["st_value"], linked)
    for i, seg in enumerate(old.iter_segments()):
        at = old["e_phoff"] + i * old["e_phentsize"]
        if seg["p_offset"] >= split:
            struct.pack_into("<I", patched, at+4, seg["p_offset"] + gap)
        if seg["p_type"] == "PT_LOAD":
            if seg["p_flags"] == 5:
                struct.pack_into("<II", patched, at+16, end, end)
            else:
                struct.pack_into("<I", patched, at+20, data_end - seg["p_vaddr"])
    shoff = old["e_shoff"] + gap
    struct.pack_into("<I", patched, 32, shoff)
    for i, sec in enumerate(old.iter_sections()):
        if sec["sh_offset"] >= split:
            struct.pack_into("<I", patched, shoff + i * old["e_shentsize"] + 16, sec["sh_offset"] + gap)
    new = arm_elf(bytes(patched))
    for previous, current in zip(old.iter_sections(), new.iter_sections(), strict=True):
        assert previous.name == current.name
        if previous.name != ".text" and previous["sh_type"] != "SHT_NOBITS":
            assert previous.data() == current.data(), (name, previous.name)
    return bytes(patched)


def binaries():
    iap = overlay(
        "libiap.so", "9f59165eaa4b6435de7851775021491ca07165fe618d2b720001728818a88a26",
        HERE/"payload.elf", 0x21000, 0x3a800, (
            (0x123e0, "0600a0e1", "hook_ident", True),
            (0x2e7c, "010057e3", "hook_before", True),
            (0x2ef8, "04309de5", "hook_after", True),
            (0x11d04, "70402de9", "hook_closing", False),
            (0x7bbc, "0300a0e3", "hook_auth", True),
            (0x7be4, "0000a0e3", "hook_auth_complete", True),
            (0x2ad8, "232b00eb", "hook_control_handoff", True),
            (0xd404, "0c0094e5", "hook_service", True),
            (0x47a8, "87f3ffeb", "hook_wait", True),
        ),
    )
    cp = overlay(
        "lylinkapp", "84b4471e31e89d5e1716b554b0cc6da7dc4f7fea899a4fb6e408e608ea98549a",
        HERE/"cp_payload.elf", 0xf000, 0x1f200, (
            (0x886c, "bae5ffea", "hook_carplay_send", False),
            (0x8870, "3c309fe5", "hook_carplay_skipped", True),
        ),
    )
    (HERE/"candidate-libiap.so").write_bytes(iap)
    (HERE/"candidate-lylinkapp").write_bytes(cp)
    return iap, cp


if __name__ == "__main__":
    binaries()
