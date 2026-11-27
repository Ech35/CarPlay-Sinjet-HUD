#!/usr/bin/env python3
"""Inspect and prepare Sinjet HUD .pak firmware packages.

The tool never edits an input file in place.  It understands the record
headers used by the packages in this project and verifies each embedded
record by its stored MD5.
Record headers also have an independent one-byte checksum; MD5 alone is not
sufficient to validate a package for the receiver.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"\x66\x60\x23\x84"
INNER_RECORD_TAG = 0x12345678

# Pin the known inputs so a similarly named but different file is not patched.
KNOWN_SHA256 = {
    "mcu_HUDBACA0_230922.pak": "6538b4ec1958799f61359b44e5e13f9c90d74f67ee6131606d6e8821ac42dad7",
    "mcu_HUDBASA0_221019.pak": "8747d98c921ffe5c3d175ce49e7e6c7d34f37797cfdb820f9fd86347e90f26f8",
}


@dataclass(frozen=True)
class Record:
    offset: int
    length: int
    tag: int
    name: str
    md5_offset: int
    stored_md5: str
    search_start: int
    payload_start: int | None

    @property
    def end(self) -> int:
        return self.offset + self.length

    @property
    def kind(self) -> str:
        return "inner" if self.tag == INNER_RECORD_TAG else "top"


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require_known(path: Path) -> None:
    expected = KNOWN_SHA256.get(path.name)
    if expected is None:
        raise ValueError(f"Unpinned input filename: {path.name}")
    actual = file_sha256(path)
    if actual != expected:
        raise ValueError(
            f"Input SHA-256 mismatch for {path.name}\n"
            f"expected: {expected}\nactual:   {actual}"
        )


def _payload_start(data: bytes, start: int, end: int, stored_md5: str) -> int | None:
    # All observed record preambles are under 512 bytes.  Keep a larger bound
    # for robustness while avoiding an unbounded scan.
    stop = min(end, start + 4096)
    for candidate in range(start, stop + 1):
        if hashlib.md5(data[candidate:end]).hexdigest() == stored_md5:
            return candidate
    return None


def parse_records(data: bytes) -> list[Record]:
    records: list[Record] = []
    for match in re.finditer(re.escape(MAGIC), data):
        offset = match.start() - 4
        if offset < 0 or offset + 13 > len(data):
            continue
        length, magic, tag = struct.unpack_from("<III", data, offset)
        if magic != struct.unpack("<I", MAGIC)[0]:
            continue
        if length <= 0 or offset + length > len(data):
            continue
        name_len = data[offset + 12]
        if not 0 < name_len < 100:
            continue
        name_start = offset + 13
        name_bytes = data[name_start : name_start + name_len]
        if len(name_bytes) != name_len or not all(32 <= value < 127 for value in name_bytes):
            continue
        name = name_bytes.decode("ascii")
        cursor = name_start + name_len
        if cursor < len(data) and data[cursor] == 0:
            cursor += 1
        md5_offset = cursor
        stored_bytes = data[cursor : cursor + 32]
        if not re.fullmatch(rb"[0-9a-f]{32}", stored_bytes):
            continue
        stored_md5 = stored_bytes.decode("ascii")
        cursor += 32
        if cursor < len(data) and data[cursor] == 0:
            cursor += 1
        records.append(
            Record(
                offset=offset,
                length=length,
                tag=tag,
                name=name,
                md5_offset=md5_offset,
                stored_md5=stored_md5,
                search_start=cursor,
                payload_start=_payload_start(data, cursor, offset + length, stored_md5),
            )
        )
    return records


def record_header_checksum(data: bytes, record: Record) -> int:
    """Match firmware 0x08064178: additive byte sum, with 0xFF mapped to 0.

    search_start is immediately after the MD5 terminator, and points at the
    checksum byte, not at arbitrary padding. Exclude that byte from the sum.
    """
    assert record.offset < record.search_start < record.end <= len(data)
    checksum = sum(data[record.offset:record.search_start]) & 0xFF
    return 0 if checksum == 0xFF else checksum


def record_header_valid(data: bytes, record: Record) -> bool:
    return data[record.search_start] == record_header_checksum(data, record)


def print_analysis(path: Path) -> int:
    data = path.read_bytes()
    records = parse_records(data)
    bad = [record for record in records if record.payload_start is None]
    bad_headers = [record for record in records if not record_header_valid(data, record)]
    print(f"{path.name}")
    print(f"  bytes:   {len(data)}")
    print(f"  MD5:     {hashlib.md5(data).hexdigest()}")
    print(f"  SHA-256: {hashlib.sha256(data).hexdigest()}")
    print(f"  records: {len(records)} ({len(bad)} MD5 failures, {len(bad_headers)} header failures)")
    for record in records:
        if record.kind == "top" or record.payload_start is None or record in bad_headers:
            state = "OK" if record.payload_start is not None else "MD5 FAIL"
            header_state = "OK" if record_header_valid(data, record) else "FAIL"
            print(
                f"  {record.kind:5s} 0x{record.offset:08x} "
                f"{record.name:32s} MD5={state} HEADER={header_state}"
            )
    return len(bad) + len(bad_headers)


def _unique_record(records: list[Record], name: str, kind: str = "top") -> Record:
    found = [record for record in records if record.name == name and record.kind == kind]
    if len(found) != 1:
        raise ValueError(f"Expected one {kind} record named {name!r}, found {len(found)}")
    return found[0]


def repair_basa_animation(basa_path: Path, donor_path: Path, output_path: Path) -> None:
    require_known(basa_path)
    require_known(donor_path)
    target = bytearray(basa_path.read_bytes())
    donor = donor_path.read_bytes()
    target_records = parse_records(target)
    donor_records = parse_records(donor)
    target_outer = _unique_record(target_records, "animationLogoMazda3.pak")
    donor_outer = _unique_record(donor_records, "animationLogoMazda3.pak")
    if target_outer.length != donor_outer.length:
        raise ValueError("Animation container lengths differ")
    if target_outer.stored_md5 != donor_outer.stored_md5:
        raise ValueError("Animation container expected MD5 values differ")

    donor_inner = min(
        record.offset
        for record in donor_records
        if record.kind == "inner"
        and donor_outer.offset <= record.offset < donor_outer.end
    )
    target_inner = min(
        record.offset
        for record in target_records
        if record.kind == "inner"
        and target_outer.offset <= record.offset < target_outer.end
    )
    donor_rel = donor_inner - donor_outer.offset
    target_rel = target_inner - target_outer.offset
    if donor_rel != target_rel:
        raise ValueError("Animation container payload offsets differ")

    target[target_inner : target_outer.end] = donor[donor_inner : donor_outer.end]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(target)
    invalid = [record for record in parse_records(target) if record.payload_start is None]
    if invalid:
        raise ValueError(
            "Repaired package still has invalid records: "
            + ", ".join(record.name for record in invalid)
        )


def make_basa_hybrid(baca_path: Path, output_path: Path) -> None:
    require_known(baca_path)
    data = bytearray(baca_path.read_bytes())
    records = parse_records(data)
    system = _unique_record(records, "system.bin")
    if system.payload_start is None:
        raise ValueError("Source system.bin does not pass its stored MD5")

    old = b"#SW_MODULE#HUDBACA0"
    new = b"#SW_MODULE#HUDBASA0"
    payload = bytes(data[system.payload_start : system.end])
    if payload.count(old) != 1 or payload.count(new) != 0:
        raise ValueError("Unexpected system.bin module markers")
    patched_payload = payload.replace(old, new)
    data[system.payload_start : system.end] = patched_payload
    new_md5 = hashlib.md5(patched_payload).hexdigest().encode("ascii")
    data[system.md5_offset : system.md5_offset + 32] = new_md5

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(data)
    invalid = [record for record in parse_records(data) if record.payload_start is None]
    if invalid:
        raise ValueError(
            "Hybrid package has invalid records: "
            + ", ".join(record.name for record in invalid)
        )
    if bytes(data).count(old) != 0 or bytes(data).count(new) != 1:
        raise ValueError("Final module-marker verification failed")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    analyze = subparsers.add_parser("analyze", help="list records and verify MD5 values")
    analyze.add_argument("packages", nargs="+", type=Path)

    repair = subparsers.add_parser(
        "repair-basa", help="restore the corrupt BASA 221019 animation container"
    )
    repair.add_argument("basa", type=Path)
    repair.add_argument("donor", type=Path)
    repair.add_argument("output", type=Path)

    hybrid = subparsers.add_parser(
        "make-hybrid", help="relabel BACA 230922 system.bin as HUDBASA0 and update MD5"
    )
    hybrid.add_argument("baca", type=Path)
    hybrid.add_argument("output", type=Path)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.command == "analyze":
        invalid = 0
        for package in args.packages:
            invalid += print_analysis(package)
        return 1 if invalid else 0
    if args.command == "repair-basa":
        repair_basa_animation(args.basa, args.donor, args.output)
        print_analysis(args.output)
        return 0
    if args.command == "make-hybrid":
        make_basa_hybrid(args.baca, args.output)
        print_analysis(args.output)
        return 0
    raise AssertionError(args.command)


if __name__ == "__main__":
    raise SystemExit(main())
