from __future__ import annotations

import argparse
import csv
from pathlib import Path


def parse_int(value: str) -> int:
    return int(value, 16) if value.lower().startswith("0x") else int(value)


def main() -> None:
    parser = argparse.ArgumentParser(description="Rebuild a module image from snapshot_process output.")
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("module", help="Module name from modules.tsv")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    with (args.snapshot / "modules.tsv").open("r", encoding="utf-8", newline="") as stream:
        modules = list(csv.DictReader(stream, delimiter="\t"))

    match = next((row for row in modules if row["module"].lower() == args.module.lower()), None)
    if match is None:
        raise SystemExit(f"module not found: {args.module}")

    module_base = parse_int(match["base"])
    module_size = parse_int(match["size"])
    image = bytearray(module_size)
    loaded = 0

    with (args.snapshot / "regions.tsv").open("r", encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            if parse_int(row["allocation_base"]) != module_base:
                continue

            region_base = parse_int(row["base"])
            offset = region_base - module_base
            if offset < 0 or offset >= module_size:
                continue

            data = (args.snapshot / row["file"]).read_bytes()
            size = min(len(data), module_size - offset)
            image[offset : offset + size] = data[:size]
            loaded += 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"module={args.module} base=0x{module_base:x} size=0x{module_size:x} regions={loaded}")
    print(f"output={args.output}")


if __name__ == "__main__":
    main()