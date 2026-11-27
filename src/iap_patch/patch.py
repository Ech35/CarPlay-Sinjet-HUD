from hashlib import sha256
from io import BytesIO
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile


def branch(src, dst, link=True):
    displacement = dst - src - 8
    assert displacement % 4 == 0 and -(1 << 25) <= displacement < (1 << 25)
    return struct.pack("<I", (0xeb000000 if link else 0xea000000) | ((displacement >> 2) & 0xffffff))


work = Path(__file__).parent
original = (work.parent.parent / "inputs/u5-app/libiap_new.so").read_bytes()
assert sha256(original).hexdigest() == "5918ccb1ce396b0469167ba23a2b551b744772e4c60422aa9857fbb810ced94b"
image = ELFFile(BytesIO(original))
phoff = image.header["e_phoff"]
shoff = image.header["e_shoff"]
split = 0x1d4a0
padding = 0x10000
payload = ELFFile(BytesIO((work / "payload.elf").read_bytes()))
content = payload.get_section_by_name(".payload")
assert content["sh_addr"] == 0x1e000
assert not [s for s in payload.iter_sections()
            if s["sh_type"] in ("SHT_REL", "SHT_RELA") and s["sh_size"]]
assert [s.name for s in payload.iter_sections()
        if s["sh_flags"] & 2 and s["sh_size"]] == [".payload"]
code = content.data()
code_end = content["sh_addr"] + len(code)
assert code_end < 0x2d000
out = bytearray(original[:split] + bytes(padding) + original[split:])
out[content["sh_addr"]:code_end] = code
symbols = payload.get_section_by_name(".symtab")
for addr, expected, name in (
    (0x13ecc, "04309be5", "probe_outbound"),
    (0x14330, "003097e5", "probe_accepted"),
    (0x14350, "0600a0e1", "probe_ready"),
    (0x14174, "0600a0e1", "probe_accepted_return"),
    (0x0d480, "5431d4e5", "probe_received"),
    (0x0d354, "003090e5", "probe_hu_location"),
    (0x0cd90, "f04f2de9", "probe_transport"),
    (0x08458, "8c2080e5", "probe_pass_state"),
    (0x084a0, "08d08de2", "probe_pass_applied"),
):
    assert original[addr:addr + 4] == bytes.fromhex(expected)
    sym, = symbols.get_symbol_by_name(name)
    # SendiAPRawDataToHS has not saved LR at its first instruction. A BL here
    # destroys its caller's return address; the entry trampoline must use B.
    out[addr:addr + 4] = branch(addr, sym["st_value"], link=addr not in (0x0d354, 0x0cd90))

# Existing PT_LOAD virtual addresses are preserved. Insert a whole segment
# alignment in the file so all relocated p_offset values remain congruent.
for idx, seg in enumerate(image.iter_segments()):
    at = phoff + image.header["e_phentsize"] * idx
    if seg["p_offset"] >= split:
        struct.pack_into("<I", out, at + 4, seg["p_offset"] + padding)
    if seg["p_type"] == "PT_LOAD" and seg["p_vaddr"] == 0:
        struct.pack_into("<II", out, at + 16, code_end, code_end)
    if seg["p_type"] == "PT_LOAD" and seg["p_vaddr"] == 0x2d4a0:
        # Reserve diagnostic mapping state and a bounded HU control-frame buffer.
        assert seg["p_vaddr"] + seg["p_memsz"] == 0x2e490
        struct.pack_into("<I", out, at + 20, seg["p_memsz"] + 0x4000)

assert shoff >= split
struct.pack_into("<I", out, 32, shoff + padding)
for idx, section in enumerate(image.iter_sections()):
    if section["sh_offset"] >= split:
        at = shoff + padding + image.header["e_shentsize"] * idx
        struct.pack_into("<I", out, at + 16, section["sh_offset"] + padding)

patched = ELFFile(BytesIO(out))
for old, new in zip(image.iter_sections(), patched.iter_sections(), strict=True):
    assert old.name == new.name
    if old.name != ".text":
        assert old.data() == new.data(), old.name
(work / "libiap_new.so").write_bytes(out)
print(f"patched {len(out)} bytes; SHA-256 {sha256(out).hexdigest()}")
