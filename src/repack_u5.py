"""Build and independently verify a local ly2734 26051317 update image."""

import os
import shutil
import argparse
from dataclasses import dataclass
import hashlib
import io
from pathlib import Path, PurePosixPath
import struct
import subprocess
import tarfile
import tempfile

from PySquashfsImage import SquashFsImage


BASE_SHA256 = "e2766acc8df294c178ab077f52a955f8d521845fb8a446d98df9ccc4bc4c9538"
SUFFIX = b"  update.swu\n"
CPIO_NAMES = ("sw-description", "customer", "kernel", "TRAILER!!!")
WORK = Path(__file__).resolve().parent
BIN = WORK / "squash-msys" / "usr" / "bin"
INVALID_XATTR = 0xFFFFFFFF


@dataclass(frozen=True)
class Entry:
    name: str
    fields: tuple[int, ...]
    data: bytes


def align(n, boundary):
    return (n + boundary - 1) & ~(boundary - 1)


def checksum(data):
    return sum(data) & 0xFFFFFFFF


def parse_update(image):
    archive = image[45:]
    if image[:45] != hashlib.md5(archive).hexdigest().encode() + SUFFIX:
        raise ValueError("SWU MD5/header verification failed")

    entries = []
    offset = 0
    for expected in CPIO_NAMES:
        hdr = archive[offset:offset + 110]
        if len(hdr) != 110 or hdr[:6] != b"070702":
            raise ValueError("invalid newc CRC entry")
        fields = tuple(int(hdr[p:p + 8], 16) for p in range(6, 110, 8))
        name_end = offset + 110 + fields[11]
        name = archive[offset + 110:name_end]
        if name != expected.encode() + b"\0":
            raise ValueError(f"unexpected CPIO entry {name!r}")
        start = align(name_end, 4)
        end = start + fields[6]
        data = archive[start:end]
        offset = align(end, 4)
        if len(data) != fields[6] or checksum(data) != fields[12]:
            raise ValueError(f"CPIO size/checksum failure in {expected}")
        if any(archive[name_end:start]) or any(archive[end:offset]):
            raise ValueError(f"nonzero CPIO padding in {expected}")
        entries.append(Entry(expected, fields, data))

    if align(offset, 512) != len(archive) or any(archive[offset:]):
        raise ValueError("CPIO trailer padding is invalid")
    return entries


def package_update(entries, customer):
    archive = bytearray()
    for entry in entries:
        data = customer if entry.name == "customer" else entry.data
        fields = list(entry.fields)
        fields[6], fields[12] = len(data), checksum(data)
        archive += b"070702" + "".join(f"{value:08X}" for value in fields).encode()
        archive += entry.name.encode() + b"\0"
        archive += b"\0" * (align(len(archive), 4) - len(archive))
        archive += data
        archive += b"\0" * (align(len(archive), 4) - len(archive))
    archive += b"\0" * (align(len(archive), 512) - len(archive))
    return hashlib.md5(archive).hexdigest().encode() + SUFFIX + archive


def metadata(entry):
    if entry.xattr != INVALID_XATTR:
        raise ValueError(f"unsupported xattrs at {entry.path}")
    node = (entry.mode, entry.uid, entry.gid, entry.time)
    if entry.is_char_device or entry.is_block_device:
        return node + (entry.major, entry.minor)
    if entry.is_dir or entry.is_file:
        return node
    raise ValueError(f"unsupported inode type at {entry.path}")


def add_tar_file(tar, info, data):
    info.size = len(data)
    tar.addfile(info, io.BytesIO(data))


def make_tar(customer, tar_path, replacements, additions, *, removals=()):
    fs = SquashFsImage.from_bytes(customer)
    expected = {}
    pseudo = []
    original_files = set()
    removals = set(removals)
    if removals & (replacements.keys() | additions.keys()):
        raise ValueError("removed files cannot also be replaced or added")
    removed = set()
    with tarfile.open(tar_path, "w", format=tarfile.PAX_FORMAT) as tar:
        for entry in fs:
            path = entry.path.lstrip("/") or "."
            if path in removals:
                if not entry.is_file:
                    raise ValueError(f"removal is not a regular file: {path}")
                removed.add(path)
                continue
            info = tarfile.TarInfo(path)
            info.mode = entry.mode & 0o7777
            info.uid, info.gid, info.mtime = entry.uid, entry.gid, entry.time
            data = None
            if entry.is_dir:
                info.type = tarfile.DIRTYPE
            elif entry.is_char_device or entry.is_block_device:
                kind = "C" if entry.is_char_device else "B"
                pseudo.extend(("-p", f"{path} {kind} {entry.time} {info.mode:o} "
                                     f"{entry.uid} {entry.gid} {entry.major} {entry.minor}"))
            elif entry.is_file:
                original_files.add(path)
                data = replacements.get(path, entry.read_bytes())
            node = metadata(entry)
            if info.type == tarfile.DIRTYPE and path != ".":
                tar.addfile(info)
            elif data is not None:
                add_tar_file(tar, info, data)
                node += (hashlib.sha256(data).hexdigest(),)
            expected[path] = node
        if missing := replacements.keys() - original_files:
            raise ValueError(f"replacements are not original regular files: {missing}")
        if missing := removals - removed:
            raise ValueError(f"removed files were not found: {missing}")
        for path, (data, mode) in additions.items():
            if path in expected:
                raise ValueError(f"addition conflicts with existing path: {path}")
            parent = expected.get(str(PurePosixPath(path).parent))
            if not parent or parent[0] & 0o170000 != 0o040000:
                raise ValueError(f"parent missing for added file: {path}")
            info = tarfile.TarInfo(path)
            info.mode, info.uid, info.gid, info.mtime = mode, 0, 0, fs._root.time
            add_tar_file(tar, info, data)
            expected[path] = (0o100000 | mode, 0, 0, info.mtime, hashlib.sha256(data).hexdigest())
    return expected, pseudo


def verify_fs(path, expected):
    actual = {}
    for entry in SquashFsImage.from_bytes(path.read_bytes()):
        node = metadata(entry)
        if entry.is_file:
            node += (hashlib.sha256(entry.read_bytes()).hexdigest(),)
        actual[entry.path.lstrip("/") or "."] = node
    if actual != expected:
        differences = [path for path in actual.keys() | expected.keys()
                       if actual.get(path) != expected.get(path)]
        raise ValueError(f"SquashFS metadata/content differs: {sorted(differences)}")


def rebuild_squashfs(original, input_tar, new_fs, expected, pseudo):
    mkfs_time = struct.unpack_from("<I", original, 8)[0]
    root_mode, root_uid, root_gid, root_mtime = expected["."]
    opts = [os.environ.get("SQFSTAR") or shutil.which("sqfstar") or str(BIN / "sqfstar.exe"), "-comp", "xz", "-b", "262144",
            "-mkfs-time", str(mkfs_time), "-root-mode", f"{root_mode & 0o7777:o}",
            "-root-uid", str(root_uid), "-root-gid", str(root_gid),
            "-root-time", str(root_mtime), "-processors", "2", "-no-progress",
            "-quiet", "-exports", "-nopad", *pseudo]

    # Prefer the stock fragment policy. Tail-end fragments use the same on-device
    # decompression path already required by the stock image's small-file fragments.
    for fragment_opts in (["-no-tailends"], []):
        new_fs.unlink(missing_ok=True)
        with input_tar.open("rb") as stream:
            subprocess.run([*opts, *fragment_opts, str(new_fs)], stdin=stream,
                           check=True, cwd=WORK)
        rebuilt = new_fs.read_bytes()
        if len(rebuilt) <= len(original):
            verify_fs(new_fs, expected)
            flags = struct.unpack_from("<H", rebuilt, 24)[0]
            print(f"SquashFS XZ 256KiB, NFS export, flags 0x{flags:04x}"
                  f" ({'stock' if fragment_opts else 'tail-end'} fragment policy)")
            return rebuilt

    raise ValueError(f"customer partition image grew: {len(original)} -> {len(rebuilt)}")


def mappings(values, added=False):
    result = {}
    for value in values:
        dest, source = value.split("=", 1)
        mode = 0o755
        if added and ":" in dest:
            dest, mode_str = dest.rsplit(":", 1)
            mode = int(mode_str, 8)
            if not 0 <= mode <= 0o777:
                raise ValueError(f"invalid mode: {mode_str}")
        path = PurePosixPath(dest)
        if (not dest or dest == "." or path.is_absolute() or ".." in path.parts or "\\" in dest
                or path.as_posix() != dest):
            raise ValueError(f"invalid customer path: {dest}")
        if dest in result:
            raise ValueError(f"duplicate customer path: {dest}")
        data = Path(source).read_bytes()
        result[dest] = (data, mode) if added else data
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--replace", metavar="PATH=FILE", default=[], action="append")
    parser.add_argument("--add", metavar="PATH[:MODE]=FILE", default=[], action="append")
    args = parser.parse_args()
    if args.output.resolve() == args.base.resolve():
        raise ValueError("output would overwrite the stock image")
    base = args.base.read_bytes()
    if hashlib.sha256(base).hexdigest() != BASE_SHA256:
        raise ValueError("base is not the verified ly2734 26051317 release image")
    entries = parse_update(base)
    original = entries[1].data
    replacements, additions = mappings(args.replace), mappings(args.add, True)
    for key in replacements.keys() & additions.keys():
        raise ValueError(f"same path both replaced and added: {key}")

    with tempfile.TemporaryDirectory(prefix="squash-u5-", dir=WORK) as tmp:
        tmp = Path(tmp)
        input_tar = tmp / "customer.tar"
        new_fs = tmp / "customer"
        expected, pseudo = make_tar(original, input_tar, replacements, additions)
        rebuilt = rebuild_squashfs(original, input_tar, new_fs, expected, pseudo)
        # Keep the original partition payload extent and kernel CPIO offset.
        customer = rebuilt.ljust(len(original), b"\0")
        candidate = package_update(entries, customer)
        validated = parse_update(candidate)
        for src, dst in zip(entries, validated):
            if src.name != "customer" and src != dst:
                raise ValueError(f"unexpected change to {src.name}")
            if src.fields[:6] != dst.fields[:6] or src.fields[7:12] != dst.fields[7:12]:
                raise ValueError(f"unexpected header metadata change to {src.name}")
        args.output.write_bytes(candidate)
    print(f"Verified {len(expected)} SquashFS paths, unchanged SWU descriptor and kernel")
    print(f"customer rebuilt to {len(rebuilt)} bytes, zero padded to stock {len(original)} bytes")
    print("CPIO offsets, CRC checksums, SWU MD5, and total stock image size verified")
    print(f"SHA256 {hashlib.sha256(candidate).hexdigest()}  {args.output}")


if __name__ == "__main__":
    main()
