from __future__ import annotations

import argparse
import csv
from pathlib import Path


def parse_int(value: str) -> int:
    return int(value, 16) if value.lower().startswith("0x") else int(value)


def main() -> None:
    parser = argparse.ArgumentParser(description="Rebuild a contiguous image from FakeHost target dump regions.")
    parser.add_argument("dump_dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    status_path = args.dump_dir / "target_status.tsv"
    regions_path = args.dump_dir / "target_regions.tsv"

    status: dict[str, str] = {}
    with status_path.open("r", encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            status[row["field"]] = row["value"]

    base = parse_int(status["allocation_base"])
    size = parse_int(status["pe_size_of_image"])
    image = bytearray(b"\0" * size)

    loaded = 0
    with regions_path.open("r", encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            region_base = parse_int(row["base"])
            region_size = parse_int(row["size"])
            off = region_base - base
            data = (args.dump_dir / row["file"]).read_bytes()
            image[off : off + min(region_size, len(data))] = data[:region_size]
            loaded += 1

    output = args.output or (args.dump_dir / "target.full_image.bin")
    output.write_bytes(image)
    print(f"base=0x{base:x}")
    print(f"size=0x{size:x}")
    print(f"regions={loaded}")
    print(f"output={output}")


if __name__ == "__main__":
    main()
