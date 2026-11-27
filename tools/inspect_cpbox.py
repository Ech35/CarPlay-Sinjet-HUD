"""Read-only inspection of CPBox update images with the U5/U6A SWU layout."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from repack_u5 import parse_update


def inspect_image(path):
    image = path.read_bytes()
    description, customer, kernel, _ = parse_update(image)
    descriptor = re.sub(r"/\*.*?\*/", "", description.data.decode("utf-8"), flags=re.S)
    projects = re.findall(r'^\s*ly_project_name\s*=\s*"([A-Za-z0-9_-]+)"\s*;\s*$', descriptor, re.M)
    if len(projects) != 1:
        raise ValueError("expected one ly_project_name assignment in sw-description")
    return {
        "project": projects[0],
        "bytes": len(image),
        "sha256": sha256(image).hexdigest(),
        "swu_md5_and_cpio_crc": "verified",
        "entries": [
            {"name": entry.name, "bytes": len(entry.data), "sha256": sha256(entry.data).hexdigest()}
            for entry in (description, customer, kernel)
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="Path to the local IMG file")
    parser.add_argument("--expect-project", help="Expected project, e.g. ly2734 or ly8952")
    args = parser.parse_args()
    try:
        info = inspect_image(args.image)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Inspection failed: {error}\n")
    print(json.dumps(info, indent=2))
    if args.expect_project and info["project"] != args.expect_project:
        parser.exit(1, f"Project mismatch: expected {args.expect_project}, found {info['project']}\n")


if __name__ == "__main__":
    main()
